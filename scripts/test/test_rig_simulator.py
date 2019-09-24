#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for RigSimulator.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_rig_simulator.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

import test_config as config
from test_master_class import DepTest, generic_main


class RigSimulatorTest(DepTest):

    """Unit test class for RigSimulator.

    Attributes:
        name (str): String representation of the class name.
    """

    def test_run(self):
        """Run test for RigSimulator.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        modes_str = (
            "mono_eqr,stereo_eqr,ftheta_ring,dodecahedron,icosahedron,rig_from_json"
        )
        modes = modes_str.split(",")
        rig_simulations_dir = "rig_simulations"

        simulations_root = os.path.join(self.io_args.output_root, rig_simulations_dir)
        self.io_args.rig_in = self.io_args.rig
        self.io_args.skybox_path = os.path.join(self.io_args.testing_dir, "skybox.png")
        self.io_args.dest_mono = os.path.join(simulations_root, "mono.png")
        self.io_args.dest_mono_depth = os.path.join(simulations_root, "mono_depth.png")
        self.io_args.dest_left = os.path.join(simulations_root, "left.png")
        self.io_args.dest_right = os.path.join(simulations_root, "right.png")
        self.io_args.dest_stereo = os.path.join(simulations_root, "stereo.png")

        for mode in modes:
            self.io_args.mode = mode
            self.io_args.rig_out = os.path.join(simulations_root, f"{mode}.json")
            self.io_args.dest_cam_images = os.path.join(simulations_root, mode)
            os.makedirs(self.io_args.dest_cam_images, exist_ok=True)
            self.run_app("RigSimulator")

        self.check_against_truth(
            truth=os.path.join(self.io_args.truth_dir, rig_simulations_dir),
            output=simulations_root,
        )


if __name__ == "__main__":
    generic_main([RigSimulatorTest])
