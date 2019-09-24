#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for GenerateForegroundMasks.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_generate_foreground_masks.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

import test_config as config
from test_master_class import DepTest, generic_main


class GenerateForegroundMasksTest(DepTest):

    """Unit test class for GenerateForegroundMasks.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        """Run test for GenerateForegroundMasks.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        self.io_args.color = os.path.join(self.io_args.color, config.TEST_LEVEL)
        self.io_args.background_color = os.path.join(
            self.io_args.background_color, config.TEST_LEVEL
        )
        self.io_args.foreground_masks = os.path.join(
            self.io_args.foreground_masks, config.TEST_LEVEL
        )

        self.run_app("GenerateForegroundMasks")
        self.check_against_truth(
            truth=os.path.join(
                self.io_args.truth_dir, "foreground_masks_levels", config.TEST_LEVEL
            ),
            output=self.io_args.foreground_masks,
        )


if __name__ == "__main__":
    generic_main([GenerateForegroundMasksTest])
