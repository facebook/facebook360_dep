#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for SimpleMeshRenderer.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_simple_mesh_renderer.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

import test_config as config
from test_master_class import DepTest, generic_main


class SimpleMeshRendererTest(DepTest):

    """Unit test class for SimpleMeshRenderer.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        """Run test for SimpleMeshRenderer.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        meshes_dir = "meshes"
        formats_str = "cubecolor,cubedisp,eqrcolor,eqrdisp,lr180,snapcolor,snapdisp,tb3dof,tbstereo"
        formats = formats_str.split(",")

        self.io_args.color = os.path.join(self.io_args.color, config.TEST_LEVEL)
        self.io_args.disparity = os.path.join(
            self.io_args.disparity_levels, config.TEST_LEVEL
        )

        for format in formats:
            self.io_args.output = os.path.join(
                self.io_args.output_root, meshes_dir, format
            )
            self.io_args.format = format
            self.run_app("SimpleMeshRenderer")


if __name__ == "__main__":
    generic_main([SimpleMeshRendererTest])
