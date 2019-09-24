/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/calibration/Calibration.h"
#include "source/util/SystemUtil.h"

const std::string kUsageMessage = R"(
   - Performs geometric calibration on a sample frame. The results of the feature matcher should
   be available before execution.

   - Example:
     ./GeometricCalibration \
     --matches=/path/to/output/matches.json \
     --rig_in=/path/to/rigs/rig.json \
     --rig_out=/path/to/rigs/rig_calibrated.json
 )";

int main(int argc, char* argv[]) {
  fb360_dep::system_util::initDep(argc, argv, kUsageMessage);
  geometricCalibration();
  return 0;
}
