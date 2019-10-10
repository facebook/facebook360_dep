/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/calibration/Calibration.h"
#include "source/util/SystemUtil.h"

#ifdef WIN32
#include "source/calibration/Calibration.cpp"
#endif

const std::string kUsageMessage = R"(
   - Performs feature matching on a sample frame.

   - Example:
     ./MatchCorners \
     --color=/path/to/video/color \
     --matches=/path/to/output/matches.json \
     --rig_in=/path/to/rigs/rig.json
 )";

int main(int argc, char* argv[]) {
  fb360_dep::system_util::initDep(argc, argv, kUsageMessage);

  return matchCorners();
}
