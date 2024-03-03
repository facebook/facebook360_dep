#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for LayerDisparities.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_layer_disparities.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

from . import test_config as config
from .test_master_class import DepTest, generic_main
from .test_util import min_max_frame_from_data_dir


class LayerDisparitiesTest(DepTest):
    """Unit test class for LayerDisparities.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        """Run test for LayerDisparities.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        merged_disparities = "merged_disparities"
        self.io_args.background_disp = os.path.join(
            self.io_args.background_disp, config.TEST_LEVEL
        )
        self.io_args.foreground_disp = os.path.join(
            self.io_args.disparity_levels, config.TEST_LEVEL
        )
        self.io_args.output = os.path.join(self.io_args.output_root, merged_disparities)
        self.io_args.first, self.io_args.last = min_max_frame_from_data_dir(
            self.io_args.foreground_disp
        )

        self.run_app("LayerDisparities")
        self.check_against_truth(
            truth=os.path.join(self.io_args.truth_dir, merged_disparities),
            output=self.io_args.output,
        )


if __name__ == "__main__":
    generic_main([LayerDisparitiesTest])
