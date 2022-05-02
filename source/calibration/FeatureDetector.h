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
DECLARE_bool(log_verbose);
DECLARE_int32(octave_count);
DECLARE_bool(same_scale);
DECLARE_int32(threads);

namespace fb360_dep {
namespace calibration {

std::vector<Keypoint>
findCorners(const Camera& camera, const cv::Mat_<uint8_t>& image, const bool useNearest);

std::vector<Camera::Vector2> findScaledCorners(
    const double scale,
    const cv::Mat_<uint8_t>& imageFull,
    const cv::Mat_<uint8_t>& maskFull,
    const std::string& cameraId = "");

std::map<std::string, std::vector<Keypoint>> findAllCorners(
    const Camera::Rig& rig,
    const std::vector<cv::Mat_<uint8_t>>& images,
    const bool useNearest);

} // namespace calibration
} // namespace fb360_dep
