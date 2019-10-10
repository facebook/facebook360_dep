/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/dynamic.h>

#include "source/util/Camera.h"
#include "source/util/CvUtil.h"

namespace fb360_dep {
namespace calibration {

inline std::string
getImageFilename(const std::string& image, const std::string& frame, const std::string& imageExt) {
  return folly::sformat("{}/{}{}", image, frame, imageExt);
}

struct Keypoint {
  Camera::Vector2 coords;
  double avg;
  double std;
  cv::Mat_<uint8_t> patch;

  Keypoint(
      const Camera::Vector2& coords,
      const cv::Mat_<uint8_t>& img,
      const int windowRadius,
      const bool useNearest)
      : coords(coords) {
    patch = cv::Mat_<uint8_t>(2 * windowRadius + 1, 2 * windowRadius + 1);
    for (int xOffset = -windowRadius; xOffset <= windowRadius; ++xOffset) {
      for (int yOffset = -windowRadius; yOffset <= windowRadius; ++yOffset) {
        patch(yOffset + windowRadius, xOffset + windowRadius) = useNearest
            ? img(int(coords.y() + yOffset), int(coords.x() + xOffset))
            : cv_util::getPixelBilinear(img, coords.x() + xOffset, coords.y() + yOffset);
      }
    }
    initializeAvgStd();
  }

  // This creates a fake keypoint by directly providing a patch without coords
  Keypoint(cv::Mat_<uint8_t>& interpolatedPatch) : coords(NAN, NAN) {
    interpolatedPatch.copyTo(patch);
    initializeAvgStd();
  }

  folly::dynamic serialize() const {
    return folly::dynamic::object("x", coords.x())("y", coords.y());
  }

  static folly::dynamic serializeVector(const std::vector<Keypoint>& corners) {
    folly::dynamic cornerArray = folly::dynamic::array();
    for (const Keypoint& corner : corners) {
      cornerArray.push_back(corner.serialize());
    }
    return cornerArray;
  }

  static folly::dynamic serializeRig(
      const std::map<std::string, std::vector<Keypoint>>& allCorners,
      const std::string& frame,
      const std::string& imageExt) {
    folly::dynamic allCornersData = folly::dynamic::object;
    for (const auto& cameraCorners : allCorners) {
      allCornersData[getImageFilename(cameraCorners.first, frame, imageExt)] =
          serializeVector(cameraCorners.second);
    }
    return allCornersData;
  }

 private:
  void initializeAvgStd() {
    cv::Scalar_<double> cvMean;
    cv::Scalar_<double> cvStd;
    cv::meanStdDev(patch, cvMean, cvStd);
    avg = cvMean[0];
    std = cvStd[0];
  }
};

struct Match {
  double score;
  std::array<int, 2> corners;

  Match(const double score, const int corner0, const int corner1) : score(score) {
    corners[0] = corner0;
    corners[1] = corner1;
  }

  folly::dynamic serialize() const {
    folly::dynamic result =
        folly::dynamic::object("idx1", corners[0])("idx2", corners[1])("score", score);
    return result;
  }
};

struct Overlap {
  std::array<std::string, 2> images;
  std::vector<Match> matches;

  Overlap() {};

  Overlap(const std::string& image0, const std::string& image1) {
    images[0] = image0;
    images[1] = image1;
  }

  folly::dynamic serialize(const std::string& frame, const std::string& imageExt) const {
    folly::dynamic overlapData = folly::dynamic::object;
    overlapData["image1"] = getImageFilename(images[0], frame, imageExt);
    overlapData["image2"] = getImageFilename(images[1], frame, imageExt);
    overlapData["matches"] = folly::dynamic::array();
    for (const auto& match : matches) {
      overlapData["matches"].push_back(match.serialize());
    }
    return overlapData;
  }
};

} // namespace calibration
} // namespace fb360_dep
