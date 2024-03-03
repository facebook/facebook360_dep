#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for RigAligner.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_rig_aligner.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

from . import test_config as config
from .test_master_class import DepTest, generic_main


class RigAlignerTest(DepTest):
    """Unit test class for RigAligner.

    Attributes:
        name (str): String representation of the class name.
    """

    def parse_align_results(self, log_dir):
        """Parses the log file produced by RigAligner and returns relevant metrics in a dict.

        Args:
            log_dir (str): Path to the directory where glog files are saved.

        Returns:
            dict[str, float]: Map of relevant metric names to their values.
        """
        info_path = os.path.join(log_dir, "RigAligner.INFO")
        with open(info_path, "r") as f:
            lines = f.readlines()

        # Relevant_results here refers to lines specifically in the format:
        # <timestamp> RigAligner.h:<line_number> Ceres Solver Report: Iterations: <iters>, Initial cost: <initial_cost>, Final cost: <final_cost>, Termination: CONVERGENCE
        record = {}
        metric_values = lines[-4].split("Final cost: ")[-1].strip()
        metric, _ = metric_values.split(", ", 1)
        record["final cost"] = float(metric)
        return record

    def test_run(self):
        """Run test for RigAligner.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        aligned_rig_fn = "rig_aligned.json"
        self.io_args.rig_reference = os.path.join(
            self.io_args.input_root, "rig_calibrated.json"
        )
        self.io_args.rig_out = os.path.join(self.io_args.output_root, aligned_rig_fn)

        self.run_app("RigAligner")
        record = self.parse_align_results(self.io_args.log_dir)
        self.check_metrics(record)


if __name__ == "__main__":
    generic_main([RigAlignerTest])
