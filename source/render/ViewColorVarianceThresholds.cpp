/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

const char* kUsage = R"(
   Reads a color image and a width and displays a trackbar to interactively visualize how the
   flags --var_noise_floor and --var_high_thresh affect the areas of the image that will be
   either ignored or considered at different stages:

   - var_noise_floor: noise variance floor on original, full-size images. Variance noise is
     multiplied by the square of the scale at a given level, whose width is given by --width
     Random proposals and disparity mismatches are ignored if their variance is lower than this
     threshold.
   - var_high_thresh: ignore variances higher than this threshold
     Random proposals and disparity mismatches are accepted if their variance is higher than this
     threshold.

 )";

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/depth_estimation/DerpUtil.h"
#include "source/util/CvUtil.h"
#include "source/util/SystemUtil.h"

using PixelType = cv::Vec3w;

using namespace fb360_dep;
using namespace fb360_dep::cv_util;

DEFINE_string(fullsize_image, "", "path to full-size RGB image (required)");
DEFINE_double(var_high_max, 5e-2, "max high variance allowed");
DEFINE_double(var_low_max, 4e-3, "max low variance allowed");
DEFINE_int32(width, 2048, "loaded image width (0 = original size)");

const std::string varLowFlag = "--var_noise_floor";
const std::string varHighFlag = "--var_high_thresh";

class TrackVar {
  const std::string winName = "Color thresholds";
  const int sliderMaxCount = 100;

  cv::Mat_<PixelType> image;
  cv::Mat_<float> var;

  int sliderLowVal;
  int sliderHighVal;
  float varLowMax;
  float varHighMax;

  float varLowShow;
  float varHighShow;
  float varNoiseFloor;
  float varHighThresh;

  float scaleVar;

  static void onChange(int, void* object) {
    reinterpret_cast<TrackVar*>(object)->update();
  }

 private:
  void update() {
    varNoiseFloor = varLowMax * sliderLowVal / sliderMaxCount;
    varHighThresh = varHighMax * sliderHighVal / sliderMaxCount;
    varLowShow = std::max(varNoiseFloor * scaleVar, depth_estimation::kMinVar);
    varHighShow = std::max(varHighThresh, varLowShow);
    cv::Mat_<PixelType> srcMarked = image.clone();

    const PixelType kBlue = cv_util::createBGR<PixelType>(1, 0, 0);
    const PixelType kPurple = cv_util::createBGR<PixelType>(1, 0, 1);
    srcMarked.setTo(kBlue, var < varLowShow);
    srcMarked.setTo(kPurple, var > varHighShow);
    imshow(winName, srcMarked);
  }

 public:
  TrackVar(
      const std::string& imagePath,
      const int width,
      const float varLowMaxIn,
      const float varHighMaxIn)
      : varLowMax(varLowMaxIn), varHighMax(varHighMaxIn), varNoiseFloor(1e-4), varHighThresh(1e-3) {
    // Load image
    image = loadImage<PixelType>(imagePath);
    const double scale = width > 0 ? double(width) / image.cols : 1.0;
    if (width > 0) {
      image = scaleImage(image, scale);
    }
    var = depth_estimation::computeImageVariance(image);
    scaleVar = math_util::square(scale);

    // Initialize values
    sliderLowVal = varNoiseFloor / varLowMax * sliderMaxCount;
    sliderHighVal = varHighThresh / varHighMax * sliderMaxCount;

    // Create trackbars
    cv::namedWindow(winName, 1);
    cv::createTrackbar(varLowFlag, winName, &sliderLowVal, sliderMaxCount, onChange, this);
    cv::createTrackbar(varHighFlag, winName, &sliderHighVal, sliderMaxCount, onChange, this);
    update();
  }

  float getVarNoiseFloor() {
    return varNoiseFloor;
  }

  float getVarHighThresh() {
    return varHighThresh;
  }
};

int main(int argc, char* argv[]) {
  gflags::SetUsageMessage(kUsage);
  system_util::initDep(argc, argv);

  CHECK_NE(FLAGS_fullsize_image, "");
  CHECK_GT(FLAGS_var_low_max, 0);
  CHECK_GT(FLAGS_var_high_max, 0);
  CHECK_GE(FLAGS_width, 0);

  TrackVar trackVar(FLAGS_fullsize_image, FLAGS_width, FLAGS_var_low_max, FLAGS_var_high_max);

  LOG(INFO) << "Press any key to exit.";
  cv::waitKey(0);

  LOG(INFO) << folly::sformat("{}={:.3e}", varLowFlag, trackVar.getVarNoiseFloor());
  LOG(INFO) << folly::sformat("{}={:.3e}", varHighFlag, trackVar.getVarHighThresh());

  return EXIT_SUCCESS;
}
