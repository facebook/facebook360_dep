/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/calibration/FeatureMatcher.h"

#include <future>
#include <queue>

#include <boost/timer/timer.hpp>

#include <folly/Format.h>

#include "source/calibration/FeatureDetector.h"

DEFINE_bool(custom_zncc, false, "uses custom ZNCC formula for patch matching");
DEFINE_double(depth_max, 100.0, "max depth in m");
DEFINE_double(depth_min, 1.0, "min depth in m");
DEFINE_double(depth_samples, 1000, "number of depths to sample");
DEFINE_double(max_depth_for_remap, 50, "max depth to reproject features");
DEFINE_double(overlap_threshold, 0, "minimum overlap between matched images");
DEFINE_double(reprojected_corner_drift_tolerance, 0.5, "in pixels");
DEFINE_double(
    search_overlap,
    0.25,
    "overlap fraction between search windows at different disparities");
DEFINE_int32(search_radius, 100, "search radius in pixels");
DEFINE_double(
    zncc_delta_threshold,
    0.05,
    "minimum zncc score difference betwen best and second best potential matches for a corner");

using PixelType = uint8_t;
using Image = cv::Mat_<PixelType>;
using ImageId = std::string;

namespace fb360_dep::calibration {

struct BestMatch {
  int bestIdx;
  double bestScore;
  int secondBestIdx;
  double secondBestScore;

  BestMatch() {
    // Initialize scores to -1. zncc scores are between -1 and 1, higher is better
    // Initialize index to -1 indicating there is no match.
    bestIdx = -1;
    bestScore = -1;
    secondBestIdx = -1;
    secondBestScore = -1;
  }

  void updateCornerScore(const double newScore, const int newIdx) {
    if (newScore > bestScore) {
      // newScore is a better score for this corner
      if (bestIdx == newIdx) {
        // If this is the same index as the previous best don't update the second best
        bestScore = newScore;
      } else {
        secondBestIdx = bestIdx;
        secondBestScore = bestScore;
        bestIdx = newIdx;
        bestScore = newScore;
      }
    } else if (newScore > secondBestScore && bestIdx != newIdx) {
      secondBestScore = newScore;
      secondBestIdx = newIdx;
    }
  }

