/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/calibration/Keypoint.h"
#include "source/util/Camera.h"

DECLARE_bool(enable_timing);
DECLARE_int32(threads);
DECLARE_bool(use_nearest);
DECLARE_double(match_score_threshold);

namespace fb360_dep::calibration {

bool getNextDepthSample(
    int& currentDepthSample,
    double& currentDisparity,
    cv::Rect2f& currentBox,
    const Camera& camera0,
    const Camera::Vector2& corner0Coords,
    const Camera& camera1);

Overlap findMatches(
    const cv::Mat_<uint8_t>& img0,
    const std::vector<Keypoint>& corners0,
    const Camera& camera0,
    const cv::Mat_<uint8_t>& img1,
    const std::vector<Keypoint>& corners1,
    const Camera& camera1);

std::vector<Overlap> findAllMatches(
    const Camera::Rig& rig,
    const std::vector<cv::Mat_<uint8_t>>& images,
    const std::map<std::string, std::vector<Keypoint>>& allCorners);

} // namespace fb360_dep::calibration
