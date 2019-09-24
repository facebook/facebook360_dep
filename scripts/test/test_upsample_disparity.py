#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for UpsampleDisparity.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_upsample_disparity.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

import test_config as config
from test_master_class import DepTest, generic_main


class UpsampleDisparityTest(DepTest):

    """Unit test class for UpsampleDisparity.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        """Run test for UpsampleDisparity.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        output_level = "level_0"
        input_level = "level_1"
        foreground_masks_in = os.path.join(
            self.io_args.foreground_masks_levels, input_level
        )
        foreground_masks_out = os.path.join(
            self.io_args.foreground_masks_levels, output_level
        )

        test_type_to_masks = {
            "both": {
                "foreground_masks_in": foreground_masks_in,
                "foreground_masks_out": foreground_masks_out,
            },
            "none": {"foreground_masks_in": "", "foreground_masks_out": ""},
            "only_in": {
                "foreground_masks_in": foreground_masks_in,
                "foreground_masks_out": "",
            },
            "only_out": {
                "foreground_masks_in": "",
                "foreground_masks_out": foreground_masks_out,
            },
        }

        disparity_upsample_dir = "disparity_upsample"
        self.io_args.color = os.path.join(self.io_args.color, output_level)
        self.io_args.background_disp = os.path.join(
            self.io_args.background_disp_levels, output_level
        )
        self.io_args.disparity = os.path.join(
            self.io_args.disparity_levels, input_level
        )

        for test_type, mask_paths in test_type_to_masks.items():
            self.io_args.foreground_masks_in = mask_paths["foreground_masks_in"]
            self.io_args.foreground_masks_out = mask_paths["foreground_masks_out"]
            self.io_args.output = os.path.join(
                self.io_args.output_root, disparity_upsample_dir, test_type
            )
            self.run_app("UpsampleDisparity")
            self.check_against_truth(
                truth=os.path.join(
                    self.io_args.truth_dir, disparity_upsample_dir, test_type
                ),
                output=self.io_args.output,
            )


if __name__ == "__main__":
    generic_main([UpsampleDisparityTest])
