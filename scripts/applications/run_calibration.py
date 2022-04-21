#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Python script for running Calibration.

Example:
    $ python run_calibration.py \
        --color=/path/to/colors
        --rig_in=/path/to/input/rig
        --matches=/path/to/output/matches.json
        --rig_out=/path/to/output/rig_calibrated.json
"""

import argparse

from applications_util import AppUtil

parser = argparse.ArgumentParser()
parser.add_argument(
    "--color", help="path to input color images (required)", required=True
)
parser.add_argument(
    "--enable_timing", action="store_true", help="print timing results", default=False
)
parser.add_argument("--frame", help="frame to process (lexical)", default="")
parser.add_argument(
    "--log_verbose", action="store_true", help="log_verbose", default=False
)
parser.add_argument(
    "--matches", help="path to output matches .json file (required)", required=True
)
parser.add_argument(
    "--match_score_threshold",
    help="minimum zncc score required for a match to be included",
    default=0.75,
)
parser.add_argument(
    "--rig_in", help="input camera rig .json file (required)", required=True
)
parser.add_argument(
    "--rig_out",
    help="output calibrated camera rig .json file (required)",
    required=True,
)
parser.add_argument(
    "--threads",
    help="number of threads (-1 = max allowed, 0 = no threading)",
    default=-1,
)
parser.add_argument("--points_file_json", help="path to output calibration points file")

if __name__ == "__main__":
    args = parser.parse_args()
    flags = vars(args)

    app = AppUtil(binary_name="Calibration", flags=flags)
    time_elapsed = app.run_app()
    print(f"Execution time: {time_elapsed}s")
