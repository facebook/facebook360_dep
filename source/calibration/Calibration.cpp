/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/calibration/Calibration.h"

DEFINE_string(color, "", "path to input data");
DEFINE_bool(enable_timing, false, "print timing results");
DEFINE_string(frame, "", "frame to process (lexical)");
DEFINE_bool(log_verbose, false, "enable verbose log output from ceres during refine");
DEFINE_double(
    match_score_threshold,
    0.75,
    "minimum zncc score required for a match to be included");
DEFINE_string(matches, "", "path to matches .json file");
DEFINE_string(rig_in, "", "input camera rig .json filename");
DEFINE_string(rig_out, "", "output camera rig .json filename");
DEFINE_int32(threads, -1, "number of threads (-1 = max allowed, 0 = no threading)");
