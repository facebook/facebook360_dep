/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <opencv2/core/core.hpp>

#include "source/util/Camera.h"
#include "source/util/ThreadPool.h"

namespace fb360_dep {

template <typename T>
cv::Mat_<cv::Vec4f> disparityColor(
    const cv::Mat_<float>& disparity,
    Camera camera,
    const Eigen::Vector3f& position,
    const T& functor) {
  // disparity is relative to the camera's position
  // recompute relative to position
  camera = camera.rescale({disparity.cols, disparity.rows});
  cv::Mat_<cv::Vec4f> color(disparity.rows, disparity.cols);
  for (int y = 0; y < disparity.rows; ++y) {
    for (int x = 0; x < disparity.cols; ++x) {
      float distance = 1.0 / disparity(y, x);
      const Eigen::Vector3f world = camera.rig({x + 0.5, y + 0.5}, distance).cast<float>();
      color(y, x) = functor((world - position).norm());
    }
  }
  return color;
}

template <typename T>
std::vector<cv::Mat_<cv::Vec4f>> disparityColors(
    const Camera::Rig& cameras,
    const std::vector<cv::Mat_<float>>& disparities,
    const Eigen::Vector3f& position,
    const T& functor) {
  std::vector<cv::Mat_<cv::Vec4f>> colors(ssize(cameras));
  ThreadPool threads;
  for (ssize_t i = 0; i < ssize(cameras); ++i) {
    threads.spawn(
        [&, i] { colors[i] = disparityColor(disparities[i], cameras[i], position, functor); });
  }
  threads.join();
  return colors;
}

inline cv::Vec4f metersToGrayscale(float meters) {
  float disparity = 1 / meters;
  return {disparity, disparity, disparity, 1};
}

} // namespace fb360_dep
