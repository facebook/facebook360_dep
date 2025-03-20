/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Format.h>

#include "source/util/CvUtil.h"
#include "source/util/ThreadPool.h"

namespace fb360_dep::background_subtraction {

template <typename T, typename U>
cv::Mat_<bool> generateForegroundMask(
    const cv::Mat_<T>& templateColor,
    const cv::Mat_<T>& frameColor,
    const int blurRadius,
    const float threshold,
    const int morphClosingRadius) {
  CHECK_EQ(templateColor.size(), frameColor.size());

  // Blur images to reduce noise
  const cv::Mat_<T> templateBlurred =
      blurRadius > 0 ? cv_util::gaussianBlur(templateColor, blurRadius) : templateColor;
  const cv::Mat_<T> frameBlurred =
      blurRadius > 0 ? cv_util::gaussianBlur(frameColor, blurRadius) : frameColor;

  const cv::Mat_<U> templateFloat = cv_util::convertTo<float>(templateBlurred);
  const cv::Mat_<U> frameFloat = cv_util::convertTo<float>(frameBlurred);

  // mask = ||template - frame||^2 > threshold
  cv::Mat_<U> imageDiff;
  cv::absdiff(templateFloat, frameFloat, imageDiff);
  cv::Mat_<cv::Vec3f> imageDiffNoAlpha = cv_util::removeAlpha(imageDiff);
  cv::Mat_<bool> foregroundMask(imageDiff.size(), false);
  for (int y = 0; y < imageDiffNoAlpha.rows; ++y) {
    for (int x = 0; x < imageDiffNoAlpha.cols; ++x) {
      foregroundMask(y, x) = cv::norm(imageDiffNoAlpha(y, x)) > threshold;
    }
  }

  // Fill holes
  if (morphClosingRadius > 0) {
    const cv::Size kElementSize = cv::Size(morphClosingRadius, morphClosingRadius);
    cv::Mat element = cv::getStructuringElement(cv::MORPH_RECT, kElementSize);
    cv::morphologyEx(foregroundMask, foregroundMask, cv::MORPH_CLOSE, element);
  }

  const int count = cv::countNonZero(foregroundMask);
  const float fgPct = 100.0f * count / (foregroundMask.cols * foregroundMask.rows);
  LOG(INFO) << std::fixed << std::setprecision(2)
            << folly::sformat("foreground amount: {}%", fgPct);
  return foregroundMask;
}

template <typename T, typename U>
std::vector<cv::Mat_<bool>> generateForegroundMasks(
    const std::vector<cv::Mat_<T>>& templateColors,
    const std::vector<cv::Mat_<T>>& frameColors,
    const cv::Size& size,
    const int blurRadius,
    const float threshold,
    const int morphClosingRadius,
    const int numThreads = -1) {
  CHECK_GT(frameColors.size(), 0);
  const cv::Mat_<bool> allPass(size, true);
  std::vector<cv::Mat_<bool>> masks(templateColors.size());
  ThreadPool threadPool(numThreads);
  for (int i = 0; i < int(templateColors.size()); ++i) {
    threadPool.spawn([&, i] {
      LOG(INFO) << folly::sformat("{} of {}...", i + 1, templateColors.size());
      masks[i] = templateColors[i].empty()
          ? allPass
          : generateForegroundMask<T, U>(
                templateColors[i], frameColors[i], blurRadius, threshold, morphClosingRadius);
    });
  }
  threadPool.join();
  return masks;
}

} // namespace fb360_dep::background_subtraction
