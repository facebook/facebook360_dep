/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/depth_estimation/UpsampleDisparityLib.h"

#include <utility>
#include <vector>

#include <glog/logging.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "DerpUtil.h"
#include "source/util/Camera.h"
#include "source/util/MathUtil.h"
#include "source/util/ThreadPool.h"

namespace fb360_dep {
namespace depth_estimation {

// Clock-wise outward spiral starting at (0, 0) of diameter w
static std::vector<std::pair<int, int>> spiral(const int w) {
  int x = 0;
  int y = 0;
  int dx = 0; // 1: right, -1: left, 0: don't move
  int dy = -1;
  int t = w;
  int samples = t * t;
  std::vector<std::pair<int, int>> locs;
  for (int i = 0; i < samples; ++i) {
    const bool isValidX = (-w / 2 <= x) && (x <= w / 2);
    const bool isValidY = (-w / 2 <= y) && (y <= w / 2);
    if (isValidX && isValidY) {
      locs.emplace_back(x, y);
    }
    const bool isCorner = x == y;
    const bool isEdgeLeftX = (x < 0) && (x == -y);
    const bool isEdgeRightX = (x > 0) && (x == 1 - y);
    if (isCorner || isEdgeLeftX || isEdgeRightX) {
      // Rotate 90 degrees and switch direction of x
      t = dx;
      dx = -dy;
      dy = t;
    }
    x += dx;
    y += dy;
  }
  return locs;
}

static cv::Mat_<float> replaceNans(
    const cv::Mat_<float>& dispUp,
    const cv::Mat_<float>& bgDispUp,
    const cv::Mat_<bool>& maskUp,
    const int radius) {
  cv::Mat_<bool> maskNan(maskUp.size());
  maskUp.copyTo(maskNan);
  maskNan.setTo(false, dispUp > 0); // true = NAN inside mask
  std::vector<cv::Point> nanLocs;
  cv::findNonZero(maskNan, nanLocs);
  cv::Mat_<float> dispOut(dispUp.size());
  dispUp.copyTo(dispOut);
  const std::vector<std::pair<int, int>> spiralLocs = spiral(radius * 2 + 1);
  for (const cv::Point& p : nanLocs) {
    // Find valid pixel in neighborhood
    for (const auto& loc : spiralLocs) {
      const int xx = math_util::clamp(p.x + loc.first, 0, maskNan.cols - 1);
      const int yy = math_util::clamp(p.y + loc.second, 0, maskNan.rows - 1);
      const float d = dispUp(yy, xx);
      if (d > 0) {
        dispOut(p.y, p.x) = d;
        break;
      }
    }
  }

  for (int y = 0; y < dispOut.rows; ++y) {
    for (int x = 0; x < dispOut.cols; ++x) {
      if (std::isnan(dispOut(y, x)) || dispOut(y, x) == 0) {
        dispOut(y, x) = bgDispUp(y, x);
      }
    }
  }

  return dispOut;
}

int getRadius(const cv::Size& size, const cv::Size& sizeUp) {
  const float scale = float(sizeUp.width) / float(size.width);
  return scale * scale + 1;
}

static void upsampleDisparityInPlace(
    cv::Mat_<float>& dispUp,
    const cv::Mat_<float>& disp,
    const cv::Mat_<float>& bgDispUp,
    const cv::Mat_<bool>& mask,
    const cv::Mat_<bool>& maskUpIn,
    const cv::Size& sizeUp,
    const bool useForegroundMasks) {
  // NOTE: This trick is only for foreground disparities
  // The background disparity can be upscaled separately calling this app without a mask,
  // and used to fill the NaNs outside the full-size mask
  // 1) Downscale mask to disparity size, and set disparity outside the mask to NAN
  //    This prevents background disparities to be part of the upscaling
  // 2) Upscale disparity using nearest pixel
  //    Any window contain NANs interpolates to NAN. Nearest pixel does not interpolate
  // 3) Remove disparities outside full-size mask
  // 4) Replace NAN pixels inside full-size mask with the closest valid value
  // 5) Apply joint bilateral filter
  const int radius = getRadius(mask.size(), sizeUp);

  if (useForegroundMasks) {
    // 1)
    cv::Mat_<float> dispSmallMasked = disp.clone();
    dispSmallMasked.setTo(NAN, mask == 0);

    // 2)
    cv::Mat_<float> dispUpMasked;
    cv::resize(dispSmallMasked, dispUpMasked, sizeUp, 0, 0, cv::INTER_NEAREST);

    // 3)
    cv::Mat_<bool> maskUp;
    if (maskUpIn.size() != sizeUp) {
      LOG(WARNING) << "Warning: Desired resolution does not match mask resolution: " << sizeUp
                   << " vs. " << maskUpIn.size() << ". Rescaling mask to " << sizeUp;
      cv::resize(maskUp, maskUpIn, sizeUp, 0, 0, cv::INTER_NEAREST);
    } else {
      maskUp = maskUpIn;
    }
    dispUpMasked.setTo(NAN, maskUp == 0);

    // 4)
    dispUp = replaceNans(dispUpMasked, bgDispUp, maskUp, radius);
  } else {
    // OpenCV doesn't handle NaNs when resizing
    const float minDisp = 1e-4;
    cv::Mat_<float> dispSmallMasked = disp.clone();
    dispSmallMasked.setTo(minDisp, disp != disp);
    cv::resize(dispSmallMasked, dispUp, sizeUp, 0, 0, cv::INTER_LANCZOS4);
  }
}

std::vector<cv::Mat_<float>> upsampleDisparities(
    const Camera::Rig& rigIn,
    const std::vector<cv::Mat_<float>>& disps,
    const std::vector<cv::Mat_<float>>& bgDispsUp,
    const std::vector<cv::Mat_<bool>>& masks,
    const std::vector<cv::Mat_<bool>>& masksUpIn,
    const cv::Size& sizeUp,
    const bool useForegroundMasks,
    const int threads) {
  CHECK_EQ(disps.size(), masks.size());
  CHECK_EQ(disps.size(), masksUpIn.size());

  Camera::Rig rig = Camera::Rig(rigIn);
  Camera::normalizeRig(rig);
  const std::vector<cv::Mat_<bool>> fovMasks =
      depth_estimation::generateFovMasks(rig, disps[0].size(), threads);
  const std::vector<cv::Mat_<bool>> fovMasksUp =
      depth_estimation::generateFovMasks(rig, sizeUp, threads);
  std::vector<cv::Mat_<float>> dispsUp(disps.size());
  ThreadPool threadPool(threads);
  for (int i = 0; i < int(disps.size()); ++i) {
    threadPool.spawn(
        &upsampleDisparityInPlace,
        std::ref(dispsUp[i]),
        std::cref(disps[i]),
        std::cref(bgDispsUp[i]),
        fovMasks[i] & masks[i],
        fovMasksUp[i] & masksUpIn[i],
        std::cref(sizeUp),
        useForegroundMasks);
  }
  threadPool.join();
  return dispsUp;
}

} // namespace depth_estimation
} // namespace fb360_dep
