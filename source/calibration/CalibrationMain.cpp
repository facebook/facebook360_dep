/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/calibration/Calibration.h"
#include "source/util/SystemUtil.h"

const std::string kUsageMessage = R"(
   - Calibrates an uncalibrated rig by feature matching and performing geometric calibration
   on a sample frame.

   - Example:
     ./Calibration \
     --color=/path/to/video/color \
     --matches=/path/to/output/matches.json \
     --rig_in=/path/to/rigs/rig.json \
     --rig_out=/path/to/rigs/rig_calibrated.json
 )";

int main(int argc, char* argv[]) {
  // set up flags
  fb360_dep::system_util::initDep(argc, argv, kUsageMessage);

  // run the calibration
  int result = matchCorners();
  if (result != EXIT_SUCCESS) {
    return result;
  }
  return geometricCalibration();
}
