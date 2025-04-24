/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/calibration/FeatureDetector.h"

#include <boost/timer/timer.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/Format.h>

#include "source/util/Camera.h"
#include "source/util/ThreadPool.h"

DEFINE_int32(deduplicate_radius, 3, "remove duplicate corners found at different octaves");
DEFINE_double(harris_parameter, 0.04, "harris parameter");
DEFINE_double(harris_window_radius, 5, "harris corner detector window radius");
DEFINE_int32(max_corners, 10000, "maximum number of corners to detect at each level");
DEFINE_int32(min_feature_distance, 10, "minimum distance between features in pixels");
DEFINE_double(min_feature_quality, 0.00001, "minimum feature quality");
DEFINE_double(
    refine_corners_epsilon,
    0.000001,
    "epsilon termiation value for refining corners to subpixel precision");
DEFINE_int32(refine_corners_radius, 5, "window radius for refining corners to subpixel precision");
DEFINE_int32(zncc_window_radius, 16, "zncc window radius in pixels");

using PixelType = uint8_t;
using Image = cv::Mat_<PixelType>;
using ImageId = std::string;

namespace fb360_dep::calibration {

bool isUniqueCorner(
    const std::vector<Keypoint>& corners,
    const int previousCornerCount,
    const Camera::Vector2& corner) {
  if (FLAGS_deduplicate_radius <= 0) {
    return true;
  }
  for (int previousCorner = 0; previousCorner < previousCornerCount; previousCorner++) {
    if ((corners[previousCorner].coords - corner).norm() < FLAGS_deduplicate_radius) {
      return false;
    }
  }
  return true;
}

std::vector<Camera::Vector2> findScaledCorners(
    const double scale,
    const cv::Mat_<uint8_t>& imageFull,
    const cv::Mat_<uint8_t>& maskFull,
    const std::string& cameraId) {
  std::vector<Camera::Vector2> cameraCorners;
  std::vector<cv::Point2f> cvCorners;
  cv::Mat_<uint8_t> gray;
  cv::resize(imageFull, gray, cv::Size(), scale, scale, cv::INTER_AREA);
  cv::Mat_<uint8_t> mask;
  if (!maskFull.empty()) {
    cv::resize(maskFull, mask, cv::Size(), scale, scale, cv::INTER_AREA);
  }

  // Find corners using the cv harris detector
  cv::goodFeaturesToTrack(
      gray,
      cvCorners,
      FLAGS_max_corners,
      FLAGS_min_feature_quality,
      FLAGS_min_feature_distance * (FLAGS_same_scale ? scale : 1),
      mask,
      FLAGS_harris_window_radius,
      true,
      FLAGS_harris_parameter);
  if (cvCorners.empty()) {
    return cameraCorners;
  }

  // Refine corners to subpixel precision
  const cv::Size windowRadius(FLAGS_refine_corners_radius, FLAGS_refine_corners_radius);
  const cv::Size zeroZone = cv::Size(-1, -1); // means "no zeroZone"
  cv::TermCriteria criteria =
      cv::TermCriteria(cv::TermCriteria::EPS, 0, FLAGS_refine_corners_epsilon);

  // If refinement fails, opencv silently leaves the inputs untouched. So use unlikely inputs
  const cv::Point2f kOffset(0.0017, 0.0013); // just some unlikely offset
  std::vector<cv::Point2f> cvRefined(cvCorners);
  for (cv::Point2f& p : cvRefined) {
    p += kOffset;
  }
  cv::cornerSubPix(gray, cvRefined, windowRadius, zeroZone, criteria);

  // Only keep refined points, convert out of opencv coordinate convention, scale
  for (ssize_t i = 0; i < ssize(cvRefined); ++i) {
    if (cvRefined[i] != cvCorners[i] + kOffset) {
      cameraCorners.emplace_back((cvRefined[i].x + 0.5f) / scale, (cvRefined[i].y + 0.5f) / scale);
    }
  }

  return cameraCorners;
}

static bool isCloseToEdge(const Camera::Vector2& point, const Image& image, const int margin) {
  if (0 <= point.x() - margin && point.x() + margin < image.cols) {
    if (0 <= point.y() - margin && point.y() + margin < image.rows) {
      return false;
    }
  }
  return true;
}

cv::Mat_<uint8_t> generateImageCircleMask(const Camera& camera) {
  double width = camera.resolution.x();
  double height = camera.resolution.y();
  cv::Mat_<uint8_t> mask(height, width);
  for (double y = 0; y < height; ++y) {
    for (double x = 0; x < width; ++x) {
      Camera::Vector2 pixel = {x + 0.5, y + 0.5};
      mask(y, x) = camera.isOutsideImageCircle(pixel) ? 0 : 255;
    }
  }
  return mask;
}

std::vector<Keypoint> findCorners(const Camera& camera, const Image& image, const bool useNearest) {
  LOG(INFO) << folly::sformat("Processing camera {}... ", camera.id);

  // Search for features at multiple scales
  int rejectedCorners = 0;
  int deduplicatedCorners = 0;
  std::vector<Keypoint> corners;

  // if we're comparing across a single scale, we don't rescale while finding corners
  int octaveCount = FLAGS_same_scale ? 1 : FLAGS_octave_count;
  const cv::Mat_<uint8_t>& mask = generateImageCircleMask(camera);
  for (int octave = 0; octave < octaveCount; ++octave) {
    double scale = std::pow(0.5, octave);
    std::vector<Camera::Vector2> octaveCorners = findScaledCorners(scale, image, mask, camera.id);
    if (FLAGS_log_verbose) {
      LOG(INFO) << folly::sformat(
          "{} found {} corners at scale {}", camera.id, octaveCorners.size(), scale);
    }
    int cornerCountBeforeOctave = corners.size();
    for (const Camera::Vector2& octaveCorner : octaveCorners) {
      if (isCloseToEdge(octaveCorner, image, FLAGS_zncc_window_radius)) {
        rejectedCorners++;
      } else if (!isUniqueCorner(corners, cornerCountBeforeOctave, octaveCorner)) {
        deduplicatedCorners++;
      } else {
        corners.emplace_back(octaveCorner, image, FLAGS_zncc_window_radius, useNearest);
      }
    }
  }

  if (FLAGS_deduplicate_radius != 0) {
    LOG(INFO) << folly::sformat(
        "{} accepted corners: {} deduplicated corners: {} rejected corners {}",
        camera.id,
        corners.size(),
        deduplicatedCorners,
        rejectedCorners);
  } else {
    LOG(INFO) << folly::sformat(
        "{} accepted corners: {} rejected corners {}", camera.id, corners.size(), rejectedCorners);
  }

  return corners;
}

std::map<ImageId, std::vector<Keypoint>>
findAllCorners(const Camera::Rig& rig, const std::vector<Image>& images, const bool useNearest) {
  std::map<ImageId, std::vector<Keypoint>> allCorners;
  boost::timer::cpu_timer featureTimer;
  ThreadPool threadPool(FLAGS_threads);
  for (int currentCamera = 0; currentCamera < ssize(rig); currentCamera++) {
    const Camera& camera = rig[currentCamera];
    std::vector<Keypoint>& keypoints = allCorners[camera.id]; // modify allCorners in main thread
    const Image& image = images[currentCamera];
    threadPool.spawn([&keypoints, &camera, &image, &useNearest] {
      keypoints = findCorners(camera, image, useNearest);
    });
  }
  threadPool.join();

  if (FLAGS_enable_timing) {
    LOG(INFO) << folly::sformat("Find corners stage time: {}", featureTimer.format());
  }

  return allCorners;
}

} // namespace fb360_dep::calibration
