#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.


""" Camera unit tests

Runs unit tests on the Camera class using the test functions from camera_tester

Example:
        $ python test_camera.py
"""

import os
import sys
import unittest

import numpy as np

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
dir_testutil = os.path.join(dir_scripts, "test/util")

sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "util"))
sys.path.append(dir_testutil)

from camera import Camera
from camera_tester import CameraTester, RigTester
from rig import Rig


class cameraTest(unittest.TestCase):
    def setUp(self):
        rig_file = dir_root + "/res/test/rigs/rig.json"
        ftheta_file = dir_root + "/res/test/cameras/ftheta.json"
        rect_file = dir_root + "/res/test/cameras/rectilinear.json"
        orth_file = dir_root + "/res/test/cameras/orthographic.json"

        self.rig = Rig(rig_file)
        self.cameras = self.rig.cameras
        self.cameras.append(Camera(ftheta_file))
        self.cameras.append(Camera(rect_file))
        self.cameras.append(Camera(orth_file))

    def test_projection_in_fov(self):
        # Case 1: test using a direction inside the fov of each camera
        for camera in self.cameras:
            ct = CameraTester(camera)
            direction = camera.forward()
            depth = 1.23
            test_output = ct.test_projection(direction, depth)
            self.assertTrue(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_projection_out_fov(self):
        # Case 2: test using a direction outside the fov of each camera
        for camera in self.cameras:
            ct = CameraTester(camera)
            direction = camera.backward()
            depth = 4.2
            test_output = ct.test_projection(direction, depth)
            self.assertTrue(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_projection_fixed(self):
        # Case 3: test using a fixed direction for all cameras in rig
        for camera in self.cameras:
            ct = CameraTester(camera)
            direction = np.array([-2, 3, -1])
            depth = 3.1
            test_output = ct.test_projection(direction, depth)
            self.assertTrue(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_sees_in_fov(self):
        # Case 1: test using point inside the fov and sensor
        for camera in self.cameras:
            ct = CameraTester(camera)
            pixel = camera.principal
            point = camera.point_near_infinity(pixel)
            test_output = ct.test_fov_with_point(point)
            self.assertTrue(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_sees_out_fov(self):
        # Case 2: test using point outside the fov
        for camera in self.cameras:
            ct = CameraTester(camera)
            point = camera.backward()
            test_output = ct.test_fov_with_point(point)
            self.assertFalse(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_sees_out_sensor(self):
        # Case 3: test using point outside the sensor
        for camera in self.cameras:
            ct = CameraTester(camera)
            pixel = np.array([-1, -1])
            test_output = ct.test_fov_with_pixel(pixel)
            self.assertFalse(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_rotation(self):
        for camera in self.cameras:
            ct = CameraTester(camera)
            rotation_matrix = np.array([[1, 0, 0], [0, 1, 0], [0, 0, 1]])
            test_output = ct.test_rotation(rotation_matrix)
            self.assertTrue(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_distort_positive_real(self):
        # Case 1: positive real roots
        for camera in self.cameras:
            ct = CameraTester(camera)
            distortion = np.array([0.2, 0.02, 0])
            radial_position = 2
            test_output = ct.test_distort_undistort(distortion, radial_position)
            self.assertTrue(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_distort_negative_real(self):
        # Case 2: negative real roots
        for camera in self.cameras:
            ct = CameraTester(camera)
            distortion = np.array([2 / 3, 1 / 5, 0])
            radial_position = 2
            test_output = ct.test_distort_undistort(distortion, radial_position)
            self.assertFalse(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_distort_imaginary(self):
        # Case 3: imaginary roots
        for camera in self.cameras:
            ct = CameraTester(camera)
            distortion = np.array([1, 1, 0])
            radial_position = 2
            test_output = ct.test_distort_undistort(distortion, radial_position)
            self.assertFalse(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_distort_no_op(self):
        # Case 4: no-op (default distortion)
        for camera in self.cameras:
            ct = CameraTester(camera)
            distortion = np.array([0, 0, 0])
            radial_position = 3
            test_output = ct.test_distort_undistort(distortion, radial_position)
            self.assertTrue(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_distort_monotonic(self):
        # Case 5: monotonic (check that distortion value is non-decreasing)
        for camera in self.cameras:
            ct = CameraTester(camera)
            distortion = np.array([-0.03658484692522479, -0.004515457470690702, 0])
            test_output = ct.test_distort_monotonic(distortion)
            self.assertTrue(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_rescale_small(self):
        # Case 1: snall scale factor
        for camera in self.cameras:
            ct = CameraTester(camera)
            test_output = ct.test_rescale(0.0012)
            self.assertTrue(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_rescale_large(self):
        # Case 2: large scale factor
        for camera in self.cameras:
            ct = CameraTester(camera)
            test_output = ct.test_rescale(99999.9)
            self.assertTrue(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_normalize_camera(self):
        # Case 1: normalize each individual camera
        for camera in self.cameras:
            ct = CameraTester(camera)
            test_output = ct.test_normalize()
            self.assertTrue(
                test_output.result, "{}: {}".format(camera.id, test_output.message)
            )

    def test_normalize_rig(self):
        # Case 2: normalize entire rig
        rt = RigTester(self.rig)
        test_output = rt.test_normalize()
        self.assertTrue(test_output.result, test_output.message)

    def test_save_rig_to_file(self):
        rt = RigTester(self.rig)
        test_output = rt.test_save_to_file("test_rig_file.json")
        self.assertTrue(test_output.result, test_output.message)


if __name__ == "__main__":
    unittest.main()
