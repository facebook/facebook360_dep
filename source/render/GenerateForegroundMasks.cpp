/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/Format.h>

#include "source/render/BackgroundSubtractionUtil.h"
#include "source/util/CvUtil.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"
#include "source/util/ThreadPool.h"

using namespace fb360_dep;
using namespace fb360_dep::image_util;

using PixelType = cv::Vec3w;
using PixelTypeFloat = cv::Vec3f;

const std::string kUsageMessage = R"(
   - Generates foreground masks for a series of frames assuming a fixed background. Various
   parameters can be tweaked to improve the mask accuracy.

   - Example:
     ./GenerateForegroundMasks \
     --first=000000 \
     --last=000000 \
     --rig=/path/to/rigs/rig.json \
     --color=/path/to/video/color \
     --background_color=/path/to/background/color \
     --foreground_masks=/path/to/video/output
 )";

DEFINE_string(background_color, "", "path to input background color images (required)");
DEFINE_string(background_frame, "000000", "background frame (lexical)");
DEFINE_int32(blur_radius, 1, "Gaussian blur radius (0 = no blur)");
DEFINE_string(cameras, "", "comma-separated cameras to render (empty for all)");
DEFINE_string(color, "", "path to input color images (required)");
DEFINE_string(first, "", "first frame to process (lexical) (required)");
DEFINE_string(foreground_masks, "", "path to output foreground masks (required)");
DEFINE_string(last, "", "last frame to process (lexical) (required)");
DEFINE_int32(morph_closing_size, 4, "Morphological closing size (0 = no closing)");
DEFINE_string(rig, "", "path to camera rig .json (required)");
DEFINE_int32(threads, -1, "number of threads (-1 = max allowed, 0 = no threading)");
DEFINE_double(threshold, 0.04, "foreground/background RGB L2-norm threshold [0..1]");
DEFINE_int32(width, 2048, "optional downscaled output width");

void verifyInputs() {
  CHECK_NE(FLAGS_color, "");
  CHECK_NE(FLAGS_rig, "");
  CHECK_NE(FLAGS_background_color, "");
  CHECK_NE(FLAGS_foreground_masks, "");
  CHECK_NE(FLAGS_first, "");
  CHECK_NE(FLAGS_last, "");
  CHECK_NE(FLAGS_background_frame, "");
  CHECK_GT(FLAGS_width, 0);
  CHECK_GE(FLAGS_blur_radius, 0);
  CHECK_GE(FLAGS_threshold, 0);
  CHECK_GE(FLAGS_morph_closing_size, 0);
}

int main(int argc, char* argv[]) {
  system_util::initDep(argc, argv, kUsageMessage);

  verifyInputs();

  const Camera::Rig rig = image_util::filterDestinations(Camera::loadRig(FLAGS_rig), FLAGS_cameras);
  CHECK_GT(rig.size(), 0);

  // Load background colors
  std::vector<cv::Mat_<PixelType>> backgroundColorsFullSize =
      loadImages<PixelType>(FLAGS_background_color, rig, FLAGS_background_frame, FLAGS_threads);

  // Downscale background images
  int outputWidth = std::min(backgroundColorsFullSize[0].cols, FLAGS_width);
  int outputHeight = lrint(
      outputWidth * backgroundColorsFullSize[0].rows / float(backgroundColorsFullSize[0].cols));
  const cv::Size outputSize(outputWidth, outputHeight);
  const std::vector<cv::Mat_<PixelType>> backgroundColors =
      cv_util::resizeImages<PixelType>(backgroundColorsFullSize, outputSize);

  // not needed anymore
  backgroundColorsFullSize.clear();

  verifyImagePaths(FLAGS_color, rig, FLAGS_first, FLAGS_last);
  for (const Camera& cam : rig) {
    filesystem::create_directories(filesystem::path(FLAGS_foreground_masks) / cam.id);
  }

  ThreadPool threadPool(FLAGS_threads);
  const int numFrames = std::stoi(FLAGS_last) - std::stoi(FLAGS_first) + 1;
  for (int iFrame = 0; iFrame < numFrames; ++iFrame) {
    threadPool.spawn([&, iFrame] {
      const std::string frameName =
          image_util::intToStringZeroPad(iFrame + std::stoi(FLAGS_first), 6);
      LOG(INFO) << folly::sformat("Processing frame {}...", frameName);

      const int numThreads = 0; // we're multithreading already
      const std::vector<cv::Mat_<PixelType>> frameColors = loadResizedImages<PixelType>(
          FLAGS_color, rig, frameName, outputSize, cv::INTER_AREA, numThreads);

      // Generate foreground masks
      const std::vector<cv::Mat_<bool>> foregroundMasks =
          background_subtraction::generateForegroundMasks<PixelType, PixelTypeFloat>(
              backgroundColors,
              frameColors,
              outputSize,
              FLAGS_blur_radius,
              FLAGS_threshold,
              FLAGS_morph_closing_size,
              numThreads);

      for (ssize_t iCam = 0; iCam < ssize(rig); ++iCam) {
        const std::string fn =
            folly::sformat("{}/{}/{}.png", FLAGS_foreground_masks, rig[iCam].id, frameName);
        cv_util::imwriteExceptionOnFail(fn, 255.0f * foregroundMasks[iCam]);
      }
    });
  }
  threadPool.join();

  return EXIT_SUCCESS;
}
