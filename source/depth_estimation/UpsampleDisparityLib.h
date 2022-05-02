/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <opencv2/core.hpp>
#include <opencv2/core/types.hpp>

#include "source/util/Camera.h"
#include "source/util/ImageUtil.h"

namespace fb360_dep {
namespace depth_estimation {

int getRadius(const cv::Size& size, const cv::Size& sizeUp);

std::vector<cv::Mat_<float>> upsampleDisparities(
    const Camera::Rig& rig,
    const std::vector<cv::Mat_<float>>& disps,
    const std::vector<cv::Mat_<float>>& bgDispsUp,
    const std::vector<cv::Mat_<bool>>& masks,
    const std::vector<cv::Mat_<bool>>& masksUpIn,
    const cv::Size& sizeUp,
    const bool useForegroundMasks,
    const int threads = -1);

} // namespace depth_estimation
} // namespace fb360_dep
