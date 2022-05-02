/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gflags/gflags.h>

int matchCorners();
double geometricCalibration();

DECLARE_string(rig_in);
DECLARE_string(matches);
DECLARE_string(rig_out);

DECLARE_string(color);
DECLARE_string(frame);

DECLARE_double(match_score_threshold);

DECLARE_int32(threads);
DECLARE_bool(enable_timing);
DECLARE_bool(log_verbose);
