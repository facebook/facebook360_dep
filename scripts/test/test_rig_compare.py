#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for RigCompare.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_rig_compare.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

import test_config as config
from test_master_class import DepTest, generic_main


class RigCompareTest(DepTest):

    """Unit test class for RigCompare.

    Attributes:
        name (str): String representation of the class name.
    """

    def parse_diffs(self, log_dir):
        """Parses the log file produced by RigCompare and returns relevant metrics in a dict.

        Args:
            log_dir (str): Path to the directory where glog files are saved.

        Returns:
            dict[str, float]: Map of relevant metric names to their values.
        """
        info_path = os.path.join(log_dir, "RigCompare.INFO")
        with open(info_path, "r") as f:
            lines = f.readlines()
        avg_results = lines[-5:]

        record = {}
        for line in avg_results:
            metric_value = line.split("-")[-1].strip()
            metric, value = metric_value.split(": ")
            record[metric.strip()] = float(value)
        return record

    def test_run(self):
        """Run test for RigCompare.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        self.io_args.reference = os.path.join(
            self.io_args.input_root, "rig_calibrated.json"
        )

        self.run_app("RigCompare")
        record = self.parse_diffs(self.io_args.log_dir)
        self.check_metrics(record)


if __name__ == "__main__":
    generic_main([RigCompareTest])
