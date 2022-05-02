/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/util/CvUtil.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;
using namespace fb360_dep::cv_util;
using namespace fb360_dep::system_util;

const std::string kUsageMessage = R"(
 - Converts a PNG single-channel disparity image to a PFM.

 - Example:
   ./PngToPfm \
   --png=/path/to/video/000000.png \
   --pfm=/path/to/video/000000.pfm
 )";

DEFINE_string(pfm, "", "path to output disparity pfm (required)");
DEFINE_string(png, "", "path to input disparity png (required)");

int main(int argc, char** argv) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_png, "");
  CHECK_NE(FLAGS_pfm, "");

  const cv::Mat_<float> disparity = cv_util::loadImage<float>(FLAGS_png);
  cv_util::writeCvMat32FC1ToPFM(FLAGS_pfm, disparity);
  return EXIT_SUCCESS;
}
