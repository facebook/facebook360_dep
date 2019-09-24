#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for CalibrationLibMain.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_calibration_lib_main.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os
import shutil

import test_config as config
from test_calibration import CalibrationTest, parse_calibration_results
from test_master_class import DepTest, generic_main


class CalibrationLibMainTest(DepTest):

    """Unit test class for CalibrationLibMain.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        """Run test for CalibrationLibMain.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        CalibrationTest.setup_flags(self)
        log_file = os.path.join(self.io_args.log_dir, "CalibrationLibMain.INFO")

        # CalibrationLibMain assumes the operating frame to be 000000
        lib_main_input = self.io_args.color_full + "_000000"
        if not os.path.exists(lib_main_input):
            shutil.copytree(self.io_args.color_full, lib_main_input)
            for cam in os.listdir(lib_main_input):
                cur_img = os.path.join(lib_main_input, cam, f"{self.io_args.first}.png")
                new_img = os.path.join(lib_main_input, cam, "000000.png")
                os.rename(cur_img, new_img)

        self.run_app(
            "CalibrationLibMain",
            args=f"{self.io_args.rig_out} {self.io_args.matches} {self.io_args.rig_in} {lib_main_input}",
            log_file=log_file,
        )
        record = parse_calibration_results(
            self.io_args.log_dir, bin_name="CalibrationLibMain"
        )
        self.check_metrics(record)


if __name__ == "__main__":
    generic_main([CalibrationLibMainTest])
