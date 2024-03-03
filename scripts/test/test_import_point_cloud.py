#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for ImportPointCloud.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_import_point_cloud.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

from .test_master_class import DepTest, generic_main


class ImportPointCloudTest(DepTest):
    """Unit test class for ImportPointCloud.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        """Run test for ImportPointCloud.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        point_cloud_fn = "point_cloud.xyz"
        projected_disparity_dir = "projected_disparity"

        self.io_args.point_cloud = os.path.join(
            self.io_args.testing_dir, point_cloud_fn
        )
        self.io_args.output = os.path.join(
            self.io_args.testing_dir, projected_disparity_dir
        )

        self.run_app("ImportPointCloud")
        self.check_against_truth(
            truth=os.path.join(self.io_args.truth_dir, projected_disparity_dir),
            output=self.io_args.output,
        )


if __name__ == "__main__":
    generic_main([ImportPointCloudTest])
