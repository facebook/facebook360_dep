/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <opencv2/opencv.hpp>

#include <folly/dynamic.h>

#include "source/calibration/Keypoint.h"
#include "source/util/Camera.h"

namespace fb360_dep {
namespace calibration {

cv::Mat_<uint8_t> extractSingleChannelImage(const cv::Mat_<cv::Vec3b>& image);

std::vector<cv::Mat_<uint8_t>> loadChannels(const Camera::Rig& rig);

} // namespace calibration
} // namespace fb360_dep
