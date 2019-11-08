/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <tuple>
#include <vector>

#include "source/util/Camera.h"
#include "source/util/CvUtil.h"
#include "source/util/FilesystemUtil.h"
#include "source/util/ImageTypes.h"

namespace fb360_dep {
namespace image_util {

int getSingleFrame(const filesystem::path& imageDir, const Camera::Rig& rig, std::string frame);

std::pair<int, int> getFrameRange(
    const filesystem::path& imageDir,
    const Camera::Rig& rig,
    std::string firstFrame,
    std::string lastFrame);

void verifyImagePaths(
    const filesystem::path& imageDir,
    const Camera::Rig& rig,
    const std::string& firstFrame,
    const std::string& lastFrame,
    const std::string& extension = "");

double probeDisparity(
    const int probe,
    const int probeCount,
    const double minDisparity,
    const double maxDisparity);

inline std::string intToStringZeroPad(const int x, const int padlen = 6) {
  std::ostringstream ss;
  ss << std::setw(padlen) << std::setfill('0') << x;
  return ss.str();
}

inline const filesystem::path imagePath(
    const filesystem::path& dir,
    const std::string& camId,
    const std::string& frameName,
    const std::string& extension = "") {
  filesystem::path camDir = dir / camId;
  std::string ext = extension.empty() ? filesystem::getFirstExtension(camDir) : extension;
  return camDir / (frameName + ext);
}

template <typename T>
cv::Mat_<T>
loadImage(const filesystem::path& dir, const std::string& camId, const std::string& frameName) {
  return cv_util::loadImage<T>(imagePath(dir, camId, frameName));
}

template <typename T>
std::vector<cv::Mat_<T>> loadImages(
    const filesystem::path& dir,
    const Camera::Rig& rig,
    const std::string& frameName,
    const int numThreads = -1) {
  std::vector<cv::Mat_<T>> images(rig.size());
  ThreadPool threadPool(numThreads);
  for (int i = 0; i < int(rig.size()); ++i) {
    threadPool.spawn([&, i] { images[i] = loadImage<T>(dir, rig[i].id, frameName); });
  }
  threadPool.join();
  return images;
}

template <typename T>
std::vector<cv::Mat_<T>> loadLevelImages(
    const filesystem::path& dir,
    const int level,
    const Camera::Rig& rig,
    const std::string& frameName,
    const int numThreads = -1) {
  std::vector<cv::Mat_<T>> images(rig.size());
  ThreadPool threadPool(numThreads);
  const filesystem::path dirLevel = dir / ("level_" + std::to_string(level));
  for (int i = 0; i < int(rig.size()); ++i) {
    threadPool.spawn([&, i] { images[i] = loadImage<T>(dirLevel, rig[i].id, frameName); });
  }
  threadPool.join();
  return images;
}

inline cv::Mat_<float>
loadPfmImage(const filesystem::path& dir, const std::string& camId, const std::string& frameName) {
  return cv_util::readCvMat32FC1FromPFM(imagePath(dir, camId, frameName, ".pfm"));
}

inline std::vector<cv::Mat_<float>> loadPfmImages(
    const filesystem::path& dir,
    const Camera::Rig& rig,
    const std::string& frameName,
    const int numThreads = -1) {
  std::vector<cv::Mat_<float>> images(rig.size());
  ThreadPool threadPool(numThreads);
  for (int i = 0; i < int(rig.size()); ++i) {
    threadPool.spawn([&, i] { images[i] = loadPfmImage(dir, rig[i].id, frameName); });
  }
  threadPool.join();
  return images;
}

template <typename T>
cv::Mat_<T> loadScaledImage(
    const filesystem::path& dir,
    const std::string& camId,
    const std::string& frameName,
    const double scaleFactor,
    const int interp = cv::INTER_AREA) {
  return cv_util::loadScaledImage<T>(imagePath(dir, camId, frameName), scaleFactor, interp);
}

template <typename T>
std::vector<cv::Mat_<T>> loadScaledImages(
    const filesystem::path& dir,
    const Camera::Rig& rig,
    const std::string& frameName,
    const double scaleFactor,
    const int interp = cv::INTER_AREA,
    const int numThreads = -1) {
  std::vector<cv::Mat_<T>> images(rig.size());
  ThreadPool threadPool(numThreads);
  for (int i = 0; i < int(rig.size()); ++i) {
    threadPool.spawn(
        [&, i] { images[i] = loadScaledImage<T>(dir, rig[i].id, frameName, scaleFactor, interp); });
  }
  threadPool.join();
  return images;
}

template <typename T>
cv::Mat_<T> loadResizedImage(
    const filesystem::path& dir,
    const std::string& camId,
    const std::string& frameName,
    const cv::Size& size,
    const int interp = cv::INTER_AREA) {
  return cv_util::loadResizedImage<T>(imagePath(dir, camId, frameName), size, interp);
}

template <typename T>
std::vector<cv::Mat_<T>> loadResizedImages(
    const filesystem::path& dir,
    const Camera::Rig& rig,
    const std::string& frameName,
    const cv::Size& size,
    const int interp = cv::INTER_AREA,
    const int numThreads = -1) {
  std::vector<cv::Mat_<T>> images(rig.size());
  ThreadPool threadPool(numThreads);
  for (int i = 0; i < int(rig.size()); ++i) {
    threadPool.spawn(
        [&, i] { images[i] = loadResizedImage<T>(dir, rig[i].id, frameName, size, interp); });
  }
  threadPool.join();
  return images;
}

Camera::Rig filterDestinations(const Camera::Rig rigIn, const std::string& destinations);

Camera::Vector2 worldToEquirect(const Camera::Vector3 world, const int eqrW, const int eqrH);

cv::Mat_<cv::Vec2f> computeWarpDstToSrc(const Camera& dst, const Camera& src);

} // namespace image_util
} // end namespace fb360_dep
