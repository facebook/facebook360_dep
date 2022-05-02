/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <map>
#include <string>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <Eigen/Geometry>

#include "source/util/CvUtil.h"
#include "source/util/MathUtil.h"
#include "source/util/SystemUtil.h"
#include "source/util/ThreadPool.h"

namespace fb360_dep {
namespace depth_estimation {

// helper for jointBilateralFilter and jointBilateralUpsampling. call one of
// those instead of this. when computing the bilateral weight, two colors are
// compared. the generalization is that the color for the current pixel comes
// from guide, and the colors for the neighbor pixels come from neighborGuide.
// For the standard joint-bilateral filter, guide and neighborGuide are the
// same, but for the improved joint-bilatral upsampler, guide is the high
// resolution image, and neighborGuide is a blurred version of it, which is a
// proxy for a low-resolution version of the image.
// Pixel should be either float, Vec2f or Vec3f.
// guide and neighborGuide should be cv::Mats of type CV_32FC3, CV_16UC3 or CV_8UC3
// weightR, weightG, and weightB control how much weight is on each color
// channel in computing color differences for bilateral weight.
template <typename TPixel, typename TGuide>
cv::Mat_<TPixel> generalizedJointBilateralFilter(
    const cv::Mat_<TPixel>& image, // Either float, Vec2f or Vec3f
    const cv::Mat_<TGuide>& guide,
    const cv::Mat_<TGuide>& neighborTGuide,
    const cv::Mat_<bool>& mask,
    const int radius,
    const float sigma,
    const float weight0 = 1.0f,
    const float weight1 = 1.0f,
    const float weight2 = 1.0f,
    const int numThreads = -1) {
  CHECK_EQ(guide.size(), neighborTGuide.size());
  CHECK_EQ(image.size(), guide.size());
  CHECK_EQ(guide.size(), mask.size());

  const TPixel zero = 0.0;

  cv::Mat_<TPixel> dest(image.size());
  ThreadPool threadPool(numThreads);
  const int edgeX = image.cols;
  const int edgeY = 1;
  for (int yBegin = 0; yBegin < image.rows; yBegin += edgeY) {
    for (int xBegin = 0; xBegin < image.cols; xBegin += edgeX) {
      const int xEnd = std::min(xBegin + edgeX, image.cols);
      const int yEnd = std::min(yBegin + edgeY, image.rows);
      threadPool.spawn([&, xBegin, yBegin, xEnd, yEnd] {
        for (int y = yBegin; y < yEnd; ++y) {
          for (int x = xBegin; x < xEnd; ++x) {
            if (!mask(y, x)) {
              dest(y, x) = image(y, x);
              continue;
            }

            const auto guideColor = guide(y, x);
            float sumWeight = 0.0f;
            TPixel weightedAvg = zero;

            const float guideFactor = 1 / cv_util::maxPixelValue(guide);
            const float neighborTGuideFactor = 1 / cv_util::maxPixelValue(neighborTGuide);

            for (int v = -radius; v <= radius; ++v) {
              for (int u = -radius; u <= radius; ++u) {
                const int sampleX = math_util::clamp(x + u, 0, image.cols - 1);
                const int sampleY = math_util::clamp(y + v, 0, image.rows - 1);

                if (!mask(sampleY, sampleX)) {
                  continue;
                }

                const TGuide& neighborTGuideColor = neighborTGuide(sampleY, sampleX);

                const float colorDiffSq = // BGR
                    weight0 *
                        math_util::square(
                            (guideColor[0] * guideFactor) -
                            (neighborTGuideColor[0] * neighborTGuideFactor)) +
                    weight1 *
                        math_util::square(
                            (guideColor[1] * guideFactor) -
                            (neighborTGuideColor[1] * neighborTGuideFactor)) +
                    weight2 *
                        math_util::square(
                            (guideColor[2] * guideFactor) -
                            (neighborTGuideColor[2] * neighborTGuideFactor));
                const float weight =
                    expf((-colorDiffSq / 3.0f) / (2.0f * math_util::square(sigma)));

                sumWeight += weight;
                weightedAvg += weight * image(sampleY, sampleX);
              }
            }
            if (sumWeight != 0.0f) {
              weightedAvg /= sumWeight;
              dest(y, x) = weightedAvg;
            } else {
              dest(y, x) = image(y, x);
            }
          }
        }
      });
    }
  }
  threadPool.join();
  return dest;
}

template <typename T>
static void temporalJointBilateralFilterCol(
    const std::vector<cv::Mat_<T>>& guides,
    const std::vector<cv::Mat_<float>>& images,
    const std::vector<cv::Mat_<bool>>& masks,
    const int frameOffset,
    const float sigma,
    const int spatialRadius,
    const float weight0,
    const float weight1,
    const float weight2,
    cv::Mat_<float>& result,
    const int y) {
  const float maxImageValue = cv_util::maxPixelValue(guides[frameOffset]);
  for (int x = 0; x < result.cols; ++x) {
    if (!masks[frameOffset](y, x)) {
      result(y, x) = images[frameOffset](y, x);
      continue;
    }
    float weightedSumPix = 0.0f;
    float sumWeight = 0.0f;
    const T referenceColor = guides[frameOffset](y, x);
    for (ssize_t t = 0; t < ssize(guides); ++t) {
      for (int u = -spatialRadius; u <= spatialRadius; ++u) {
        for (int v = -spatialRadius; v <= spatialRadius; ++v) {
          const int sampleX = math_util::clamp(x + u, 0, result.cols - 1);
          const int sampleY = math_util::clamp(y + v, 0, result.rows - 1);
          if (!masks[t](sampleY, sampleX)) {
            continue;
          }

          const T sampleColor = guides[t](sampleY, sampleX);

          const float weightedDiff = // BGR
              weight0 * math_util::square((referenceColor[0] - sampleColor[0]) / maxImageValue) +
              weight1 * math_util::square((referenceColor[1] - sampleColor[1]) / maxImageValue) +
              weight2 * math_util::square((referenceColor[2] - sampleColor[2]) / maxImageValue);
          const float weight = expf(-weightedDiff / math_util::square(sigma));

          weightedSumPix += images[t](y, x) * weight;
          sumWeight += weight;
        }
      }
    }
    result(y, x) = (weightedSumPix / sumWeight);
  }
}

// temporal joint-bilateral filter: intended for use with time series of depth
// maps. uses the RGB images as a guide for the depth bilateral weights.
// returns the filtered depthmap for camera camIdx in frame frameIdx. the
// spatial support of the filter is 1x1.
// Assumes frameImages are CV_32F, CV_16U or CV_8U
// frameOffset is an index of guides/images for which frame to render
template <typename T>
static void temporalJointBilateralFilter(
    const std::vector<cv::Mat_<T>>& guides,
    const std::vector<cv::Mat_<float>>& images,
    const std::vector<cv::Mat_<bool>>& masks,
    const int frameOffset,
    const float sigma,
    const int spatialRadius,
    const float weight0,
    const float weight1,
    const float weight2,
    cv::Mat_<float>& result,
    const int numThreads = -1) {
  for (ssize_t t = 0; t < ssize(guides); ++t) {
    CHECK_GE(guides[t].channels(), 3);
  }

  result = cv::Mat(images[frameOffset].size(), CV_32FC1);
  ThreadPool threadPool(numThreads);
  for (int y = 0; y < result.rows; ++y) {
    threadPool.spawn(
        &temporalJointBilateralFilterCol<T>,
        std::ref(guides),
        std::ref(images),
        std::ref(masks),
        frameOffset,
        sigma,
        spatialRadius,
        weight0,
        weight1,
        weight2,
        std::ref(result),
        y);
  }
  threadPool.join();
}

} // namespace depth_estimation
} // namespace fb360_dep
