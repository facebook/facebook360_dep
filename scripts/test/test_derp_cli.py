#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for DerpCLI.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_derp_cli.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os
import sys

sys.path.append(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)
from . import test_config as config
from .test_master_class import DepTest, generic_main


class DerpCLITest(DepTest):
    """Unit test class for DerpCLI.

    Attributes:
        name (str): String representation of the class name.
    """

    def parse_rephoto_errors(self, log_dir):
        """Parses the relevant rephotography errors for red, green, and blue from logs.

        Args:
            log_dir (str): Path to the directory where glog files are saved.

        Returns:
            dict[str, float]: Map from relevant metric to its value. The map contains
                the keys "error_r", "error_g", and "error_b."
        """
        info_path = os.path.join(log_dir, "ComputeRephotographyErrors.INFO")
        with open(info_path, "r") as f:
            lines = f.readlines()

        # rephoto_error_line here refers to line specifically in the format:
        # <timestamp> ComputeRephotographyErrors.cpp:<line_number> TOTAL average MSSIM: R <error_r>%, G <error_g>%, B <error_b>%
        rephoto_error_line = lines[-1]
        parts = rephoto_error_line.split("%")
        parts = [float(parts[i].split(" ")[-1]) for i in range(3)]  # returns R, G, B
        part_labels = ["error_r", "error_g", "error_b"]

        errors = dict(zip(part_labels, parts))
        return errors

    def test_run(self):
        """Run test for DerpCLI.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        derp_command = self.run_app("DerpCLI")

        # Rephotography error is computed on a particular level (e.g. finest level)
        self.io_args.disparity = os.path.join(
            self.io_args.disparity_levels, config.TEST_LEVEL
        )
        self.io_args.color = os.path.join(self.io_args.color, config.TEST_LEVEL)
        rephoto_command = self.run_app("ComputeRephotographyErrors")

        derp_records = self.parse_rephoto_errors(self.io_args.log_dir)
        total_rephoto_error = (
            derp_records["error_r"] + derp_records["error_g"] + derp_records["error_b"]
        ) / 3
        record = {
            "test_name": self.__class__.__name__,
            "total_rephoto_error": total_rephoto_error,
            "r_rephoto_error": derp_records["error_r"],
            "g_rephoto_error": derp_records["error_g"],
            "b_rephoto_error": derp_records["error_b"],
            "derpcli_invocation": derp_command,
            "computerephotography_invocation": rephoto_command,
        }
        self.check_metrics(record)


if __name__ == "__main__":
    generic_main([DerpCLITest])