  // A corner is weak if its best match score falls below the score threshold
  // or if 2 (or more) highest match scores are close together (within
  // zncc_delta_threshold). Weak corners are rejected since the potiential matches
  // are nearly indistiguishable.
  bool isWeakCorner() const {
    return bestScore < FLAGS_match_score_threshold ||
        bestScore - secondBestScore < FLAGS_zncc_delta_threshold;
  }
};

double computeZncc(const Keypoint& corner0, const Keypoint& corner1) {
  CHECK_EQ(corner0.patch.size, corner1.patch.size);
  double zncc = 0;
  for (int x = 0; x < corner0.patch.cols; ++x) {
    for (int y = 0; y < corner0.patch.rows; ++y) {
      zncc += (corner0.patch(y, x) - corner0.avg) * (corner1.patch(y, x) - corner1.avg);
    }
  }

  if (FLAGS_custom_zncc) {
    /* Custom ZNCC formula:

      mean((p0 - p0.avg) * (p1 - p1.avg)) / p0.avg / p1.avg
      ---------------------------------------------
      max(p0.stddev / p0.avg, p1.stddev / p1.avg)^2
    */

    zncc /= corner0.patch.total();
    zncc /= (corner0.avg * corner1.avg); // finishes numerator calculation

    float denominator = std::max(corner0.std / corner0.avg, corner1.std / corner1.avg);
    zncc /= (denominator * denominator);
  } else {
    zncc /= (corner0.std * corner1.std * corner0.patch.total());
  }

  return zncc;
}

// Compute a box around the pixel in camera 1 corresponding to the specified point in camera 0
cv::Rect2f computeBox(
    const Camera& camera1,
    const Camera& camera0,
    const Camera::Vector2& pixel0,
    const double depth) {
  const Camera::Vector3 world = camera0.rig(pixel0, depth);
  const Camera::Vector2 pixel1 = camera1.pixel(world);
  return cv::Rect2f(
      pixel1.x() - FLAGS_search_radius,
      pixel1.y() - FLAGS_search_radius,
      2 * FLAGS_search_radius,
      2 * FLAGS_search_radius);
}

bool tooMuchOverlap(const cv::Rect2f& box, const cv::Rect2f& lastBox) {
  return (box & lastBox).area() > FLAGS_search_overlap * box.area();
}

// Compute what a corner in camera 0 looks like from camera 1. I.e.
// - find the point in camera 1 corresponding to the specified point in camera 0
// - then, for each pixel in a square around that point, read the corresponding pixel from camera 0
// Return false if camera 1 doesn't see the specified point
bool projectCorner(
    Image& projection1,
    const Camera& camera1,
    const Image& img0,
    const Camera& camera0,
    const Keypoint& corner0,
    const double depth0) {
  CHECK_EQ(img0.cols, camera0.resolution.x());
  CHECK_EQ(img0.rows, camera0.resolution.y());
  const Camera::Vector3 corner = camera0.rig(corner0.coords, depth0);
  Camera::Vector2 corner1;
  if (!camera1.sees(corner, corner1)) {
    return false;
  }
  const double depth1 = (corner - camera1.position).norm();

  CHECK_EQ(corner0.patch.cols, corner0.patch.rows);
  const int radius = corner0.patch.cols / 2;
  projection1.create(2 * radius + 1, 2 * radius + 1);
  for (int xOffset = -radius; xOffset <= radius; xOffset++) {
    for (int yOffset = -radius; yOffset <= radius; yOffset++) {
      const Camera::Vector2 pixel1 = {corner1.x() + xOffset, corner1.y() + yOffset};
      const Camera::Vector3 world = camera1.rig(pixel1, depth1);
      Camera::Vector2 pixel0;
      if (!camera0.sees(world, pixel0)) {
        return false;
      }
      projection1(yOffset + radius, xOffset + radius) = FLAGS_use_nearest
          ? img0(int(pixel0.y()), int(pixel0.x()))
          : cv_util::getPixelBilinear(img0, pixel0.x(), pixel0.y());
    }
  }
  return true;
}

bool hasCornerNearCenter(const Image& image) {
  const Camera::Vector2 center = 0.5 * Camera::Vector2(image.cols, image.rows);
  Camera::Vector2 best = center;
  for (const Camera::Vector2& corner : findScaledCorners(1, image, cv::Mat_<uint8_t>())) {
    Camera::Vector2 offset = corner - center;
    if (offset.dot(offset) < best.dot(best)) {
      best = offset;
    }
  }
  return best.dot(best) <= math_util::square(FLAGS_reprojected_corner_drift_tolerance);
}

bool getNextDepthSample(
    int& currentDepthSample,
    double& currentDisparity,
    cv::Rect2f& currentBox,
    const Camera& camera0,
    const Camera::Vector2& corner0Coords,
    const Camera& camera1) {
  for (int nextDepthSample = currentDepthSample + 1; nextDepthSample < FLAGS_depth_samples;
       ++nextDepthSample) {
    // We don't bother testing a disparity unless the search box is substantially different
    const double nextDisparity = math_util::lerp(
        1 / FLAGS_depth_max, 1 / FLAGS_depth_min, nextDepthSample / (FLAGS_depth_samples - 1.0));
    const cv::Rect2f nextBox = computeBox(camera1, camera0, corner0Coords, 1 / nextDisparity);
    if (!tooMuchOverlap(nextBox, currentBox)) {
      currentDepthSample = nextDepthSample;
      currentDisparity = nextDisparity;
      currentBox = nextBox;
      return true;
    }
  }
  return false;
}

Overlap findMatches(
    const Image& img0,
    const std::vector<Keypoint>& corners0,
    const Camera& camera0,
    const Image& img1,
    const std::vector<Keypoint>& corners1,
    const Camera& camera1) {
  boost::timer::cpu_timer timer;
  boost::timer::cpu_timer znccTimer;
  znccTimer.stop();
  boost::timer::cpu_timer projectCornerTimer;
  projectCornerTimer.stop();

  Image image1; // optimization: avoid reallocation by keeping this outside loop

  // For each corner in corners0, compute its best and second best match in corners1. and vice versa
  std::vector<BestMatch> bestMatches0(corners0.size());
  std::vector<BestMatch> bestMatches1(corners1.size());
  int callsToZncc = 0;
  int callsToProjectCorners = 0;
  for (ssize_t index0 = 0; index0 < ssize(corners0); index0++) {
    LOG_IF(INFO, (FLAGS_threads == 0 || FLAGS_threads == 1) && (index0 % 1000) == 0)
        << "Processing feature " << index0 << " of " << corners0.size() << " from pair "
        << camera0.id << " " << camera1.id;

    const Keypoint& corner0 = corners0[index0];
    int depthSample = -1;
    double disparity = 0;
    cv::Rect2f box1(0, 0, 0, 0);
    bool firstProjection = true;
    while (getNextDepthSample(depthSample, disparity, box1, camera0, corner0.coords, camera1)) {
      // only remap corner for sufficiently large disparities
      if (firstProjection || disparity > 1 / FLAGS_max_depth_for_remap) {
        // compute what the area around corner 0 would look like from camera 1
        projectCornerTimer.resume();
        callsToProjectCorners++;
        if (!projectCorner(image1, camera1, img0, camera0, corner0, 1 / disparity)) {
          continue;
        }
        projectCornerTimer.stop();

        // don't match if we can't rediscover the corner after it has been reprojected
        if (!hasCornerNearCenter(image1)) {
          continue;
        }
        firstProjection = false;
      }

      Keypoint projection1(image1);

      // look for a corner in c1 that is in the box and looks similar
      znccTimer.resume();
      for (ssize_t index1 = 0; index1 < ssize(corners1); ++index1) {
        cv::Point2f cvCoords(corners1[index1].coords.x(), corners1[index1].coords.y());
        if (!box1.contains(cvCoords)) {
          continue;
        }
        double score = computeZncc(projection1, corners1[index1]);
        bestMatches0[index0].updateCornerScore(score, index1);
        bestMatches1[index1].updateCornerScore(score, index0);
        callsToZncc++;
      }
      znccTimer.stop();
    }
  }

  // Take match if both ends are strong and each other's best match
  Overlap overlap(camera0.id, camera1.id);
  for (const BestMatch& bestMatch0 : bestMatches0) {
    if (bestMatch0.isWeakCorner()) {
      continue;
    }
    const BestMatch& bestMatch1 = bestMatches1[bestMatch0.bestIdx];
    if (bestMatch1.isWeakCorner()) {
      continue;
    }
    if (&bestMatch0 != &bestMatches0[bestMatch1.bestIdx]) {
      continue;
    }
    overlap.matches.emplace_back(bestMatch0.bestScore, bestMatch1.bestIdx, bestMatch0.bestIdx);
  }

  // Only report timing in single threaded mode
  // In multithreaded mode these will clocks include time from other threads
  // running simultaneously
  if (FLAGS_enable_timing && FLAGS_threads == 1) {
    LOG(INFO) << folly::sformat(
        "{} and {} matching complete. Overlap fraction: {}. Matches: {}. Timing: {} "
        "Calls to ZNCC: {}. ZNCC Time: {} "
        "Calls to ProjectCorners: {}. Project Corner Time: {} ",
        camera0.id,
        camera1.id,
        camera0.overlap(camera1),
        overlap.matches.size(),
        timer.format(),
        callsToZncc,
        znccTimer.format(),
        callsToProjectCorners,
        projectCornerTimer.format());
  } else {
    LOG(INFO) << folly::sformat(
        "{} and {} matching complete. Overlap fraction: {}. Matches: {}",
        camera0.id,
        camera1.id,
        camera0.overlap(camera1),
        overlap.matches.size());
  }
  return overlap;
}

std::vector<Overlap> findAllMatches(
    const Camera::Rig& rig,
    const std::vector<Image>& images,
    const std::map<ImageId, std::vector<Keypoint>>& allCorners) {
  std::vector<Overlap> overlaps;
  boost::timer::cpu_timer matchTimer;

  // Find matches across all pairings
  const int threadCount = ThreadPool::getThreadCountFromFlag(FLAGS_threads);
  std::queue<std::future<Overlap>> overlapFutures;
  for (ssize_t c1 = 0; c1 < ssize(rig); c1++) {
    for (ssize_t c2 = c1 + 1; c2 < ssize(rig); c2++) {
      if (rig[c1].overlap(rig[c2]) < FLAGS_overlap_threshold) {
        continue;
      }
      overlapFutures.push(std::async(
          threadCount == 0 ? std::launch::deferred : std::launch::async,
          &findMatches,
          std::cref(images[c1]),
          std::cref(allCorners.at(rig[c1].id)),
          std::cref(rig[c1]),
          std::cref(images[c2]),
          std::cref(allCorners.at(rig[c2].id)),
          std::cref(rig[c2])));
      if (ssize(overlapFutures) >= threadCount && !overlapFutures.empty()) {
        overlaps.emplace_back(overlapFutures.front().get());
        overlapFutures.pop();
      }
    }
  }

  while (!overlapFutures.empty()) {
    overlaps.emplace_back(overlapFutures.front().get());
    overlapFutures.pop();
  }

  if (FLAGS_enable_timing) {
    LOG(INFO) << folly::sformat("Matching stage time: {}", matchTimer.format());
  }

  return overlaps;
}

} // namespace fb360_dep::calibration
