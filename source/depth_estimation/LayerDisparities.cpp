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

#include "source/depth_estimation/DerpUtil.h"
#include "source/util/Camera.h"
#include "source/util/ImageTypes.h"
#include "source/util/ImageUtil.h"

using namespace fb360_dep;
using namespace fb360_dep::image_util;

const std::string kUsageMessage = R"(
   - Layers foreground disparity atop background disparity assuming nans to correspond to locations
   without valid disparities.

   - Example:
     ./LayerDisparities \
     --rig=/path/to/rigs/rig.json \
     --background_disp=/path/to/background/disparity \
     --foreground_disp=/path/to/output/disparity \
     --output=/path/to/output \
     --first=000000 \
     --last=000000
 )";

DEFINE_string(background_disp, "", "path to background disparity directory (required)");
DEFINE_string(background_frame, "000000", "background frame to process (lexical)");
DEFINE_string(cameras, "", "destination cameras");
DEFINE_string(first, "000000", "first frame to process (lexical)");
DEFINE_string(foreground_disp, "", "path to foreground disparity directory (required)");
DEFINE_string(last, "000000", "last frame to process (lexical)");
DEFINE_string(output, "", "path to output disparity directory");
DEFINE_string(rig, "", "path to camera rig .json (required)");
DEFINE_int32(threads, -1, "number of threads (-1 = auto, 0 = none)");

void layerDisparities(
    const cv::Mat_<float>& foregroundImage,
    const cv::Mat_<float>& backgroundImage,
    const filesystem::path& outputPath) {
  CHECK(foregroundImage.size() == backgroundImage.size())
      << "Background and foreground images must be of the same size!";
  cv::Mat_<float> mask(foregroundImage.size());
  cv::threshold(foregroundImage, mask, 0.0, 1.0, cv::THRESH_BINARY);
  cv::Mat_<float> layerImage = foregroundImage.mul(mask) + backgroundImage.mul(1 - mask);
  cv_util::imwriteExceptionOnFail(outputPath, layerImage * 255);
}

int main(int argc, char* argv[]) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_rig, "");
  CHECK_NE(FLAGS_background_disp, "");
  CHECK_NE(FLAGS_foreground_disp, "");
  CHECK_LE(FLAGS_first, FLAGS_last);

  const Camera::Rig rigSrc = Camera::loadRig(FLAGS_rig);
  Camera::Rig rigDst = image_util::filterDestinations(rigSrc, FLAGS_cameras);

  const std::vector<cv::Mat_<float>> backgroundDisparities =
      loadImages<float>(FLAGS_background_disp, rigDst, FLAGS_background_frame, FLAGS_threads);
  const int numFrames = std::stoi(FLAGS_last) - std::stoi(FLAGS_first) + 1;

  for (int iFrame = 0; iFrame < numFrames; ++iFrame) {
    const std::string frameName =
        image_util::intToStringZeroPad(iFrame + std::stoi(FLAGS_first), 6);
    const std::vector<cv::Mat_<float>> foregroundDisparities =
        loadImages<float>(FLAGS_foreground_disp, rigDst, frameName, FLAGS_threads);

    for (ssize_t camIdx = 0; camIdx < ssize(rigDst); ++camIdx) {
      const filesystem::path outputDir =
          depth_estimation::getImageDir(FLAGS_output, ImageType::disparity, rigDst[camIdx].id);
      boost::filesystem::create_directories(outputDir);
      const filesystem::path outputPath = outputDir / (frameName + ".png");
      layerDisparities(foregroundDisparities[camIdx], backgroundDisparities[camIdx], outputPath);
    }
  }

  return EXIT_SUCCESS;
}
