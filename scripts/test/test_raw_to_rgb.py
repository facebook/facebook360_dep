#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for RawToRgb.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_raw_to_rgb.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

import test_config as config
from test_master_class import DepTest, generic_main


class RawToRgbTest(DepTest):

    """Unit test class for RawToRgb.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        """Run test for RawToRgb.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        processed_image = "000000.png"
        self.io_args.input_image_path = os.path.join(
            self.io_args.input_root, "000000.raw"
        )
        self.io_args.output_image_path = os.path.join(
            self.io_args.testing_dir, processed_image
        )
        self.io_args.isp_config_path = os.path.join(self.io_args.input_root, "isp.json")

        self.run_app("RawToRgb")
        self.check_against_truth(
            truth=os.path.join(self.io_args.testing_dir, processed_image),
            output=self.io_args.output_image_path,
        )


if __name__ == "__main__":
    generic_main([RawToRgbTest])
