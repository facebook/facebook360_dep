#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for ExportPointCloud.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_export_point_cloud.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

from . import test_config as config
from .test_master_class import DepTest, generic_main


class ExportPointCloudTest(DepTest):
    """Unit test class for ExportPointCloud.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        point_cloud_fn = "point_cloud.xyz"
        self.io_args.color = os.path.join(self.io_args.color, config.TEST_LEVEL)
        self.io_args.disparity = os.path.join(
            self.io_args.disparity_levels, config.TEST_LEVEL
        )
        self.io_args.output = os.path.join(self.io_args.output_root, point_cloud_fn)

        self.run_app("ExportPointCloud")
        self.check_against_truth(
            truth=os.path.join(self.io_args.truth_dir, point_cloud_fn),
            output=self.io_args.output,
        )


if __name__ == "__main__":
    generic_main([ExportPointCloudTest])
