/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/util/ImageUtil.h"

#include <boost/algorithm/string/split.hpp>

namespace fb360_dep::image_util {

static inline const std::vector<filesystem::path> checkAndGetSortedFiles(
    const filesystem::path& imageDir,
    const Camera::Rig& rig) {
  CHECK_GT(rig.size(), 0);
  filesystem::path camDir = imageDir / rig[0].id;
  CHECK(filesystem::exists(camDir)) << folly::sformat("No folder found at {}", camDir.string());
  const bool includeHidden = false;
  const std::vector<filesystem::path> sortedFiles =
      filesystem::getFilesSorted(camDir, includeHidden);
  CHECK_GT(sortedFiles.size(), 0) << folly::sformat("No files found in {}", camDir.string());
  return sortedFiles;
}

// Get first lexical frame if flag isn't filled in and validate the frame
int getSingleFrame(const filesystem::path& imageDir, const Camera::Rig& rig, std::string frame) {
  if (frame == "") {
    const std::vector<filesystem::path>& sortedFiles = checkAndGetSortedFiles(imageDir, rig);
    frame = sortedFiles.front().stem().string();
  }
  verifyImagePaths(imageDir, rig, frame, frame);
  return std::stoi(frame);
}

// Gets first and last lexical frame if flags aren't filled in and
// validates the frame range
std::pair<int, int> getFrameRange(
    const filesystem::path& imageDir,
    const Camera::Rig& rig,
    std::string firstFrame,
    std::string lastFrame) {
  if (firstFrame == "" || lastFrame == "") {
    const std::vector<filesystem::path>& sortedFiles = checkAndGetSortedFiles(imageDir, rig);
    if (firstFrame == "") {
      firstFrame = sortedFiles.front().stem().string();
    }
    if (lastFrame == "") {
      lastFrame = sortedFiles.back().stem().string();
    }
  }

  verifyImagePaths(imageDir, rig, firstFrame, lastFrame);

  return std::make_pair<int, int>(std::stoi(firstFrame), std::stoi(lastFrame));
}

// Generates paths to images, one vector per camera
void verifyImagePaths(
    const filesystem::path& imageDir,
    const Camera::Rig& rig,
    const std::string& firstFrame,
    const std::string& lastFrame,
    const std::string& extension) {
  int first;
  int last;
  try {
    first = std::stoi(firstFrame);
  } catch (...) {
    CHECK(false) << "Invalid frame name: " << firstFrame;
  }
  try {
    last = std::stoi(lastFrame);
  } catch (...) {
    CHECK(false) << "Invalid frame name: " << lastFrame;
  }

  CHECK_LE(first, last);
  CHECK_GT(rig.size(), 0);
  const std::string extGuess = filesystem::getFirstExtension(imageDir / rig[0].id);
  const std::string ext = !extension.empty() ? extension : extGuess;
  for (const Camera& cam : rig) {
    const filesystem::path camDir = imageDir / cam.id;
    for (int frameNum = first; frameNum <= last; ++frameNum) {
      const std::string frameName = intToStringZeroPad(frameNum, 6);
      const filesystem::path p = camDir / (frameName + ext);
      const bool exists = filesystem::is_regular_file(p);
      CHECK(exists) << "Missing file: " << p;
    }
  }
}

// Returns a value between min disparity and max disparity.
// Since we take non-rational steps, we cannot guarantee perfect precision on
// ends of the range, but we can on the first one (probe = 0), so we choose it
// to be at the furthest disparity (= closest depth)
double probeDisparity(
    const int probe,
    const int probeCount,
    const double minDisparity,
    const double maxDisparity) {
  const double fraction = double(probe) / double(probeCount - 1);
  return fraction * minDisparity + (1 - fraction) * maxDisparity;
}

// Assuming comma-separated list of destinations as input
Camera::Rig filterDestinations(const Camera::Rig rigIn, const std::string& destinations) {
  Camera::Rig rigOut;
  if (destinations.empty()) {
    return rigIn;
  }
  std::vector<std::string> destVec;
  boost::split(destVec, destinations, [](char c) { return c == ','; });
  for (const std::string& dest : destVec) {
    for (const auto& cam : rigIn) {
      if (cam.id == dest) {
        rigOut.push_back(cam);
      }
    }
  }
  return rigOut;
}

Camera::Vector2 worldToEquirect(const Camera::Vector3 world, const int eqrW, const int eqrH) {
  const float depth = world.norm();
  const float x = world.x() / depth;
  const float y = world.y() / depth;
  const float z = world.z() / depth;
  const float phi = acos(z);
  float theta = atan2(y, x);
  if (theta > 0) {
    theta -= 2 * M_PI;
  }
  const float v = phi / M_PI;
  const float u = -theta / (2.0f * M_PI);
  return Camera::Vector2(u * eqrW, v * eqrH);
}

cv::Mat_<cv::Vec2f> computeWarpDstToSrc(const Camera& dst, const Camera& src) {
  const cv::Size srcSize(src.resolution.x(), src.resolution.y());
  const cv::Size dstSize(dst.resolution.x(), dst.resolution.y());
  cv::Mat_<cv::Vec2f> warpMap(dstSize, cv::Vec2f(NAN, NAN));

  if (dst.id == src.id) {
    return warpMap;
  }

  for (int y = 0; y < warpMap.rows; ++y) {
    for (int x = 0; x < warpMap.cols; ++x) {
      const Camera::Vector2 dstPixel(x + 0.5, y + 0.5);
      if (dst.isOutsideImageCircle(dstPixel)) {
        continue;
      }
      const Camera::Vector3 rig = dst.rigNearInfinity(dstPixel);
      Camera::Vector2 srcPixel;
      if (!src.sees(rig, srcPixel)) {
        continue;
      }
      // note: convert to opencv coordinate convention because it must be fed to cv::remap later
      warpMap(y, x) = cv::Vec2f(srcPixel.x() - 0.5f, srcPixel.y() - 0.5f);
    }
  }
  return warpMap;
}

} // namespace fb360_dep::image_util
