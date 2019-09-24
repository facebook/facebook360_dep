/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/calibration/CalibrationLib.h"

#include "source/calibration/Calibration.h"

int calibration(
    const std::string& output_rig,
    const std::string& matches,
    const std::string& input_rig,
    const std::string& color,
    const std::string& frame) {
  // set up flags
  FLAGS_rig_out = output_rig;
  FLAGS_matches = matches;
  FLAGS_rig_in = input_rig;
  FLAGS_color = color;
  if (!frame.empty()) {
    FLAGS_frame = frame;
  }

  // run the calibration
  int result = matchCorners();
  if (result != EXIT_SUCCESS) {
    return result;
  }
  return geometricCalibration();
}
