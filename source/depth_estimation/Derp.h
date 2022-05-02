/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "source/depth_estimation/DerpUtil.h"

#include <random>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/depth_estimation/PyramidLevel.h"
#include "source/util/CvUtil.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"

namespace fb360_dep {
namespace depth_estimation {

// Cost function
static const int kSearchWindowRadius = 1;
static const int kNeighborTemplateCode = 0; // defined in ImageUtil::candidateTemplate*
static const int kMinOverlappingCams = 2;
static const int kDoColorPruning = false; // only use perceptually similar color neighbors for cost
static const int kColorPruningNumNeighbors = 25;

// Brute force
static const int kNumDepths = 150; // for brute-force step

// Random proposals
static const float kRandomPropMaxCost = 5.0;
static const float kRandomPropHighVarDeviation = 0.1;

// Median filter
static const int kMedianFilterRadius = 1; // must be 1 or 2

// Spatial bilateral filter
static const int kBilateralSpaceRadiusMin = 1; // at coarsest level of the pyramid
static const int kBilateralSpaceRadiusMax = 5; // at finest level of the pyramid
static const float kBilateralSigma = 0.005;
static const float kBilateralWeightR = 1.0;
static const float kBilateralWeightG = 1.0;
static const float kBilateralWeightB = 0.5;

// Debugging: plot destination (x, y, depth) matches on all overlapping src cameras at given level
// Outputs for random proposal and ping pong propagation
static const int kDebugPlotMatchLevel = -1;
static const std::string kDebugPlotMatchDst = "";
static const int kDebugPlotMatchX = -1;
static const int kDebugPlotMatchY = -1;

void plotMatches(
    const PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const std::string& caller,
    const filesystem::path& outputDir = "");

void getPyramidLevelSizes(std::map<int, cv::Size>& sizes, const filesystem::path& imageDir);

void computeBruteForceCosts(
    PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const int dstIdx,
    const float disparity,
    cv::Mat_<float>& costMap,
    cv::Mat_<float>& confidenceMap);

void computeBruteForceDisparity(
    PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const int dstIdx,
    const float minDepthMeters,
    const float maxDepthMeters,
    const bool partialCoverage,
    const bool useForegroundMasks,
    const int numThreads = -1);

void computeBruteForceDisparities(
    PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const float minDepthMeters,
    const float maxDepthMeters,
    const bool partialCoverage,
    const bool useForegroundMasks,
    const int numThreads = -1);

void pingPongPropagation(
    PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const int iterations,
    const int numThreads = -1,
    const filesystem::path& debugDir = "");

void handleDisparityMismatches(
    PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const int startLevel,
    const int numThreads = -1);

void precomputeProjections(
    PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const int numThreads = -1);

void preprocessLevel(
    PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const float minDepthMeters,
    const float maxDepthMeters,
    const bool partialCoverage,
    const bool useForegroundMasks,
    const int numThreads = -1);

void reprojectColors(
    PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const int numThreads = -1);

void randomProposals(
    PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const int numProposals,
    const float minDepthM,
    const float maxDepthM,
    const int numThreads = -1,
    const filesystem::path& outputDir = "");

// Note: If this doesn't link, then an explicit template instantiation entry
// needs to be added in Derp.cpp
void bilateralFilter(
    PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const int numThreads = -1);

void medianFilter(
    PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const int numThreads = -1);

void maskFov(PyramidLevel<depth_estimation::PixelType>& pyramidLevel, const int numThreads = -1);

void processLevel(
    PyramidLevel<PixelType>& pyramidLevel,
    const std::string& outputFormats,
    const bool useForegroundMasks,
    const std::string& outputRoot,
    const int numRandomProposals,
    const bool partialCoverage,
    const float minDepthM,
    const float maxDepthM,
    const bool doMedianFilter,
    const bool saveDebugImages,
    const int pingPongIterations,
    const int mismatchesStartLevel,
    const bool doBilateralFilter,
    const int threads);

void saveResults(
    PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const bool saveDebugImages,
    const std::string& outputFormatsIn);

std::tuple<float, float> computeCost(
    const PyramidLevel<depth_estimation::PixelType>& pyramidLevel,
    const int dstIdx,
    const float disparity,
    const int x,
    const int y);

} // namespace depth_estimation
} // namespace fb360_dep
