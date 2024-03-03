#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for ConvertToBinary.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_convert_to_binary.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

from . import test_config as config
from .test_master_class import DepTest, generic_main


class ConvertToBinaryTest(DepTest):
    """Unit test class for ConvertToBinary.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        """Run test for ConvertToBinary.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        fused_dir = "fused"
        binary_dir = os.path.join(self.io_args.output_root, fused_dir)
        os.makedirs(binary_dir, exist_ok=True)

        self.io_args.color = self.io_args.color_full
        self.io_args.disparity = os.path.join(
            self.io_args.disparity_levels, config.TEST_LEVEL
        )
        self.io_args.bin = os.path.join(self.io_args.testing_dir, "bin")
        self.io_args.fused = binary_dir
        self.io_args.cameras = config.TEST_CAM

        self.run_app("ConvertToBinary")
        self.check_against_truth(
            truth=os.path.join(self.io_args.truth_dir, fused_dir), output=binary_dir
        )


if __name__ == "__main__":
    generic_main([ConvertToBinaryTest])
