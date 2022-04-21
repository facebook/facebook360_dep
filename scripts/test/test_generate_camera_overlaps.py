#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for GenerateCameraOverlaps.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_generate_camera_overlaps.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

from . import test_config as config
from .test_master_class import DepTest, generic_main


class GenerateCameraOverlapsTest(DepTest):

    """Unit test class for GenerateCameraOverlaps.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        """Run test for GenerateCameraOverlaps.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        overlap_dir = "overlaps"
        self.io_args.color = os.path.join(self.io_args.color, config.TEST_LEVEL)
        self.io_args.output = overlap_dir

        self.run_app("GenerateCameraOverlaps")
        self.check_against_truth(
            truth=os.path.join(self.io_args.truth_dir, overlap_dir),
            output=self.io_args.output,
        )


if __name__ == "__main__":
    generic_main([GenerateCameraOverlapsTest])
