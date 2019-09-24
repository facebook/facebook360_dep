#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for ProjectEquirectsToCameras.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_project_equirects_to_cameras.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

import test_config as config
from test_master_class import DepTest, generic_main
from test_util import min_max_frame_from_data_dir


class ProjectEquirectsToCamerasTest(DepTest):

    """Unit test class for ProjectEquirectsToCameras.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        """Run test for ProjectEquirectsToCameras.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        projected_dir = "equirect_projections"
        eqr_masks = "equirect_foreground_masks"

        self.io_args.eqr_masks = os.path.join(self.io_args.testing_dir, eqr_masks)
        self.io_args.first, self.io_args.last = min_max_frame_from_data_dir(
            self.io_args.eqr_masks
        )
        self.io_args.output = os.path.join(self.io_args.output_root, projected_dir)

        self.run_app("ProjectEquirectsToCameras")
        self.check_against_truth(
            truth=os.path.join(self.io_args.truth_dir, projected_dir),
            output=self.io_args.output,
        )


if __name__ == "__main__":
    generic_main([ProjectEquirectsToCamerasTest])
