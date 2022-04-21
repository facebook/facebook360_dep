#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for RigAnalyzer.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_rig_analyzer.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://facebook360-dep-sample-data/complex-single-frame
"""

import os

from . import test_config as config
from .test_master_class import DepTest, generic_main


class RigAnalyzerTest(DepTest):

    """Unit test class for RigAnalyzer.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        """Run test for RigAnalyzer.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        rig_analysis_dir = "rig_analysis"
        analysis_root = os.path.join(self.io_args.output_root, rig_analysis_dir)
        os.makedirs(analysis_root, exist_ok=True)

        self.io_args.output_obj = os.path.join(analysis_root, "final.obj")
        self.io_args.output_equirect = os.path.join(analysis_root, "equirect.ppm")
        self.io_args.output_camera = os.path.join(analysis_root, "camera.ppm")
        self.io_args.output_camera_id = "0"
        self.io_args.output_cross_section = os.path.join(analysis_root, "cross.ppm")

        self.run_app("RigAnalyzer")
        self.check_against_truth(
            truth=os.path.join(self.io_args.truth_dir, rig_analysis_dir),
            output=analysis_root,
        )


if __name__ == "__main__":
    generic_main([RigAnalyzerTest])
