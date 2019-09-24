/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <opencv2/imgproc.hpp>

#include "source/util/MathUtil.h"

namespace fb360_dep {
namespace color {

static float kRgb2yuvData[9] =
    {0.299f, 0.587f, 0.114f, -0.14713f, -0.28886f, 0.436f, 0.615f, -0.51499f, -0.10001f};

static cv::Mat_<float> rgb2yuv(3, 3, kRgb2yuvData);

static float kYuv2rgbData[9] =
    {1.0f, 0.0f, 1.13983f, 1.0f, -0.39465f, -0.58060f, 1.0f, 2.03211f, 0.0f};

static cv::Mat_<float> yuv2rgb(3, 3, kYuv2rgbData);

} // end namespace color
} // end namespace fb360_dep
