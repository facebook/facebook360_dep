/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <opencv2/opencv.hpp>

#include <folly/Format.h>

#include "source/util/Camera.h"
#include "source/util/ImageUtil.h"

using namespace fb360_dep;
using namespace fb360_dep::image_util;

const std::string kUsageMessage = R"(
   - Aligns colors using separate (calibrated) color rigs.

   - Example:
     ./AlignColors \
     --output=/path/to/output \
     --color=/path/to/video/color \
     --calibrated_rig=/path/to/rigs/rig_calibrated.json \
     --rig_blue=/path/to/rigs/rig_blue.json \
     --rig_green=/path/to/rigs/rig_green.json \
     --rig_red=/path/to/rigs/rig_red.json
 )";

DEFINE_string(calibrated_rig, "", "path to calibrated green rig .json filename (required)");
DEFINE_string(cameras, "", "cameras to align (comma-separated)");
DEFINE_string(color, "", "path to input color images (required)");
DEFINE_string(first, "", "first frame to process (lexical)");
DEFINE_string(last, "", "last frame to process (lexical)");
DEFINE_string(output, "", "path to output directory (must be different than color path)");
DEFINE_string(rig_blue, "", "path to camera blue rig .json filename (required)");
DEFINE_string(rig_green, "", "path to camera green rig .json filename (required)");
DEFINE_string(rig_red, "", "path to camera red rig .json filename (required)");

using Image = cv::Mat_<cv::Vec3w>;
using WarpMap = cv::Mat_<cv::Point2f>;

cv::Vec3w getPixelColor(const Image& srcImage, const cv::Point2f& srcPixel) {
  return cv_util::getPixelBilinear(srcImage, srcPixel.x, srcPixel.y);
}

WarpMap createWarpMap(const Camera& srcCamera, const Camera& dstCamera) {
  const int width = int(srcCamera.resolution.x());
  const int height = int(srcCamera.resolution.y());
  cv::Mat_<cv::Point2f> warpMap(height, width);
  for (double x = 0.5; x < width; ++x) {
    for (double y = 0.5; y < height; ++y) {
      const Camera::Vector2 dstPixel(x, y);
      const double dstR = (dstPixel - dstCamera.principal).norm() / dstCamera.getScalarFocal();

      const double theta = dstCamera.undistort(dstR);
      const double srcR = srcCamera.distort(theta);
      const Camera::Vector2 srcPixel = (dstPixel - dstCamera.principal) /
              (dstPixel - dstCamera.principal).norm() * srcCamera.getScalarFocal() * srcR +
          dstCamera.principal;
      warpMap(y, x) = cv::Point2f(srcPixel.x(), srcPixel.y());
    }
  }
  return warpMap;
}

void createCalibratedRBRigs(
    Camera::Rig& calibratedRedRig,
    Camera::Rig& calibratedBlueRig,
    const Camera::Rig& referenceRedCamera,
    const Camera::Rig& referenceGreenCamera,
    const Camera::Rig& referenceBlueCamera,
    const Camera::Rig& calibratedGreenRig) {
  const double referenceGreenFocal = referenceGreenCamera[0].getScalarFocal();
  const double referenceRedFocal = referenceRedCamera[0].getScalarFocal();
  const double referenceBlueFocal = referenceBlueCamera[0].getScalarFocal();
  for (auto& calibratedGreenCamera : calibratedGreenRig) {
    Camera calibratedRedCamera = calibratedGreenCamera;
    Camera calibratedBlueCamera = calibratedGreenCamera;
    const double calibratedGreenFocal = calibratedGreenCamera.getScalarFocal();

    const float focalRatio = calibratedGreenFocal / referenceGreenFocal;
    calibratedRedCamera.setScalarFocal(referenceRedFocal * focalRatio);
    calibratedRedCamera.setDistortion(referenceRedCamera[0].getDistortion());
    calibratedBlueCamera.setScalarFocal(referenceBlueFocal * focalRatio);
    calibratedBlueCamera.setDistortion(referenceBlueCamera[0].getDistortion());

    calibratedRedRig.emplace_back(calibratedRedCamera);
    calibratedBlueRig.emplace_back(calibratedBlueCamera);
  }
}

