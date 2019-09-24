/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdlib.h>
#include <string>

#include <gtest/gtest.h>

#include <folly/FileUtil.h>
#include <folly/Format.h>
#include <folly/json.h>

#include "source/calibration/Calibration.h"
#include "source/util/Camera.h"
#include "source/util/CvUtil.h"

DECLARE_int32(min_features); // required to prevent MatchCorners from throwing "Too few features"

namespace fb360_dep {

using PixelType = cv::Vec4b;

struct MatchCornersTest : ::testing::Test {};

static const char* testRigJson = R"({
  "cameras" : [
    {
      "id" : "cam",
      "origin" : [
        0.2513105810211681,
        0.07628069635674192,
        0.19981052832654608
      ],
      "right" : [
        -0.25596142689619755,
        -0.7510205805560425,
        0.6086475462223826
      ],
      "up" : [
        -0.5954238716565995,
        0.618495019447276,
        0.5127712199220811
      ],
      "forward" : [
        0.7615472152156608,
        0.23115362532346037,
        0.605486449474382
      ],
      "focal" : [
        1094.418432,
        -1094.418432
      ],
      "resolution" : [
        3360,
        2160
      ],
      "type" : "FTHETA",
      "version" : 1
    }
  ]
})";

std::vector<Camera::Vector2> loadCorners(const std::string& path) {
  std::string json;
  folly::readFile(path.c_str(), json);
  folly::dynamic parsed = folly::parseJson(json);

  std::vector<Camera::Vector2> corners;
  for (const auto& image : parsed["images"].items()) {
    for (const auto& corner : image.second) {
      corners.push_back(Camera::Vector2(corner["x"].asDouble(), corner["y"].asDouble()));
    }
  }
  return corners;
}

cv::Mat_<PixelType> rotateImage(const cv::Mat_<PixelType>& image, const float angle) {
  cv::Point2f center((image.cols - 1) / 2.0, (image.rows - 1) / 2.0); // OpenCV center for rotation
  cv::Mat_<float> rotMat = cv::getRotationMatrix2D(center, angle, 1.0);
  cv::Mat_<PixelType> dst(image.size());
  cv::warpAffine(image, dst, rotMat, image.size(), cv::INTER_AREA);
  return dst;
}

Camera::Vector2
rotatePoint(const cv::Mat_<PixelType>& image, const Camera::Vector2& pt, const float angle) {
  cv::Point2f center((image.cols - 1) / 2.0, (image.rows - 1) / 2.0); // OpenCV center for rotation
  cv::Mat_<float> rotMat = cv::getRotationMatrix2D(center, angle, 1.0);
  cv::Point2f cvPt(pt.x() - 0.5f, pt.y() - 0.5f); // convert to OpenCV for rotation
  float rotX = rotMat(0, 0) * cvPt.x + rotMat(0, 1) * cvPt.y + rotMat(0, 2);
  float rotY = rotMat(1, 0) * cvPt.x + rotMat(1, 1) * cvPt.y + rotMat(1, 2);
  return Camera::Vector2(rotX + 0.5f, rotY + 0.5f); // convert out of OpenCV coords
}

cv::Mat_<PixelType> translate(const cv::Mat_<PixelType>& image, const float t_x, const float t_y) {
  cv::Mat_<float> transMat = (cv::Mat_<float>(2, 3) << 1, 0, t_x, 0, 1, t_y);
  cv::Mat_<PixelType> dst(image.size());
  cv::warpAffine(image, dst, transMat, image.size());
  return dst;
}

void insertIfInsideImage(
    std::vector<Camera::Vector2>& trueCorners,
    const cv::Mat_<PixelType>& image,
    const Camera::Vector2& corner) {
  if (corner.x() > 0 && corner.x() < image.cols && corner.y() > 0 && corner.y() < image.rows) {
    trueCorners.push_back(corner);
  }
}

