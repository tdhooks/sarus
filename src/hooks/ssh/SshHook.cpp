/*
 * Sarus
 *
 * Copyright (c) 2018-2021, ETH Zurich. All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "SshHook.hpp"

#include <fstream>
#include <cstdlib>
#include <tuple>
#include <sstream>
#include <grp.h>
#include <sys/prctl.h>
#include <boost/format.hpp>
#include <boost/regex.hpp>

#include "common/Error.hpp"
#include "common/Utility.hpp"
#include "common/Lockfile.hpp"
#include "common/PasswdDB.hpp"
#include "runtime/mount_utilities.hpp"
#include "hooks/common/Utility.hpp"


namespace sarus {
namespace hooks {
namespace ssh {

void SshHook::generateSshKeys(bool overwriteSshKeysIfExist) {
    log("Generating SSH keys", sarus::common::LogLevel::INFO);

    uidOfUser = getuid(); // keygen command is executed with user identity
    gidOfUser = getgid();
    username = getUsername(uidOfUser);
    sshKeysDirInHost = getSshKeysDirInHost(username);
    dropbearDirInHost = sarus::common::getEnvironmentVariable("DROPBEAR_DIR");

    sarus::common::createFoldersIfNecessary(sshKeysDirInHost);
    sarus::common::Lockfile lock{ sshKeysDirInHost }; // protect keys from concurrent writes

    if(userHasSshKeys() && !overwriteSshKeysIfExist) {
        auto message = boost::format("SSH keys not generated because they already exist in %s."
                                     " Use the '--overwrite' option to overwrite the existing keys.")
                        % sshKeysDirInHost;
        log(message, sarus::common::LogLevel::GENERAL);
        return;
    }

    boost::filesystem::remove_all(sshKeysDirInHost);
    sarus::common::createFoldersIfNecessary(sshKeysDirInHost);
    sshKeygen(sshKeysDirInHost / "dropbear_ecdsa_host_key");
    sshKeygen(sshKeysDirInHost / "id_dropbear");
    generateAuthorizedKeys(sshKeysDirInHost / "id_dropbear", sshKeysDirInHost / "authorized_keys");

    log("Successfully generated SSH keys", sarus::common::LogLevel::GENERAL);
    log("Successfully generated SSH keys", sarus::common::LogLevel::INFO);
}

void SshHook::checkUserHasSshKeys() {
    log("Checking that user has SSH keys", sarus::common::LogLevel::INFO);

    uidOfUser = getuid(); // "user-has-ssh-keys" command is executed with user identity
    gidOfUser = getgid();
    username = getUsername(uidOfUser);
    sshKeysDirInHost = getSshKeysDirInHost(username);

    if(!userHasSshKeys()) {
        log(boost::format{"Could not find SSH keys in %s"} % sshKeysDirInHost, sarus::common::LogLevel::INFO);
        exit(EXIT_FAILURE); // exit with non-zero to communicate missing keys to calling process
    }

    log("Successfully checked that user has SSH keys", sarus::common::LogLevel::INFO);
}

void SshHook::startSshDaemon() {
    log("Activating SSH in container", sarus::common::LogLevel::INFO);

    dropbearRelativeDirInContainer = boost::filesystem::path("/opt/oci-hooks/dropbear");
    dropbearDirInHost = sarus::common::getEnvironmentVariable("DROPBEAR_DIR");
    serverPort = std::stoi(sarus::common::getEnvironmentVariable("SERVER_PORT"));
    std::tie(bundleDir, pidOfContainer) = hooks::common::utility::parseStateOfContainerFromStdin();
    hooks::common::utility::enterNamespacesOfProcess(pidOfContainer);
    parseConfigJSONOfBundle();
    username = getUsername(uidOfUser);
    sshKeysDirInHost = getSshKeysDirInHost(username);
    sshKeysDirInContainer = getSshKeysDirInContainer();
    copyDropbearIntoContainer();
    setupSshKeysDirInContainer();
    copySshKeysIntoContainer();
    patchPasswdIfNecessary();
    createEnvironmentFile();
    createEtcProfileModule();
    startSshDaemonInContainer();
    createSshExecutableInContainer();

    log("Successfully activated SSH in container", sarus::common::LogLevel::INFO);
}

void SshHook::parseConfigJSONOfBundle() {
    log("Parsing bundle's config.json", sarus::common::LogLevel::INFO);

    auto json = sarus::common::readJSON(bundleDir / "config.json");

    hooks::common::utility::applyLoggingConfigIfAvailable(json);

    // get rootfs
    auto root = boost::filesystem::path{ json["root"]["path"].GetString() };
    if(root.is_absolute()) {
        rootfsDir = root;
    }
    else {
        rootfsDir = bundleDir / root;
    }

    dropbearDirInContainer = rootfsDir / dropbearRelativeDirInContainer;

    // get uid + gid of user
    uidOfUser = json["process"]["user"]["uid"].GetInt();
    gidOfUser = json["process"]["user"]["gid"].GetInt();

    log("Successfully parsed bundle's config.json", sarus::common::LogLevel::INFO);
}

bool SshHook::userHasSshKeys() const {
    auto expectedKeyFiles = std::vector<std::string>{"dropbear_ecdsa_host_key", "id_dropbear", "authorized_keys"};
    for(const auto& file : expectedKeyFiles) {
        auto fullPath = sshKeysDirInHost / file;
        if(!boost::filesystem::exists(fullPath)) {
            auto message = boost::format{"Expected SSH key file %s not found"} % fullPath;
            log(message, sarus::common::LogLevel::DEBUG);
            return false;
        }
    }
    log(boost::format{"Found SSH keys in %s"} % sshKeysDirInHost, sarus::common::LogLevel::DEBUG);
    return true;
}

std::string SshHook::getUsername(uid_t uid) const {
    auto passwdFile = boost::filesystem::path{ sarus::common::getEnvironmentVariable("PASSWD_FILE") };
    return sarus::common::PasswdDB{passwdFile}.getUsername(uidOfUser);
}

boost::filesystem::path SshHook::getSshKeysDirInHost(const std::string& username) const {
    auto baseDir = boost::filesystem::path{ sarus::common::getEnvironmentVariable("HOOK_BASE_DIR") };
    return baseDir / username / ".oci-hooks/ssh/keys";
}

boost::filesystem::path SshHook::getSshKeysDirInContainer() const {
    // For an explanation of the logic within this function please consult
    // https://sarus.readthedocs.io/en/latest/developer/ssh.html#determining-the-location-of-the-ssh-keys-inside-the-container
    auto homeDirectory = sarus::common::PasswdDB{rootfsDir / "etc/passwd"}.getHomeDirectory(uidOfUser);

    if (homeDirectory.empty() || homeDirectory == boost::filesystem::path{"/nonexistent"}) {
        log(boost::format("SSH Hook: Found invalid home directory in container's /etc/passwd for user %s (%d): \"%s\"")
            % username % uidOfUser % homeDirectory,
            sarus::common::LogLevel::GENERAL);
        exit(EXIT_FAILURE);
    }

    auto sshKeysFullPath = rootfsDir / homeDirectory / ".ssh";
    log(boost::format("Setting SSH keys directory in container to %s") % sshKeysFullPath,
        sarus::common::LogLevel::DEBUG);
    return sshKeysFullPath;
}

void SshHook::sshKeygen(const boost::filesystem::path& outputFile) const {
    log(boost::format{"Generating %s"} % outputFile, sarus::common::LogLevel::INFO);
    auto command = boost::format{"%s/bin/dropbearkey -t ecdsa -f %s"}
        % dropbearDirInHost.string()
        % outputFile.string();
    sarus::common::executeCommand(command.str());
}

void SshHook::generateAuthorizedKeys(const boost::filesystem::path& userKeyFile,
                                     const boost::filesystem::path& authorizedKeysFile) const {
    log(boost::format{"Generating \"authorized_keys\" file (%s)"} % authorizedKeysFile,
        sarus::common::LogLevel::INFO);

    // output user's public key
    auto command = boost::format{"%s/bin/dropbearkey -y -f %s"}
        % dropbearDirInHost.string()
        % (sshKeysDirInHost / "id_dropbear").string();
    auto output = sarus::common::executeCommand(command.str());

    // extract public key
    auto ss = std::stringstream{ output };
    auto line = std::string{};
    auto matches = boost::smatch{};
    auto re = boost::regex{"^(ecdsa-.*)$"};

    // write public key to "authorized_keys" file
    while(std::getline(ss, line)) {
        if(boost::regex_match(line, matches, re)) {
            auto ofs = std::ofstream{ (sshKeysDirInHost / "authorized_keys").c_str() };
            ofs << matches[1] << std::endl;
            ofs.close();
            log("Successfully generated \"authorized_keys\" file", sarus::common::LogLevel::INFO);
            return;
        }
    }

    auto message = boost::format("Failed to parse key from %s") % userKeyFile;
    SARUS_THROW_ERROR(message.str());
}

void SshHook::copyDropbearIntoContainer() const {
    log(boost::format("Copying Dropbear binaries into container under %s") % dropbearDirInContainer,
        sarus::common::LogLevel::INFO);

    sarus::common::copyFile(dropbearDirInHost / "bin/dbclient", dropbearDirInContainer / "bin/dbclient");
    sarus::common::copyFile(dropbearDirInHost / "bin/dropbear", dropbearDirInContainer / "bin/dropbear");

    log("Successfully copied Dropbear binaries into container", sarus::common::LogLevel::INFO);
}

void SshHook::setupSshKeysDirInContainer() const {
    log(boost::format("Setting up directory for SSH keys into container under %s") % sshKeysDirInContainer,
        sarus::common::LogLevel::INFO);

    auto rootIdentity = sarus::common::UserIdentity{};
    auto userIdentity = sarus::common::UserIdentity(uidOfUser, gidOfUser, {});

    // switch to unprivileged user to make sure that the user has the
    // permission to create a new folder ~/.ssh in the container
    sarus::common::switchIdentity(userIdentity);
    sarus::common::createFoldersIfNecessary(sshKeysDirInContainer);
    sarus::common::switchIdentity(rootIdentity);

    // mount overlayfs on top of the container's ~/.ssh, otherwise we
    // could mess up with the host's ~/.ssh directory. E.g. when the user
    // bind mounts the host's /home into the container
    auto lowerDir = bundleDir / "overlay/ssh-lower";
    auto upperDir = bundleDir / "overlay/ssh-upper";
    auto workDir = bundleDir / "overlay/ssh-work";
    sarus::common::createFoldersIfNecessary(lowerDir);
    sarus::common::createFoldersIfNecessary(upperDir, uidOfUser, gidOfUser);
    sarus::common::createFoldersIfNecessary(workDir);
    runtime::mountOverlayfs(lowerDir, upperDir, workDir, sshKeysDirInContainer);

    log("Successfully set up directory for SSH keys into container", sarus::common::LogLevel::INFO);
}

void SshHook::copySshKeysIntoContainer() const {
    log("Copying SSH keys into container", sarus::common::LogLevel::INFO);

    // server keys
    sarus::common::copyFile(sshKeysDirInHost / "dropbear_ecdsa_host_key",
                            sshKeysDirInContainer / "dropbear_ecdsa_host_key",
                            uidOfUser, gidOfUser);

    // client keys
    sarus::common::copyFile(sshKeysDirInHost / "id_dropbear",
                            sshKeysDirInContainer / "id_dropbear",
                            uidOfUser, gidOfUser);
    sarus::common::copyFile(sshKeysDirInHost / "authorized_keys",
                            sshKeysDirInContainer / "authorized_keys",
                            uidOfUser, gidOfUser);

    log("Successfully copied SSH keys into container", sarus::common::LogLevel::INFO);
}

void SshHook::createSshExecutableInContainer() const {
    log("Creating ssh binary (shell script) in container", sarus::common::LogLevel::INFO);

    boost::filesystem::remove_all(rootfsDir / "usr/bin/ssh");

    // create file
    auto ofs = std::ofstream((rootfsDir / "usr/bin/ssh").c_str());
    ofs << "#!/bin/sh" << std::endl
        << dropbearRelativeDirInContainer.string() << "/bin/dbclient -y -p " << serverPort << " $*" << std::endl;
    ofs.close();

    // set permissions
    boost::filesystem::permissions(rootfsDir / "usr/bin/ssh",
        boost::filesystem::owner_all |
        boost::filesystem::group_read | boost::filesystem::group_exe |
        boost::filesystem::others_read | boost::filesystem::others_exe);

    log("Successfully created ssh binary in container", sarus::common::LogLevel::INFO);
}

void SshHook::patchPasswdIfNecessary() const {
    log("Patching container's /etc/passwd if necessary"
        " (ensure that command interpreter is valid)", sarus::common::LogLevel::INFO);

    auto passwd = sarus::common::PasswdDB{rootfsDir / "etc/passwd"};
    for(auto& entry : passwd.getEntries()) {
        if( entry.userCommandInterpreter
            && !boost::filesystem::exists(rootfsDir / *entry.userCommandInterpreter)) {
            entry.userCommandInterpreter = "/bin/sh";
        }
    }
    passwd.write(rootfsDir / "etc/passwd");

    log("Successfully patched container's /etc/passwd", sarus::common::LogLevel::INFO);
}

void SshHook::createEnvironmentFile() const {
    log(boost::format("Creating script to export container environment upon login in %s") %
        (dropbearDirInContainer / "environment"),
        sarus::common::LogLevel::INFO);

    // create file
    auto containerEnvironment = hooks::common::utility::parseEnvironmentVariablesFromOCIBundle(bundleDir);
    auto ofs = std::ofstream((dropbearDirInContainer / "environment").c_str());
    ofs << "#!/bin/sh" << std::endl;
    for (const auto& variable : containerEnvironment) {
        ofs << "export " << variable.first << "=\"" << variable.second << "\"" << std::endl;
    }
    ofs.close();

    // set permissions
    boost::filesystem::permissions(dropbearDirInContainer / "environment",
        boost::filesystem::owner_all |
        boost::filesystem::group_read |
        boost::filesystem::others_read );

    log("Successfully created script to export container environment upon login", sarus::common::LogLevel::INFO);
}

void SshHook::createEtcProfileModule() const {
    log("Creating module in container's /etc/profile.d", sarus::common::LogLevel::INFO);

    // create file
    auto ofs = std::ofstream((rootfsDir / "etc/profile.d/ssh-hook.sh").c_str());
    ofs << "#!/bin/sh" << std::endl
        << "if [ \"$SSH_CONNECTION\" ]; then" << std::endl
        << "    . " << dropbearRelativeDirInContainer.string() << "/environment" << std::endl
        << "fi" << std::endl;
    ofs.close();

    // set permissions
    boost::filesystem::permissions(rootfsDir / "etc/profile.d/ssh-hook.sh",
        boost::filesystem::owner_read | boost::filesystem::owner_write |
        boost::filesystem::group_read |
        boost::filesystem::others_read);

    log("Successfully created module in container's /etc/profile.d", sarus::common::LogLevel::INFO);
}

void SshHook::startSshDaemonInContainer() const {
    log("Starting SSH daemon in container", sarus::common::LogLevel::INFO);

    std::function<void()> preExecActions = [this]() {
        if(chroot(rootfsDir.c_str()) != 0) {
            auto message = boost::format("Failed to chroot to %s: %s")
                % rootfsDir % strerror(errno);
            SARUS_THROW_ERROR(message.str());
        }

        // drop all capabilities
        // go through capability zero, one, two, ... until prctl() fails
        // because we went beyond the last valid capability
        for(int capIdx = 0; ; capIdx++) {
            if (prctl(PR_CAPBSET_DROP, capIdx, 0, 0, 0) != 0) {
                if(errno == EINVAL) {
                    break; // reached end of valid capabilities
                }
                else {
                    auto message = boost::format("Failed to prctl(PR_CAPBSET_DROP, %d, 0, 0, 0): %s")
                        % capIdx % strerror(errno);
                    SARUS_THROW_ERROR(message.str());
                }
            }
        }

        // drop supplementary groups (if any)
        if(setgroups(0, NULL) != 0) {
            auto message = boost::format("Failed to setgroups(0, NULL): %s") % strerror(errno);
            SARUS_THROW_ERROR(message.str());
        }

        // change to user's gid
        if(setresgid(gidOfUser, gidOfUser, gidOfUser) != 0) {
            auto message = boost::format("Failed to setresgid(%1%, %1%, %1%): %2%") % gidOfUser % strerror(errno);
            SARUS_THROW_ERROR(message.str());
        }

        // change to user's uid
        if(setresuid(uidOfUser, uidOfUser, uidOfUser) != 0) {
            auto message = boost::format("Failed to setresuid(%1%, %1%, %1%): %2%") % uidOfUser % strerror(errno);
            SARUS_THROW_ERROR(message.str());
        }

        // set NoNewPrivs
        if(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
            auto message = boost::format("Failed to prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0): %s") % strerror(errno);
            SARUS_THROW_ERROR(message.str());
        }
    };

    auto sshKeysPathWithinContainer = "/" / boost::filesystem::relative(sshKeysDirInContainer, rootfsDir);

    auto dropbearCommand = sarus::common::CLIArguments{
        dropbearRelativeDirInContainer.string() + "/bin/dropbear",
        "-E",
        "-r", sshKeysPathWithinContainer.string() + "/dropbear_ecdsa_host_key",
        "-p", std::to_string(serverPort)
    };
    auto status = sarus::common::forkExecWait(dropbearCommand, preExecActions);
    if(status != 0) {
        auto message = boost::format("%s/bin/dropbear exited with status %d")
            % dropbearRelativeDirInContainer % status;
        SARUS_THROW_ERROR(message.str());
    }

    log("Successfully started SSH daemon in container", sarus::common::LogLevel::INFO);
}

void SshHook::log(const boost::format& message, sarus::common::LogLevel level) const {
    log(message.str(), level);
}

void SshHook::log(const std::string& message, sarus::common::LogLevel level) const {
    auto subsystemName = "SSH hook";
    sarus::common::Logger::getInstance().log(message, subsystemName, level);
}

}}} // namespace
