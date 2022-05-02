/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <iostream>

#include <glog/logging.h>

#include "source/calibration/CalibrationLib.h"

const std::string kUsageMessage = R"(
  - Calibrates an uncalibrated rig by feature matching and performing geometric calibration
  on a sample frame. Unlike Calibration, this app takes fixed command line arguments.

  - Example:
    ./CalibrationLibMain \
      /path/to/rigs/rig_calibrated.json \
      /path/to/output/matches.json \
      /path/to/rigs/rig.json \
      /path/to/video/color
  )";

int main(int argc, char* argv[]) {
  if (argc != 5) {
    std::cerr << "Error: expected 4 arguments" << std::endl;
    std::cerr
        << "Usage: calibrationlib <output_rig_filename> <matches_filename> <input_rig_filename> "
        << "<color_directory>" << std::endl;
    std::cerr << kUsageMessage << std::endl;
    return EXIT_FAILURE;
  }

  FLAGS_logtostderr = true;
  google::InitGoogleLogging(argv[0]);

  calibration(argv[1], argv[2], argv[3], argv[4]);

  return EXIT_SUCCESS;
}
