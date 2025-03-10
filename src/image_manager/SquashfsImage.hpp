/*
 * Sarus
 *
 * Copyright (c) 2018-2021, ETH Zurich. All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef sarus_image_manger_SquashfsImage_hpp
#define sarus_image_manger_SquashfsImage_hpp

#include <boost/filesystem.hpp>

#include "common/Config.hpp"


namespace sarus {
namespace image_manager {

/**
 * This class builds and represents the squashfs image.
 */
class SquashfsImage {
public:
    SquashfsImage(  const common::Config& config,
                    const boost::filesystem::path& expandedImage,
                    const boost::filesystem::path& pathOfImage);
    boost::filesystem::path getPathOfImage() const;

private:
    void log(const boost::format &message, common::LogLevel level) const;

private:
    boost::filesystem::path pathOfImage;
};

}
}

#endif