TEST(MatchCornersTest, TestTransformationDetection) {
  // flags that need to be defined for calls to MatchCorners
  FLAGS_color = boost::filesystem::unique_path("test_%%%%%%").string();
  FLAGS_frame = "000000";
  const std::string matches_basenane = boost::filesystem::unique_path("matches_%%%%%%").string();
  FLAGS_matches = folly::sformat("{}/{}.json", FLAGS_color, matches_basenane);
  const std::string rig_basenane = boost::filesystem::unique_path("rig_%%%%%%").string();
  FLAGS_rig_in = folly::sformat("{}/{}.json", FLAGS_color, rig_basenane);
  FLAGS_min_features = 0;

  static const int squareDim = 300;
  static const int xGap = 200;
  static const int yGap = 200;
  static const int rows = 3;
  static const int cols = 5;
  static const double tolerance = 0.25;

  static const double angle = 1.0;
  static const double tX = 5.0;
  static const double tY = 10.0;

  Camera::Rig rig = Camera::loadRigFromJsonString(testRigJson);
  const Camera::Vector2& resolution = rig[0].resolution;

  cv::Mat_<PixelType> image(resolution.y(), resolution.x());
  image.setTo(cv::Scalar(0, 0, 0, 1));

  int xJump = xGap + squareDim;
  int yJump = yGap + squareDim;
  int xOffset = (resolution.x() - cols * squareDim - (cols - 1) * xGap) / 2;
  int yOffset = (resolution.y() - rows * squareDim - (rows - 1) * yGap) / 2;

  std::vector<Camera::Vector2> trueCorners;
  Camera::Vector2 translation(tX, tY);
  for (int i = 0; i < cols; ++i) {
    for (int j = 0; j < rows; ++j) {
      std::vector<Camera::Vector2> boxOffsets = {Camera::Vector2(0, 0),
                                                 Camera::Vector2(squareDim, 0),
                                                 Camera::Vector2(0, squareDim),
                                                 Camera::Vector2(squareDim, squareDim)};

      Camera::Vector2 topLeft(xOffset + i * xJump, yOffset + j * yJump);
      image(cv::Rect(topLeft.x(), topLeft.y(), squareDim, squareDim)) =
          PixelType(rand() % 256, rand() % 256, rand() % 256, 1);
      for (const auto& offset : boxOffsets) {
        Camera::Vector2 transformedPoint =
            rotatePoint(image, topLeft + offset, angle) + translation;
        insertIfInsideImage(trueCorners, image, transformedPoint);
      }
    }
  }

  image = rotateImage(image, angle);
  image = translate(image, tX, tY);

  std::string testPath = folly::sformat("{}/cam/", FLAGS_color);
  boost::filesystem::create_directories(testPath);

  cv_util::imwriteExceptionOnFail(folly::sformat("{}/{}.png", testPath, FLAGS_frame), image);
  Camera::saveRig(FLAGS_rig_in, rig);

  matchCorners();
  std::vector<Camera::Vector2> corners = loadCorners(FLAGS_matches);

  CHECK_EQ(corners.size(), trueCorners.size());
  float toleranceSq = tolerance * tolerance;
  for (const Camera::Vector2& corner : corners) {
    float bestDistanceSq = FLT_MAX;
    Camera::Vector2 bestTrueCorner;
    for (const Camera::Vector2& trueCorner : trueCorners) {
      float distanceSq = (trueCorner - corner).squaredNorm();
      if (distanceSq < bestDistanceSq) {
        bestDistanceSq = distanceSq;
        bestTrueCorner = trueCorner;
      }
    }
    CHECK_LE(bestDistanceSq, toleranceSq) << folly::sformat(
        "No corners near ({}, {}). Closest found: ({}, {})",
        corner.x(),
        corner.y(),
        bestTrueCorner.x(),
        bestTrueCorner.y());
  }

  boost::filesystem::remove_all(FLAGS_color);
  boost::filesystem::remove_all(FLAGS_rig_in);
  boost::filesystem::remove_all(FLAGS_matches);
}

} // namespace fb360_dep