Image warpImage(
    const Image& currentImage,
    const WarpMap& redWarpMap,
    const WarpMap& blueWarpMap,
    const int width,
    const int height) {
  Image alignedImage(height, width);
  for (double x = 0.5; x < width; ++x) {
    for (double y = 0.5; y < height; ++y) {
      cv::Vec3w alignedColor(0, 0, 0);
      alignedColor[1] = currentImage(y, x)[1];
      cv::Vec3w interpolatedRed = getPixelColor(currentImage, redWarpMap(y, x));
      alignedColor[2] = interpolatedRed[2];
      cv::Vec3w interpolatedBlue = getPixelColor(currentImage, blueWarpMap(y, x));
      alignedColor[0] = interpolatedBlue[0];
      alignedImage(y, x) = alignedColor;
    }
  }
  return alignedImage;
}

int main(int argc, char* argv[]) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_color, "");
  CHECK_NE(FLAGS_calibrated_rig, "");
  CHECK_NE(FLAGS_rig_red, "");
  CHECK_NE(FLAGS_rig_green, "");
  CHECK_NE(FLAGS_rig_blue, "");

  CHECK_NE(FLAGS_color, FLAGS_output);

  // Load calibrated rig
  const Camera::Rig calibratedGreenRig =
      filterDestinations(Camera::loadRig(FLAGS_calibrated_rig), FLAGS_cameras);

  // Load reference camera models for each color
  const Camera::Rig redRig = Camera::loadRig(FLAGS_rig_red);
  const Camera::Rig greenRig = Camera::loadRig(FLAGS_rig_green);
  const Camera::Rig blueRig = Camera::loadRig(FLAGS_rig_blue);

  // Create calibrated rigs for red and blue by rescaling the focal parameter
  Camera::Rig calibratedRedRig;
  Camera::Rig calibratedBlueRig;
  createCalibratedRBRigs(
      calibratedRedRig, calibratedBlueRig, redRig, greenRig, blueRig, calibratedGreenRig);

  std::vector<WarpMap> redWarpMaps;
  std::vector<WarpMap> blueWarpMaps;
  for (unsigned long i = 0; i < calibratedGreenRig.size(); ++i) {
    redWarpMaps.emplace_back(createWarpMap(calibratedRedRig[i], calibratedGreenRig[i]));
    blueWarpMaps.emplace_back(createWarpMap(calibratedBlueRig[i], calibratedGreenRig[i]));
  }

  std::pair<int, int> frameRange =
      getFrameRange(FLAGS_color, calibratedGreenRig, FLAGS_first, FLAGS_last);

  for (int iFrame = frameRange.first; iFrame <= frameRange.second; ++iFrame) {
    const std::string frameName = intToStringZeroPad(iFrame);
    LOG(INFO) << folly::sformat("Loading frame {}", frameName);

    // Load input color images
    const std::vector<Image> images =
        loadImages<cv::Vec3w>(FLAGS_color, calibratedGreenRig, frameName);
    CHECK_GT(images.size(), 0) << "no images loaded!";
    CHECK_EQ(images.size(), calibratedGreenRig.size());
    CHECK_EQ(greenRig.size(), 1);
    CHECK_EQ(redRig.size(), 1);
    CHECK_EQ(blueRig.size(), 1);

    // Align to green
    for (ssize_t imageIndex = 0; imageIndex < ssize(images); ++imageIndex) {
      LOG(INFO) << folly::sformat("Aligning camera : {}", calibratedGreenRig[imageIndex].id);
      const int width = int(calibratedGreenRig[imageIndex].resolution.x());
      const int height = int(calibratedGreenRig[imageIndex].resolution.y());
      const Image& alignedImage = warpImage(
          images[imageIndex], redWarpMaps[imageIndex], blueWarpMaps[imageIndex], width, height);

      const filesystem::path camDir =
          filesystem::path(FLAGS_output) / calibratedGreenRig[imageIndex].id;
      filesystem::create_directories(camDir);
      const std::string outputFile = folly::sformat("{}/{}.png", camDir.string(), frameName);
      cv_util::imwriteExceptionOnFail(outputFile, cv_util::convertTo<uint16_t>(alignedImage));
    }
  }

  return EXIT_SUCCESS;
}
