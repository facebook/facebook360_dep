/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <opencv2/opencv.hpp>

#include "source/util/Camera.h"

namespace fb360_dep {
namespace point_cloud_util {

struct BGRPoint {
  Camera::Vector3 coords;
  cv::Vec3b bgrColor;
};

using PointCloud = std::vector<BGRPoint>;

struct PointCloudProjection {
  cv::Mat_<cv::Vec3b> image;
  cv::Mat_<float> disparityImage;
  cv::Mat_<cv::Point3f> coordinateImage;
};

std::vector<PointCloudProjection> generateProjectedImages(
    const PointCloud& pointCloud,
    const Camera::Rig& rig);
int getPointCount(const std::string& pointCloudFile);
PointCloud
extractPoints(const std::string& pointCloudFile, const int pointCount, const int maxThreads);
PointCloud extractPoints(const std::string& pointCloudFile, const int maxThreads);

} // namespace point_cloud_util
} // namespace fb360_dep
