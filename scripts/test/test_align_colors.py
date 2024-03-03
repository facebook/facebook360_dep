#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for AlignColors.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_align_colors.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

from .test_master_class import DepTest, generic_main


class AlignColorsTest(DepTest):
    """Unit test class for AlignColors.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        """Run test for AlignColors.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        aligned_dir = "color_full_alt_aligned"
        self.io_args.output = os.path.join(self.io_args.output_root, aligned_dir)
        self.io_args.color = os.path.join(self.io_args.input_root, "color_full_alt")
        self.io_args.first = self.io_args.last = "000010"
        self.io_args.calibrated_rig = os.path.join(
            self.io_args.input_root, "rig_calibrated_alt.json"
        )

        self.io_args.rig_red = os.path.join(self.io_args.input_root, "red.json")
        self.io_args.rig_green = os.path.join(self.io_args.input_root, "green.json")
        self.io_args.rig_blue = os.path.join(self.io_args.input_root, "blue.json")

        self.run_app("AlignColors")
        self.check_against_truth(
            truth=os.path.join(self.io_args.truth_dir, aligned_dir),
            output=self.io_args.output,
        )


if __name__ == "__main__":
    generic_main([AlignColorsTest])
