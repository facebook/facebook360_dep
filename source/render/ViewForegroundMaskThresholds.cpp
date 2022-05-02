/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

const char* kUsage = R"(
   Reads two color images (background and foreground) and a width and displays a trackbar to
   interactively visualize how the flags --blur_radius, --threshold and --morph_closing_size
   affect the areas of the image that will be either ignored (background) or considered (foreground)
   at different stages:

   - blur_radius: Gaussian blur radius, used to reduce noise
   - threshold: Foreground/background RGB L2-norm threshold [0..1]
     foreground mask = ||background - foreground||^2 > threshold
   - morph_closing_size: Morphological closing size, used to fill holes on the final mask

 )";

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/render/BackgroundSubtractionUtil.h"
#include "source/util/CvUtil.h"
#include "source/util/SystemUtil.h"

using PixelType = cv::Vec3w;
using PixelTypeFloat = cv::Vec3f;

using namespace fb360_dep;
using namespace fb360_dep::cv_util;

DEFINE_int32(blur_radius_max, 20, "max Gaussian blur radius allowed");
DEFINE_string(fullsize_bg_image, "", "path to full-size RGB background image (required)");
DEFINE_string(fullsize_fg_image, "", "path to full-size RGB foreground image (required)");
DEFINE_int32(morph_closing_size_max, 20, "max morphological closing size allowed");
DEFINE_int32(width, 2048, "loaded image width (0 = original size)");

const std::string blurFlag = "--blur_radius";
const std::string threshFlag = "--threshold";
const std::string closingFlag = "--morph_closing_size";

class TrackVar {
  const std::string winName = "Foreground mask thresholds";
  const PixelType green = cv_util::createBGR<PixelType>(0, 1, 0);

  cv::Mat_<PixelType> imageBg;
  cv::Mat_<PixelType> imageFg;

  int sliderThreshVal;
  float threshMax;

  int sliderBlurMaxCount;
  int sliderThreshMaxCount;
  int sliderClosingMaxCount;

  int blur;
  float threshold;
  int closing;

  static void onChange(int, void* object) {
    reinterpret_cast<TrackVar*>(object)->update();
  }

 private:
  void update() {
    threshold = threshMax * sliderThreshVal / sliderThreshMaxCount;
    const cv::Mat_<bool> mask =
        background_subtraction::generateForegroundMask<PixelType, PixelTypeFloat>(
            imageBg, imageFg, blur, threshold, closing);
    cv::Mat_<PixelType> maskPt = cv_util::convertImage<PixelType>(255.0f * mask);
    maskPt.setTo(green, mask);
    cv::Mat_<PixelType> overlay;
    cv::addWeighted(imageFg, 1.0, maskPt, 0.5, 0, overlay);
    imshow(winName, overlay);
  }

 public:
  TrackVar(
      const std::string& imageBgPath,
      const std::string& imageFgPath,
      const int width,
      const int blurMaxIn,
      const float threshMaxIn,
      const int closingMaxIn)
      : threshMax(threshMaxIn), blur(1), threshold(0.04f), closing(4) {
    // Load (and scale) images
    imageBg = loadImage<PixelType>(imageBgPath);
    imageFg = loadImage<PixelType>(imageFgPath);
    CHECK_EQ(imageBg.size(), imageFg.size());
    const double scale = width > 0 ? double(width) / imageBg.cols : 1.0;
    if (width > 0) {
      imageBg = scaleImage(imageBg, scale);
      imageFg = scaleImage(imageFg, scale);
    }

    // Initialize values
    sliderBlurMaxCount = blurMaxIn;
    sliderThreshMaxCount = 100;
    sliderClosingMaxCount = closingMaxIn;
    sliderThreshVal = threshold / threshMax * sliderThreshMaxCount;

    // Create trackbars
    cv::namedWindow(winName, 1);
    cv::createTrackbar(blurFlag, winName, &blur, sliderBlurMaxCount, onChange, this);
    cv::createTrackbar(threshFlag, winName, &sliderThreshVal, sliderThreshMaxCount, onChange, this);
    cv::createTrackbar(closingFlag, winName, &closing, sliderClosingMaxCount, onChange, this);
    update();
  }

  int getBlur() {
    return blur;
  }

  float getThreshold() {
    return threshold;
  }

  int getClosing() {
    return closing;
  }
};

int main(int argc, char* argv[]) {
  gflags::SetUsageMessage(kUsage);
  system_util::initDep(argc, argv);

  CHECK_NE(FLAGS_fullsize_bg_image, "");
  CHECK_NE(FLAGS_fullsize_fg_image, "");
  CHECK_GT(FLAGS_blur_radius_max, 0);
  CHECK_GT(FLAGS_morph_closing_size_max, 0);
  CHECK_GE(FLAGS_width, 0);

  const float threshMax = 1.0f;
  TrackVar trackVar(
      FLAGS_fullsize_bg_image,
      FLAGS_fullsize_fg_image,
      FLAGS_width,
      FLAGS_blur_radius_max,
      threshMax,
      FLAGS_morph_closing_size_max);

  LOG(INFO) << "Press any key to exit.";
  cv::waitKey(0);

  LOG(INFO) << folly::sformat("{}={}", blurFlag, trackVar.getBlur());
  LOG(INFO) << folly::sformat("{}={:.3e}", threshFlag, trackVar.getThreshold());
  LOG(INFO) << folly::sformat("{}={}", closingFlag, trackVar.getClosing());

  return EXIT_SUCCESS;
}
