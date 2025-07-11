/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include "source/util/Camera.h"
#include "source/util/CvUtil.h"
#include "source/util/ImageTypes.h"

namespace fb360_dep::depth_estimation {

using PixelType = cv::Vec3w;
using PixelTypeFloat = cv::Vec3f; // floating point version of PixelType

const float kLevelScale = 0.9f;
const float kScaleDisparityPlot = 255.0f;
const float kScaleCostPlot = 255.0f / 100.0f;
const float kScaleConfidencePlot = 255.0f * 100.0f;

const std::vector<float> kRgbWeights = {0.3333f, 0.3334f, 0.3333f};

// Use variance corresponding to 8 bit rounding error
// If noise adds 0.5 in [0..255]
// => var = integral_0.5^0.5 (x/255^2) = 1/12 / 255^2 = 1/12/65025 in [0..1]
const float kMinVar = 1.0f / 12.0f / 65025.0f;

const std::vector<std::array<int, 2>> candidateTemplateOriginal = {
    {{{0, 0}},
     {{-1, 0}},
     {{1, 0}}, //   []      []
     {{0, -1}}, //       []
     {{0, 1}}, //     [][][]
     {{-2, -2}}, //       []
     {{2, -2}}, //   []      []
     {{-2, 2}},
     {{2, 2}}}};

Camera::Vector3 dstToWorldPoint(
    const Camera& camDst,
    const int x,
    const int y,
    const float disparity,
    const int dstW,
    const int dstH,
    const double shiftX = 0.5,
    const double shiftY = 0.5);

bool worldToSrcPoint(
    Camera::Vector2& pSrc,
    const Camera::Vector3& pWorld,
    const Camera& camSrc,
    const int srcW,
    const int srcH);

std::vector<int> mapSrcToDstIndexes(const Camera::Rig& rigSrc, const Camera::Rig& rigDst);

std::vector<std::array<int, 2>> prunePingPongCandidates(
    const std::vector<std::array<int, 2>>& pingPongCandidateOffsets,
    const cv::Mat_<cv::Vec3b>& labImage,
    const std::array<int, 2>& startPoint,
    const size_t numNeighbors);

std::pair<float, float> computeSSD(
    const cv::Mat_<PixelType>& dstColor,
    const int x,
    const int y,
    const PixelType& dstBias,
    const cv::Mat_<PixelType>& dstSrcColor,
    const float xDstSrc,
    const float yDstSrc,
    const PixelType& dstSrcBias,
    const int radius);

void plotDstPointInSrc(
    const Camera& camDst,
    const int x,
    const int y,
    const float disparity,
    const Camera camSrc,
    const cv::Mat_<PixelType>& srcColor,
    const cv::Mat_<PixelType>& dstColor,
    const filesystem::path& outputDir,
    const std::string& prefix);

cv::Mat_<PixelType> project(
    const cv::Mat_<PixelType>& srcColor,
    const cv::Mat_<cv::Vec2f>& warpDstToSrc);

cv::Mat_<PixelType> colorBias(const cv::Mat_<PixelType>& color, const int blurRadius);

cv::Mat computeRgbVariance(const cv::Mat& image, const int windowRadius);

cv::Mat_<float> computeImageVariance(const cv::Mat& image);

std::vector<cv::Mat_<bool>>
generateFovMasks(const Camera::Rig& rig, const cv::Size& size, const int threads);

filesystem::path getImageDir(const filesystem::path& dir, const ImageType& imageType);

filesystem::path
getImageDir(const filesystem::path& dir, const ImageType& imageType, const int level);

filesystem::path getImageDir(
    const filesystem::path& dir,
    const ImageType& imageType,
    const int level,
    const std::string& camId);

filesystem::path
getImageDir(const filesystem::path& dir, const ImageType& imageType, const std::string& camId);

filesystem::path genFilename(
    const filesystem::path& dir,
    const ImageType& imageType,
    const int level,
    const std::string& camId,
    const std::string& frameName,
    const std::string& extension);

void createLevelOutputDirs(
    const filesystem::path& outputDir,
    const int level,
    const Camera::Rig& rig,
    const bool saveDebugImages);

} // namespace fb360_dep::depth_estimation
