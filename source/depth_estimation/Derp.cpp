/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/depth_estimation/Derp.h"

#include <random>

#include <boost/algorithm/string/predicate.hpp>
#include <fmt/format.h>
#include <glog/logging.h>

#include <folly/Format.h>

#include "source/depth_estimation/TemporalBilateralFilter.h"
#include "source/util/ImageUtil.h"
#include "source/util/ThreadPool.h"

using namespace fb360_dep::cv_util;
using namespace fb360_dep::image_util;

namespace fb360_dep::depth_estimation {

void plotMatches(
    const PyramidLevel<PixelType>& pyramidLevel,
    const std::string& caller,
    const filesystem::path& debugDir) {
  if (debugDir.empty() || kDebugPlotMatchDst.empty() ||
      kDebugPlotMatchLevel != pyramidLevel.level) {
    return;
  }

  LOG(INFO) << fmt::format("Plotting matches for {}...", kDebugPlotMatchDst);

  const int dstIdx = pyramidLevel.findDstIdx(kDebugPlotMatchDst);
  const Camera& camDst = pyramidLevel.rigDst[dstIdx];

  const cv::Mat_<float>& disparity = pyramidLevel.dstDisparity(dstIdx);
  const int xSize = disparity.cols;
  const int ySize = disparity.rows;

  if ((0 <= kDebugPlotMatchX && 0 <= kDebugPlotMatchY)) {
    CHECK(kDebugPlotMatchX < xSize && kDebugPlotMatchY < ySize) << fmt::format(
        "debug coords({}, {}) out of bounds: ({}, {})",
        kDebugPlotMatchX,
        kDebugPlotMatchY,
        xSize,
        ySize);
  }

  for (int srcIdx = 0; srcIdx < int(pyramidLevel.rigSrc.size()); ++srcIdx) {
    const Camera& camSrc = pyramidLevel.rigSrc[srcIdx];
    const cv::Mat_<PixelType>& srcColor = pyramidLevel.srcColor(srcIdx);
    const cv::Mat_<PixelType>& dstColor = pyramidLevel.dstColor(dstIdx);
    plotDstPointInSrc(
        camDst,
        kDebugPlotMatchX,
        kDebugPlotMatchY,
        disparity(kDebugPlotMatchY, kDebugPlotMatchX),
        camSrc,
        srcColor,
        dstColor,
        debugDir,
        caller);
  }
}

void getPyramidLevelSizes(std::map<int, cv::Size>& sizes, const filesystem::path& imageDir) {
  if (!boost::filesystem::exists(imageDir)) {
    return;
  }
  // Use the first image we find at each level
  const bool includeHidden = false;
  for (const auto& entry : filesystem::directory_iterator(imageDir)) {
    const bool isDir = filesystem::is_directory(entry);
    const filesystem::path p = entry.path();
    const bool isHidden = filesystem::isHidden(p);
    if (!isDir || isHidden) {
      continue;
    }

    const std::string levelDelim = "level_";
    const std::string filename = p.filename().string();
    if (!boost::starts_with(filename, levelDelim)) {
      continue;
    }
    const std::string levelStr = filename.substr(filename.find(levelDelim) + levelDelim.size());
    const filesystem::path imageFn = filesystem::getFirstFile(p, includeHidden, false, "", ".tar");
    if (imageFn.empty()) {
      continue;
    }
    const cv::Mat_<float> image = cv_util::loadImage<float>(imageFn);
    sizes[std::stoi(levelStr)] = image.size();
  }
}

// equivalent to TYPE NAME[SIZE] but sized at runtime
#define STACK_ARRAY(TYPE, NAME, SIZE) TYPE* const NAME = (TYPE*)alloca(sizeof(TYPE) * (SIZE))

std::tuple<float, float> computeCost(
    const PyramidLevel<PixelType>& pyramidLevel,
    const int dstIdx,
    const float disparity,
    const int x,
    const int y) {
  // For a given (x, y, depth) in dst, we find the corresponding (x, y) in src,
  // and then its reprojection into dst where src and dst are aligned up to
  // translation. There we can extract a square patch from src and projected dst
  // to compute the cost.
  // For a given dst, we do this for every src
  // Note that src and dst align up to translation when projected to infinity.
  // Also note that when looking out to the world from the center of dst the
  // color does not change with depth, so dst does not need to be transformed
  //
  // (1) pDst -> (2) pWorld -> (3) pSrc -> (4) pInf -> (5) pDstSrc
  //
  //             (4)
  //        ...       ...
  //         |          |
  //         |           |
  //         |            |
  //         |             |
  //         |              |
  //         |               |
  //         |       (2)      |
  //         |       /  |_     |
  //   ______|______/_    |_ ___|___________
  //  |      |     / |      ||__ |          |
  //  |      |   (1) |       |  ||          |
  //  |     (5)      |       |  (3)         |
  //  |              |       |              |
  //  |              |       |              |
  //  |______________|       |______________|
  //        dst                    src

  // (1) pDst = (x, y)
  // (2) get pWorld
  const cv::Mat_<PixelType>& dstColor = pyramidLevel.dstProjColor(dstIdx);
  const Camera& camDst = pyramidLevel.rigDst[dstIdx];
  const Camera::Vector3 pWorld =
      dstToWorldPoint(camDst, x, y, disparity, dstColor.cols, dstColor.rows);

  // Compute SSD between dst and projected src for each src
  using SSDPair = std::pair<float, float>;
  STACK_ARRAY(SSDPair, SSDs, pyramidLevel.rigSrc.size());
  int ssdCount = 0;
  const cv::Mat_<PixelType>& dstColorBias = pyramidLevel.dstProjColorBias(dstIdx);
  for (ssize_t srcIdx = 0; srcIdx < ssize(pyramidLevel.rigSrc); ++srcIdx) {
    // No SSD if src = dst
    if (srcIdx == pyramidLevel.dst2srcIdxs[dstIdx]) {
      continue;
    }

    // (3) get pSrc
    const Camera& camSrc = pyramidLevel.rigSrc[srcIdx];
    const cv::Size& srcSize = pyramidLevel.srcColor(srcIdx).size();
    Camera::Vector2 pSrc;
    if (!worldToSrcPoint(pSrc, pWorld, camSrc, srcSize.width, srcSize.height)) {
      continue;
    }

    // Exclude a half-texel band to simulate proper clamp-to-border semantics
    const bool kExcludeHalfTexel = false;
    if (kExcludeHalfTexel) {
      if (pSrc.x() < 0.5 || srcSize.width - 0.5 < pSrc.x() || pSrc.y() < 0.5 ||
          srcSize.height - 0.5 < pSrc.y()) {
        continue;
      }
    }

    // (3) -> (4) -> (5) mapping from pre-computed projection warp
    const cv::Mat_<cv::Vec2f>& dstProjWarp = pyramidLevel.dstProjWarp(dstIdx, srcIdx);
    const cv::Vec2f pDstSrc = cv_util::getPixelBilinear(dstProjWarp, pSrc.x(), pSrc.y());

    // Check if pDstSrc is within bounds
    const float xDstSrc = pDstSrc[0] + 0.5; // pDstSrc uses opencv coordinate convention
    const float yDstSrc = pDstSrc[1] + 0.5;
    if (std::isnan(xDstSrc) || std::isnan(yDstSrc)) {
      continue;
    }

    // NOTE: bias of src projected into dst (srcBias) is the average of its
    // patch values, which are bilinearly interpolated from floating point
    // projected coordinates (pDstSrc). This is mathematically different than
    // bilinearly interpolating the pre-computed projected bias around pDstSrc,
    // because we are grabbing biases from neighboring footprints, but it seems
    // to produce very similar results
    const cv::Mat_<PixelType>& dstSrcColorBias = pyramidLevel.dstProjColorBias(dstIdx, srcIdx);
    const PixelType dstSrcBias = cv_util::getPixelBilinear(dstSrcColorBias, xDstSrc, yDstSrc);
    const PixelType& dstBias = dstColorBias(y, x);

    const cv::Mat_<PixelType>& dstSrcColor = pyramidLevel.dstProjColor(dstIdx, srcIdx);
    std::pair<float, float> ssd = computeSSD(
        dstColor, x, y, dstBias, dstSrcColor, xDstSrc, yDstSrc, dstSrcBias, kSearchWindowRadius);
    SSDs[ssdCount] = ssd;
    ++ssdCount;
  }

  int keep = kMinOverlappingCams - 1;
  if (ssdCount < keep) {
    return std::make_tuple(FLT_MAX, 0.0f); // not enough cameras see this disparity, skip
  }

  // Add up unbiased SSDs for all but the two patches with the worst biased SSDs
  keep = std::max<int>(keep, ssdCount - 2);
  std::nth_element(SSDs, SSDs + keep, SSDs + ssdCount);
  float cost = 0;
  for (int i = 0; i < keep; ++i) {
    cost += SSDs[i].second;
  }
  cost /= keep;

  // Trust costs when more cameras are involved
  // This also means that we penalize closeup proposals (closer to camera rig
  // means fewer cameras see that point)
  const float trustCoef = 1.0f / keep;

  const float dstVariance = pyramidLevel.dstVariance(dstIdx)(y, x);
  const float confidence = std::max(dstVariance, kMinVar);
  const float costFinal = cost * trustCoef / confidence;
  return std::make_tuple(costFinal, confidence);
}

// Creates a cost map where each (x, y) has a cost calculated from all the
// source cameras
void computeBruteForceCosts(
    PyramidLevel<PixelType>& pyramidLevel,
    const int dstIdx,
    const float disparity,
    cv::Mat_<float>& costMap,
    cv::Mat_<float>& confidenceMap) {
  CHECK_EQ(costMap.size(), pyramidLevel.sizeLevel);
  CHECK_EQ(costMap.size(), confidenceMap.size());

  // When using background disparity, foreground pixels must be closer than background
  const cv::Mat_<bool> closerMask = pyramidLevel.hasForegroundMasks
      ? (pyramidLevel.dstBackgroundDisparity(dstIdx) < disparity)
      : cv::Mat_<bool>(pyramidLevel.dstForegroundMask(dstIdx).size(), true); // all-pass

  // Ignore margins if dst = src (won't be able to get entire patch)
  const int radius = kSearchWindowRadius;
  for (int y = radius; y < costMap.rows - radius; ++y) {
    for (int x = radius; x < costMap.cols - radius; ++x) {
      // Ignore if outside FOV or background pixel or foreground is farther than background
      const bool ignore = !pyramidLevel.dstFovMask(dstIdx)(y, x) ||
          !pyramidLevel.dstForegroundMask(dstIdx)(y, x) || !closerMask(y, x);
      if (ignore) {
        costMap(y, x) = NAN;
        confidenceMap(y, x) = NAN;
      } else {
        std::tie(costMap(y, x), confidenceMap(y, x)) =
            computeCost(pyramidLevel, dstIdx, disparity, x, y);
      }
    }
  }
}

// Brute force: find disparity with lowest cost at each location, typically at
// coarsest level of the pyramid
void computeBruteForceDisparity(
    PyramidLevel<PixelType>& pyramidLevel,
    const int dstIdx,
    const float minDepthMeters,
    const float maxDepthMeters,
    const bool partialCoverage,
    const bool useForegroundMasks,
    const int numThreads) {
  cv::Mat_<float>& dstDisparity = pyramidLevel.dstDisparity(dstIdx);
  cv::Mat_<float>& dstCosts = pyramidLevel.dstCost(dstIdx);
  cv::Mat_<float>& dstConfidences = pyramidLevel.dstConfidence(dstIdx);

  LOG(INFO) << "Computing initial costs at " << pyramidLevel.sizeLevel
            << fmt::format(" ({})", pyramidLevel.rigDst[dstIdx].id);

  std::vector<float> disparities(kNumDepths);
  const float minDisparity = 1.0f / maxDepthMeters;
  const float maxDisparity = 1.0f / minDepthMeters;
  for (int i = 0; i < kNumDepths; ++i) {
    const float d = probeDisparity(i, kNumDepths, minDisparity, maxDisparity);
    disparities[i] = d;
  }

  // Create a cost map for each possible disparity
  ThreadPool threadPool(numThreads);
  std::vector<cv::Mat_<float>> costs(disparities.size());
  std::vector<cv::Mat_<float>> confidences(disparities.size());
  for (int iDisparity = 0; iDisparity < int(disparities.size()); ++iDisparity) {
    costs[iDisparity].create(pyramidLevel.sizeLevel);
    costs[iDisparity].setTo(NAN);
    confidences[iDisparity].create(pyramidLevel.sizeLevel);
    confidences[iDisparity].setTo(NAN);
    threadPool.spawn(
        &computeBruteForceCosts,
        std::ref(pyramidLevel),
        dstIdx,
        disparities[iDisparity],
        std::ref(costs[iDisparity]),
        std::ref(confidences[iDisparity]));
  }
  threadPool.join();

  // Get best cost on each location
  // We have one cost per disparity at each location
  // Ignore margins if dst = src (won't be able to get entire patch)
  const int margin = kSearchWindowRadius;
  for (int y = margin; y < dstDisparity.rows - margin; ++y) {
    for (int x = margin; x < dstDisparity.cols - margin; ++x) {
      if (!pyramidLevel.dstFovMask(dstIdx)(y, x)) { // outside FOV
        dstDisparity(y, x) = NAN;
        continue;
      }

      // Use background value if we're outside the foreground mask
      if (!pyramidLevel.dstForegroundMask(dstIdx)(y, x)) {
        dstDisparity(y, x) = pyramidLevel.dstBackgroundDisparity(dstIdx)(y, x);
        continue;
      }

      float minCost = FLT_MAX;
      float minCostConfidence = 0;
      int bestDisparityIdx = -1;
      for (int i = 0; i < int(costs.size()); ++i) {
        const float cost = costs[i](y, x);
        if (cost < minCost) {
          minCost = cost;
          minCostConfidence = confidences[i](y, x);
          bestDisparityIdx = i;
        }
      }
      if (bestDisparityIdx == -1) {
        // This can only happen if we're outside the overlapping area when we
        // have partial coverage or due to noise in foreground masks
        std::string warning = fmt::format(
            "Insufficient coverage at {} ({}, {}) ", pyramidLevel.rigDst[dstIdx].id, x, y);
        CHECK(partialCoverage || useForegroundMasks) << warning;

        const std::string partialCoverageFailure = "due to partial coverage";
        const std::string foregroundMaskFailure = "due to noisy foreground masks";

        warning += partialCoverage ? partialCoverageFailure : "";
        warning += (partialCoverage && useForegroundMasks) ? " or " : "";
        warning += useForegroundMasks ? foregroundMaskFailure : "";

        LOG(WARNING) << warning;
        dstDisparity(y, x) = minDisparity;
      } else {
        dstDisparity(y, x) = disparities[bestDisparityIdx];
      }
      dstCosts(y, x) = minCost;
      dstConfidences(y, x) = minCostConfidence;
    }
  }

  // Extend disparities to margin
  if (margin > 0) {
    for (int y = 0; y < dstDisparity.rows; ++y) {
      for (int x = 0; x < dstDisparity.cols; ++x) {
        if (x < margin || x >= dstDisparity.cols - margin || y < margin ||
            y >= dstDisparity.rows - margin) {
          // Use background value if we're outside the foreground mask
          if (!pyramidLevel.dstForegroundMask(dstIdx)(y, x)) {
            dstDisparity(y, x) = pyramidLevel.dstBackgroundDisparity(dstIdx)(y, x);
            continue;
          }
          dstDisparity(y, x) = dstDisparity(
              math_util::clamp(y, margin, dstDisparity.rows - margin - 1),
              math_util::clamp(x, margin, dstDisparity.cols - margin - 1));
          dstCosts(y, x) = dstCosts(
              math_util::clamp(y, margin, dstCosts.rows - margin - 1),
              math_util::clamp(x, margin, dstCosts.cols - margin - 1));
          dstConfidences(y, x) = dstConfidences(
              math_util::clamp(y, margin, dstConfidences.rows - margin - 1),
              math_util::clamp(x, margin, dstConfidences.cols - margin - 1));
        }
      }
    }
  }
}

void computeBruteForceDisparities(
    PyramidLevel<PixelType>& pyramidLevel,
    const float minDepthMeters,
    const float maxDepthMeters,
    const bool partialCoverage,
    const bool useForegroundMasks,
    const int numThreads) {
  for (int dstIdx = 0; dstIdx < int(pyramidLevel.rigDst.size()); ++dstIdx) {
    computeBruteForceDisparity(
        pyramidLevel,
        dstIdx,
        minDepthMeters,
        maxDepthMeters,
        partialCoverage,
        useForegroundMasks,
        numThreads);
  }
}

void pingPongRectangle(
    cv::Mat_<float>& dispRes,
    cv::Mat_<float>& costsRes,
    cv::Mat_<float>& confidencesRes,
    const cv::Mat_<bool>& changed,
    const cv::Mat_<cv::Vec3b>& labImage,
    const PyramidLevel<PixelType>& pyramidLevel,
    const int dstIdx,
    const int xBegin,
    const int yBegin,
    const int xEnd,
    const int yEnd) {
  const cv::Mat_<float>& disp = pyramidLevel.dstDisparity(dstIdx);
  const cv::Mat_<float>& confidences = pyramidLevel.dstConfidence(dstIdx);
  const cv::Mat_<bool>& maskFov = pyramidLevel.dstFovMask(dstIdx);
  const cv::Mat_<float>& dispBackground = pyramidLevel.dstBackgroundDisparity(dstIdx);
  const cv::Mat_<float>& variance = pyramidLevel.dstVariance(dstIdx);

  for (int y = yBegin; y < yEnd; ++y) {
    for (int x = xBegin; x < xEnd; ++x) {
      if (!maskFov(y, x)) {
        // Keep value from previous frame
        continue;
      }

      // Use background value if we're outside the foreground mask
      if (!pyramidLevel.dstForegroundMask(dstIdx)(y, x)) {
        dispRes(y, x) = dispBackground(y, x);
        continue;
      }

      // Ignore locations with low variance
      if (variance(y, x) < pyramidLevel.varNoiseFloor) {
        continue;
      }

      float bestCost = INFINITY;
      float bestDisparity = disp(y, x);
      float bestConfidence = confidences(y, x);

      std::vector<std::array<int, 2>> candidateNeighborOffsets;
      if (kDoColorPruning) {
        const std::array<int, 2>& startPoint = {{x, y}};
        candidateNeighborOffsets = prunePingPongCandidates(
            candidateTemplateOriginal, labImage, startPoint, kColorPruningNumNeighbors);
      } else {
        candidateNeighborOffsets = candidateTemplateOriginal;
      }

      const float backgroundDisparity =
          pyramidLevel.hasForegroundMasks ? pyramidLevel.dstBackgroundDisparity(dstIdx)(y, x) : 0;

      for (const auto& candidateNeighborOffset : candidateNeighborOffsets) {
        const int xx = math_util::clamp(x + candidateNeighborOffset[0], 0, disp.cols - 1);
        const int yy = math_util::clamp(y + candidateNeighborOffset[1], 0, disp.rows - 1);
        if (maskFov(yy, xx)) { // inside FOV
          const float d = disp(yy, xx);

          // When using background disparity, foreground pixels must be closer than background
          if (d >= backgroundDisparity && changed(yy, xx)) {
            const auto costValues = computeCost(pyramidLevel, dstIdx, d, x, y);
            const float cost = std::get<0>(costValues);
            if (cost < bestCost) {
              bestCost = cost;
              bestDisparity = d;
              bestConfidence = std::get<1>(costValues);
            }
          }
        }
      }
      dispRes(y, x) = bestDisparity;
      costsRes(y, x) = bestCost;
      confidencesRes(y, x) = bestConfidence;
    }
  }
}

void pingPong(PyramidLevel<PixelType>& pyramidLevel, const int iterations, const int numThreads) {
  for (int dstIdx = 0; dstIdx < int(pyramidLevel.rigDst.size()); ++dstIdx) {
    cv::Mat_<float>& disp = pyramidLevel.dstDisparity(dstIdx);
    cv::Mat_<float>& costs = pyramidLevel.dstCost(dstIdx);
    cv::Mat_<float> dispRes(disp.size());
    disp.copyTo(dispRes);
    cv::Mat_<float> costsRes(disp.size(), INFINITY);
    cv::Mat_<float> confidencesRes(disp.size(), 0);
    cv::Mat_<bool> changed(disp.size(), true);

    cv::Mat_<cv::Vec3b> labImage;
    if (kDoColorPruning) {
      const cv::Mat_<PixelType>& color = pyramidLevel.dstColor(dstIdx);
      cv::Mat_<cv::Vec4b> imageScaled;
      cv::Mat_<cv::Vec3b> bgrImage;
      color.convertTo(imageScaled, CV_8UC4, 255);
      cv::cvtColor(imageScaled, bgrImage, cv::COLOR_BGRA2BGR);
      cv::cvtColor(bgrImage, labImage, cv::COLOR_BGR2Lab);
    }

    for (int it = 1; it <= iterations; ++it) {
      LOG(INFO) << fmt::format(
          "-- ping pong: iter {}/{}, {}", it, iterations, pyramidLevel.rigDst[dstIdx].id);
      const int radius = kSearchWindowRadius;
      ThreadPool threadPool(numThreads);

      const int edgeX = dispRes.cols;
      const int edgeY = 1;
      for (int y = radius; y < dispRes.rows - radius; y += edgeY) {
        for (int x = radius; x < dispRes.cols - radius; x += edgeX) {
          threadPool.spawn(
              &pingPongRectangle,
              std::ref(dispRes),
              std::ref(costsRes),
              std::ref(confidencesRes),
              std::cref(changed),
              std::cref(labImage),
              std::cref(pyramidLevel),
              dstIdx,
              x,
              y,
              std::min(x + edgeX, dispRes.cols - radius),
              std::min(y + edgeY, dispRes.rows - radius));
        }
      }
      threadPool.join();

      changed = disp != dispRes;
      dispRes.copyTo(disp);
      costsRes.copyTo(costs);

      const cv::Mat_<bool>& fovMask = pyramidLevel.dstFovMask(dstIdx);
      const int countFov = cv::countNonZero(fovMask);
      const int count = cv::countNonZero(changed);
      const float changedPct = 100.0f * count / countFov;
      LOG(INFO) << std::fixed << std::setprecision(2) << fmt::format("changed: {}%", changedPct);
    }
  }
}

void pingPongPropagation(
    PyramidLevel<PixelType>& pyramidLevel,
    const int iterations,
    const int numThreads,
    const filesystem::path& debugDir) {
  if (pyramidLevel.level == pyramidLevel.numLevels - 1) {
    return;
  }

  pingPong(pyramidLevel, iterations, numThreads);
  plotMatches(pyramidLevel, "ping_pong", debugDir);
}

void getSrcMismatches(
    std::vector<float>& dispMatches,
    std::vector<float>& dispMismatches,
    const PyramidLevel<PixelType>& pyramidLevel,
    const int dstIdx,
    const int x,
    const int y) {
  CHECK_EQ(pyramidLevel.rigDst.size(), pyramidLevel.rigSrc.size());

  // Do not mark as mismatch if (x, y) is outside foreground mask
  if (!pyramidLevel.dstForegroundMask(dstIdx)(y, x)) {
    return;
  }

  // Compute world point
  const cv::Mat_<float>& dstDisp = pyramidLevel.dstDisparity(dstIdx);
  const Camera& camDst = pyramidLevel.rigDst[dstIdx];
  const auto ptWorld = dstToWorldPoint(camDst, x, y, dstDisp(y, x), dstDisp.cols, dstDisp.rows);

  for (int srcIdx = 0; srcIdx < int(pyramidLevel.rigSrc.size()); ++srcIdx) {
    if (srcIdx == pyramidLevel.dst2srcIdxs[dstIdx]) { // ignore itself
      continue;
    }

    const Camera& camSrc = pyramidLevel.rigSrc[srcIdx];
    const cv::Size sizeSrc = pyramidLevel.srcVariance(srcIdx).size();
    const int srcW = sizeSrc.width;
    const int srcH = sizeSrc.height;
    Camera::Vector2 ptSrc;
    if (!worldToSrcPoint(ptSrc, ptWorld, camSrc, srcW, srcH)) {
      continue;
    }

    const float dSrc =
        cv_util::getPixelBilinear(pyramidLevel.dstDisparity(srcIdx), ptSrc.x(), ptSrc.y());

    // Check if disparity in src is within 10% of dst
    // NOTE: Technically we should be using distances from the rig origin, not from each camera
    // origin, but the mismatch unlock is an approximation, and this approach is faster and less
    // complex. This works well because the distance between the cameras is at least an order of
    // magnitude smaller than any mismatches
    static const float kFractionChange = 0.1f;
    const float dDstMin = (1.0f - kFractionChange) * dstDisp(y, x);
    const float dDstMax = (1.0f + kFractionChange) * dstDisp(y, x);
    if (dDstMin <= dSrc && dSrc <= dDstMax) {
      dispMatches.push_back(dSrc);
    } else {
      dispMismatches.push_back(dSrc);
    }
  }
}

void updateDstDisparityAndMismatchMask(
    bool& dstDispMask,
    float& dstDispNew,
    const float dispCurr,
    const std::vector<float>& dispMatches,
    std::vector<float>& dispMismatches,
    const float dstVar,
    const float varThreshLow,
    const float varThreshHigh) {
  if (dispMatches.size() + dispMismatches.size() == 0) {
    // We could reach this point if we are outside the foreground mask, or if the
    // current camera (x, y) falls outside the FOV of the rest of the cameras.
    // We want both the new disparity and the masked disparity to have the same
    // value
    dstDispMask = false;
    dstDispNew = dispCurr;
    return;
  }

  // Number of src cameras that have to agree with dst camera for the current
  // disparity to be considered good and not change it
  static const int kNumMinSrcCams = kMinOverlappingCams - 1;

  // Do not modify locations where
  // 1) We have a good disparity (i.e. other src cameras see the same)
  // 2) Variance is high (i.e. on top of edge, we don't wanna touch them)
  // 3) Variance is too low (i.e. noise)
  if (int(dispMatches.size()) >= kNumMinSrcCams || varThreshHigh < dstVar ||
      dstVar < varThreshLow) {
    dstDispMask = false;
    dstDispNew = dispCurr;
  } else {
    dstDispMask = true;

    // Pick median of the farther disparities
    std::sort(dispMismatches.begin(), dispMismatches.end()); // idx 0 = farthest
    int closer;
    for (closer = 0; closer < int(dispMismatches.size()); ++closer) {
      if (dispMismatches[closer] >= dispCurr) { // stop when closer
        break;
      }
    }
    const int median = closer / 2;

    // Don't pick median if current disparity is farther
    dstDispNew = std::min(dispCurr, dispMismatches[median]);
  }
}

// A mismatch happens when (x0, y0, d0) in dst maps to srci (xi, yi), but
// di != d0 for at least a certain number of src cameras
//
// Visually:
//
// + good disparity
// - bad disparities seen by dst
//
// (0) disparity seen by dst at (x, y) = (x, y, d)
// (1) disparity seen by src1 in the direction of (x, y, d)
// (2) disparity seen by src2 in the direction of (x, y, d)
//
// +++++++++++++++ (2)
//    |
//     | +++++++++ (1)
//      | /
//      --- (0)
//      /||
//     / | |
//    /  |  |
// src1 dst src2
//
// Note that (0), (1) and (2) are in separate disparity maps, so (1) and (2) can
// see past (0) in the marked direction.
// Picking (2) unlocks the set of bad disparities so that we have a better
// proposal in the next round
//
// i.e. use overlaping src cameras to look past (0) for proposals
//
// Note that the closer we are to the cameras, the farther apart can (1) and
// (2) potentially be. It'll depend on how far objects behind (0) are
void handleDisparityMismatch(
    std::unordered_map<std::string, cv::Mat_<float>>& mapDstDisp,
    PyramidLevel<PixelType>& pyramidLevel,
    const int dstIdx) {
  CHECK_EQ(pyramidLevel.rigDst.size(), pyramidLevel.rigSrc.size())
      << "Mismatches only valid when considering all cameras";

  const std::string& dstId = pyramidLevel.rigDst[dstIdx].id;
  const cv::Mat_<float>& dstDisp = pyramidLevel.dstDisparity(dstIdx);
  cv::Mat_<bool>& dstMask = pyramidLevel.dstMismatchedDisparityMask(dstIdx);
  cv::Mat_<float> dstDispNew(dstDisp.size(), NAN);

  // For every (x, y, d) in current dst, find (x', y', d') in all src cameras
  // and check if d' is similar to d
  const cv::Mat_<float>& dstVar = pyramidLevel.dstVariance(dstIdx);
  for (int y = 0; y < dstDisp.rows; ++y) {
    for (int x = 0; x < dstDisp.cols; ++x) {
      if (!pyramidLevel.dstFovMask(dstIdx)(y, x)) { // outside FOV
        continue;
      }
      std::vector<float> dispMatches;
      std::vector<float> dispMismatches;
      getSrcMismatches(dispMatches, dispMismatches, pyramidLevel, dstIdx, x, y);
      updateDstDisparityAndMismatchMask(
          dstMask(y, x),
          dstDispNew(y, x),
          dstDisp(y, x),
          dispMatches,
          dispMismatches,
          dstVar(y, x),
          pyramidLevel.varNoiseFloor,
          pyramidLevel.varHighThresh);
    }
  }
  mapDstDisp[dstId] = dstDispNew;
}

void handleDisparityMismatches(
    PyramidLevel<PixelType>& pyramidLevel,
    const int startLevel,
    const int numThreads) {
  if (pyramidLevel.level > startLevel || pyramidLevel.level == pyramidLevel.numLevels - 1) {
    return;
  }

  LOG(INFO) << "Handling source mismatches...";
  CHECK_EQ(pyramidLevel.rigDst.size(), pyramidLevel.rigSrc.size())
      << "Mismatches only valid when considering all cameras";
  ThreadPool threadPool(numThreads);
  std::unordered_map<std::string, cv::Mat_<float>> mapDstDisp;
  for (const Camera& camDst : pyramidLevel.rigDst) {
    // Preallocate buckets to avoid double-free errors when multithreading
    mapDstDisp[camDst.id] = cv::Mat_<float>();
  }
  for (int dstIdx = 0; dstIdx < int(pyramidLevel.rigDst.size()); ++dstIdx) {
    threadPool.spawn(
        &handleDisparityMismatch, std::ref(mapDstDisp), std::ref(pyramidLevel), dstIdx);
  }
  threadPool.join();

  for (int dstIdx = 0; dstIdx < int(pyramidLevel.rigDst.size()); ++dstIdx) {
    mapDstDisp.at(pyramidLevel.rigDst[dstIdx].id).copyTo(pyramidLevel.dstDisparity(dstIdx));
  }
}

void randomProposal(
    PyramidLevel<PixelType>& pyramidLevel,
    const int dstIdx,
    const int y,
    const int numProposals,
    const float minDepthMeters,
    const float maxDepthMeters) {
  std::default_random_engine engine;
  engine.seed(y * pyramidLevel.level);
  cv::Mat_<float>& dstDisparity = pyramidLevel.dstDisparity(dstIdx);
  cv::Mat_<float>& dstCosts = pyramidLevel.dstCost(dstIdx);
  cv::Mat_<float>& dstConfidence = pyramidLevel.dstConfidence(dstIdx);
  const cv::Mat_<float>& variance = pyramidLevel.dstVariance(dstIdx);
  for (int x = kSearchWindowRadius; x < dstDisparity.cols - kSearchWindowRadius; ++x) {
    if (!pyramidLevel.dstFovMask(dstIdx)(y, x)) { // outside FOV
      // Keep value from previous frame
      continue;
    }
    float currDisp = dstDisparity(y, x); // upscaled disparity

    // Use background value if we're outside the foreground mask
    if (!pyramidLevel.dstForegroundMask(dstIdx)(y, x)) {
      dstDisparity(y, x) = pyramidLevel.dstBackgroundDisparity(dstIdx)(y, x);
      continue;
    }

    // Ignore locations with low variance
    // Threshold is a little lower than our high variance threshold
    // High variance locations include:
    // - textured objects: easier to match
    // - new objects: not present at coarser levels
    // Lower than high variance locations include:
    // - weaker edges: as they appear or dissapear
    // - pixels around edges: can see what's behind
    // We can go pretty low, as long as we ignore smooth and noisy areas
    const float varHighDev = kRandomPropHighVarDeviation * pyramidLevel.varHighThresh;
    const float varHighThresh = std::max(varHighDev, pyramidLevel.varNoiseFloor);
    if (variance(y, x) < varHighThresh) {
      continue;
    }

    float currCost;
    float currConfidence;
    std::tie(currCost, currConfidence) = computeCost(pyramidLevel, dstIdx, currDisp, x, y);

    // We will refine only if we're getting much better cost
    const float costThresh = std::fmin(0.5f * currCost, kRandomPropMaxCost);

    // When using background, foreground pixels must be closer than background
    const float minDisp = pyramidLevel.hasForegroundMasks
        ? pyramidLevel.dstBackgroundDisparity(dstIdx)(y, x)
        : (1.0f / maxDepthMeters);
    const float maxDisp = 1.0f / minDepthMeters;

    float amplitude = (maxDisp - minDisp) / 2.0f;
    for (int i = 0; i < numProposals; ++i) {
      float propDisp = std::uniform_real_distribution<float>(
          std::max(float(minDisp), currDisp - amplitude),
          std::min(float(maxDisp), currDisp + amplitude))(engine);
      float propCost;
      float propConfidence;
      std::tie(propCost, propConfidence) = computeCost(pyramidLevel, dstIdx, propDisp, x, y);
      if (propCost < currCost && propCost < costThresh) {
        currCost = propCost;
        currDisp = propDisp;
        currConfidence = propConfidence;
        amplitude /= 2.0f;
      }
    }

    dstDisparity(y, x) = currDisp;
    dstCosts(y, x) = currCost;
    dstConfidence(y, x) = currConfidence;
  }
}

void preprocessLevel(
    PyramidLevel<PixelType>& pyramidLevel,
    const float minDepthMeters,
    const float maxDepthMeters,
    const bool partialCoverage,
    const bool useForegroundMasks,
    const int numThreads) {
  if (pyramidLevel.level == pyramidLevel.numLevels - 1) {
    computeBruteForceDisparities(
        pyramidLevel,
        minDepthMeters,
        maxDepthMeters,
        partialCoverage,
        useForegroundMasks,
        numThreads);
  }
}

void randomProposals(
    PyramidLevel<PixelType>& pyramidLevel,
    const int numProposals,
    const float minDepthMeters,
    const float maxDepthMeters,
    const int numThreads,
    const filesystem::path& debugDir) {
  if (numProposals <= 0 || pyramidLevel.level == pyramidLevel.numLevels - 1) {
    return;
  }

  for (int dstIdx = 0; dstIdx < int(pyramidLevel.rigDst.size()); ++dstIdx) {
    LOG(INFO) << fmt::format("-- random proposals: {}", pyramidLevel.rigDst[dstIdx].id);
    ThreadPool threadPool(numThreads);
    const cv::Size size = pyramidLevel.dstDisparity(dstIdx).size();
    for (int y = kSearchWindowRadius; y < size.height - kSearchWindowRadius; ++y) {
      threadPool.spawn(
          &randomProposal,
          std::ref(pyramidLevel),
          dstIdx,
          y,
          numProposals,
          minDepthMeters,
          maxDepthMeters);
    }
    threadPool.join();
  }

  plotMatches(pyramidLevel, "random_prop", debugDir);
}

void bilateralFilter(PyramidLevel<PixelType>& pyramidLevel, const int numThreads) {
  const float scale = std::pow(kLevelScale, pyramidLevel.level);
  const int spaceRadius =
      std::max(std::ceil(kBilateralSpaceRadiusMax * scale), float(kBilateralSpaceRadiusMin));

  for (int dstIdx = 0; dstIdx < int(pyramidLevel.rigDst.size()); ++dstIdx) {
    cv::Mat_<float>& disparity = pyramidLevel.dstDisparity(dstIdx);
    const cv::Mat_<PixelType>& color = pyramidLevel.dstColor(dstIdx);
    const cv::Mat_<bool>& maskFov = pyramidLevel.dstFovMask(dstIdx);
    const cv::Mat_<bool>& maskFg = pyramidLevel.dstForegroundMask(dstIdx);
    const cv::Mat_<bool> mask = maskFov & maskFg;
    const cv::Mat_<float> disparityFiltered =
        depth_estimation::generalizedJointBilateralFilter<float, PixelType>(
            disparity,
            color,
            color,
            mask,
            spaceRadius,
            kBilateralSigma,
            kBilateralWeightB,
            kBilateralWeightG,
            kBilateralWeightR,
            numThreads);

    // Only use filtered version on foreground pixels
    disparityFiltered.copyTo(disparity, pyramidLevel.dstForegroundMask(dstIdx));
  }
}

void medianFilter(PyramidLevel<PixelType>& pyramidLevel, const int numThreads) {
  ThreadPool threadPool(numThreads);
  for (int dstIdx = 0; dstIdx < int(pyramidLevel.rigDst.size()); ++dstIdx) {
    threadPool.spawn([&, dstIdx] {
      cv::Mat_<float>& disparity = pyramidLevel.dstDisparity(dstIdx);
      const cv::Mat_<float>& bgDisparity = pyramidLevel.dstBackgroundDisparity(dstIdx);
      const cv::Mat_<bool>& maskFov = pyramidLevel.dstFovMask(dstIdx);
      const cv::Mat_<bool>& maskFg = pyramidLevel.dstForegroundMask(dstIdx);
      const cv::Mat_<bool> mask = maskFov & maskFg;
      cv::Mat_<float> disparityFiltered =
          cv_util::maskedMedianBlur(disparity, bgDisparity, mask, kMedianFilterRadius);
      disparityFiltered.copyTo(disparity);
    });
  }

  threadPool.join();
}

void saveResults(
    PyramidLevel<PixelType>& pyramidLevel,
    const bool saveDebugImages,
    const std::string& outputFormatsIn) {
  if (saveDebugImages) {
    LOG(INFO) << fmt::format("Saving debug images for pyramid level {}...", pyramidLevel.level);
    pyramidLevel.saveDebugImages();
  }

  // Always saving outputs at finest level. Forcing PFM if no format chosen
  std::string outputFormats = outputFormatsIn;
  if (outputFormats.empty()) {
    LOG(WARNING) << "No explicit output formats specified. Forcing PFM...";
    outputFormats = "pfm";
  }
  pyramidLevel.saveResults(outputFormats);
}

void maskFov(PyramidLevel<PixelType>& pyramidLevel, const int numThreads) {
  ThreadPool threadPool(numThreads);
  for (int dstIdx = 0; dstIdx < int(pyramidLevel.rigDst.size()); ++dstIdx) {
    threadPool.spawn([&, dstIdx] {
      cv::Mat_<float>& disparity = pyramidLevel.dstDisparity(dstIdx);
      const cv::Mat_<bool>& maskFov = pyramidLevel.dstFovMask(dstIdx);
      const cv::Mat_<float> nanMat(disparity.size(), NAN);
      nanMat.copyTo(disparity, 1 - maskFov);
    });
  }
  threadPool.join();
}

// Reproject each src camera into each dst camera assuming a depth of infinity
// At infinity src and dst will align up to translation
void precomputeProjections(PyramidLevel<PixelType>& pyramidLevel, const int threads) {
  LOG(INFO) << "Pre-computing projections...";
  ThreadPool threadPool(threads);
  for (int dstIdx = 0; dstIdx < int(pyramidLevel.rigDst.size()); ++dstIdx) {
    // Project to current level dst size
    const cv::Size& dstSize = pyramidLevel.dstColor(dstIdx).size();
    Camera camDst = pyramidLevel.rigDst[dstIdx].rescale({dstSize.width, dstSize.height});

    // Project every src to current dst
    for (int srcIdx = 0; srcIdx < int(pyramidLevel.rigSrc.size()); ++srcIdx) {
      threadPool.spawn([&, srcIdx] {
        // Project from current level src size
        const cv::Size& srcSize = pyramidLevel.srcs[srcIdx].color.size();
        Camera camSrc = pyramidLevel.rigSrc[srcIdx].rescale({srcSize.width, srcSize.height});

        pyramidLevel.dstProjWarp(dstIdx, srcIdx) = computeWarpDstToSrc(camSrc, camDst);
        pyramidLevel.dstProjWarpInv(dstIdx, srcIdx) = computeWarpDstToSrc(camDst, camSrc);
      });
    }
    threadPool.join();
  }
}

void reprojectColors(PyramidLevel<PixelType>& pyramidLevel, const int numThreads) {
  LOG(INFO) << "Reprojecting colors...";
  ThreadPool threadPool(numThreads);
  for (int dstIdx = 0; dstIdx < int(pyramidLevel.rigDst.size()); ++dstIdx) {
    // Project every src to current dst
    for (int srcIdx = 0; srcIdx < int(pyramidLevel.rigSrc.size()); ++srcIdx) {
      threadPool.spawn([&, srcIdx] {
        // Project from current level src size
        const cv::Mat_<PixelType>& srcColor = pyramidLevel.srcColor(srcIdx);
        cv::Mat_<PixelType>& srcProjColor = pyramidLevel.dstProjColor(dstIdx, srcIdx);
        if (srcIdx == pyramidLevel.dst2srcIdxs[dstIdx]) {
          // No projection needed if src = dst
          srcProjColor = srcColor;
        } else {
          const cv::Mat_<cv::Vec2f>& warpDstToSrc = pyramidLevel.dstProjWarpInv(dstIdx, srcIdx);
          srcProjColor = project(srcColor, warpDstToSrc);
        }

        // Color bias is just the average over a given area around each pixel
        pyramidLevel.dstProjColorBias(dstIdx, srcIdx) =
            colorBias(srcProjColor, kSearchWindowRadius);
      });
    }
    threadPool.join();
  }
}

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
    const int threads) {
  LOG(INFO) << fmt::format("Processing {} level {}", pyramidLevel.frameName, pyramidLevel.level);
  reprojectColors(pyramidLevel, threads);
  preprocessLevel(pyramidLevel, minDepthM, maxDepthM, partialCoverage, useForegroundMasks, threads);
  randomProposals(pyramidLevel, numRandomProposals, minDepthM, maxDepthM, threads, outputRoot);
  pingPongPropagation(pyramidLevel, pingPongIterations, threads, outputRoot);
  handleDisparityMismatches(pyramidLevel, mismatchesStartLevel, threads);
  if (doBilateralFilter) {
    bilateralFilter(pyramidLevel, threads);
  }
  if (doMedianFilter) {
    medianFilter(pyramidLevel, threads);
  }
  maskFov(pyramidLevel, threads);
  saveResults(pyramidLevel, saveDebugImages, outputFormats);
}

} // namespace fb360_dep::depth_estimation
