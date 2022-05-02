/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem.hpp>
#include <opencv2/core.hpp>

#define kDefaultIspConfigFilename "isp.json"

namespace fb360_dep {

// forward declarations
class CameraIsp;
enum class DemosaicFilter : unsigned int;

// DemosaicFilter::BILINEAR
extern const DemosaicFilter kDefaultDemosaicFilterForRawToRgb;

/* Loads the ISP from the given file, configured with the given runtime options */
std::unique_ptr<CameraIsp> cameraIspFromConfigFileWithOptions(
    const boost::filesystem::path& configFilename,
    int pow2DownscaleFactor = 1,
    DemosaicFilter demosaicFilter = kDefaultDemosaicFilterForRawToRgb,
    bool applyToneCurve = true);

/* Converts the given raw image data to RGB via the given ISP */
template <typename T>
cv::Mat_<cv::Vec<T, 3>> rawToRgb(const std::vector<T>& rawImage, CameraIsp& cameraIsp);

/* Converts the given raw image data to RGB. If no config filename is specified, the ISP config is
   loaded from the default location, <image_directory>/kDefaultIspConfigFilename */
template <typename T>
cv::Mat_<cv::Vec<T, 3>> rawToRgb(
    const boost::filesystem::path& rawImageFilename,
    const boost::filesystem::path& ispConfigFilename = "",
    int pow2DownscaleFactor = 1,
    DemosaicFilter demosaicFilter = kDefaultDemosaicFilterForRawToRgb,
    bool applyToneCurve = true);

/* Same as above, except that here, output precision is determined at runtime and matches the raw
   images's precision */
cv::Mat rawToRgb(
    const boost::filesystem::path& rawImageFilename,
    const boost::filesystem::path& ispConfigFilename = "",
    int pow2DownscaleFactor = 1,
    DemosaicFilter demosaicFilter = kDefaultDemosaicFilterForRawToRgb,
    bool applyToneCurve = true);

/* Writes a DNG file for the given raw image and ISP config files. If no config filename is
   specified, the ISP config is loaded from the default location,
   <image_directory>/DEFAULT_ISP_CONFIG_FILENAME */
template <typename T>
bool writeDng(
    const boost::filesystem::path& rawImageFilename,
    const boost::filesystem::path& outputFilename,
    const boost::filesystem::path& ispConfigFilename = "");

/* Same as above, except that here, precision of the written DNG is determined at runtime and
matches the raw image's precision */
bool writeDng(
    const boost::filesystem::path& rawImageFilename,
    const boost::filesystem::path& outputFilename,
    const boost::filesystem::path& ispConfigFilename = "");

}; // namespace fb360_dep
