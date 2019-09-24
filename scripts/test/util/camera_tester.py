#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

""" Tester functions for the Camera class

Performs tests on the functions defined in camera.py and rig.py
Tester functions are used in the unit test script (test_camera.py)

"""

import copy
import os
import sys

dir_scripts = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
)
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "util"))

import numpy as np

from matrix_operations import is_approx, normalize_vector

from logger import Logger
from rig import Rig

log = Logger(__name__)
log.logger.propagate = False


def check_failed_attributes(expected, actual, tol=1e-10):
    failed_attributes = []
    for att in expected.keys():
        if type(expected[att]) is np.ndarray:
            eq = is_approx(expected[att], actual[att], tol)
        else:
            eq = expected[att] == actual[att]
        if not eq:
            failed_attributes.append(
                "{}: expected = {}, actual = {}".format(att, expected[att], actual[att])
            )
    if not failed_attributes:
        return TestOutput(True, "no failed attributes")
    else:
        return TestOutput(False, "failed attributes: {}".format(failed_attributes))


class CameraTester:
    def __init__(self, camera):
        self.camera = camera

    def test_projection(self, vector, depth=1):
        log.check(vector.shape == (3,), "not a valid point in 3d")
        camera = self.camera
        expected_point = camera.position + depth * normalize_vector(vector)
        if not camera.sees(expected_point)[0]:
            # ignore cases where camera cannot see the point
            return TestOutput(True, "point is outside the camera's view")
        pixel = camera.world_to_pixel(expected_point)
        actual_point = camera.pixel_to_world(pixel).point_at(depth)
        expected = {"projected point": expected_point}
        actual = {"projected point": actual_point}
        return check_failed_attributes(expected, actual, 1e-3)

    def test_fov_with_point(self, point):
        log.check(point.shape == (3,), "not a valid point in 3d")
        camera = self.camera
        if camera.sees(point)[0]:
            return TestOutput(True, "sees")
        else:
            return TestOutput(False, "does not see")

    def test_fov_with_pixel(self, pixel):
        log.check(pixel.shape == (2,), "not a valid 2d pixel")
        camera = self.camera
        if camera.is_outside_sensor(pixel) and camera.is_outside_image_circle(pixel):
            return TestOutput(False, "outside sensor and image circle")
        elif camera.is_outside_sensor(pixel):
            return TestOutput(False, "outside sensor")
        elif camera.is_outside_image_circle(pixel):
            return TestOutput(False, "outside image circle")
        else:
            return TestOutput(True, "inside sensor and image circle")

    def test_rotation(self, rotation_matrix):
        camera = self.camera
        expected = copy.deepcopy(camera.__dict__)
        expected["rotation"] = rotation_matrix
        camera.set_rotation(-rotation_matrix[2], rotation_matrix[1], rotation_matrix[0])
        actual = camera.__dict__
        return check_failed_attributes(expected, actual)

    def test_distort_undistort(self, distortion, r):
        camera = self.camera
        camera.set_distortion(distortion)
        # convert to ndarray for approx check
        expected = {"radial position": np.array([r])}
        actual = {"radial position": np.array([camera.undistort(camera.distort(r))])}
        return check_failed_attributes(expected, actual, 1e-4)

    def test_distort_monotonic(self, distortion):
        camera = self.camera
        camera.set_distortion(distortion)

        prev = 0
        for y in np.linspace(0, 3, 30, endpoint=False):
            x = camera.undistort(y)
            if prev > x + 1 / 1e4:
                return TestOutput(False, "failed at {}".format(y))
            prev = x
        return TestOutput(True, "success")

    def test_rescale(self, scale_factor):
        camera = self.camera
        distorted = copy.deepcopy(camera)
        distorted.resolution = distorted.resolution * scale_factor
        distorted.focal = distorted.focal * scale_factor
        distorted.principal = distorted.principal * scale_factor
        distorted.rescale(camera.resolution)
        expected = camera.__dict__
        actual = distorted.__dict__
        return check_failed_attributes(expected, actual)

    def test_normalize(self):
        camera = self.camera
        normalized = copy.deepcopy(camera)
        normalized.resolution = np.ones(2)
        normalized.focal = camera.focal / camera.resolution
        normalized.principal = camera.principal / camera.resolution
        camera.normalize()
        expected = normalized.__dict__
        actual = camera.__dict__
        return check_failed_attributes(expected, actual)


class RigTester:
    def __init__(self, rig):
        self.rig = rig

    def test_normalize(self):
        rig = self.rig
        rig.normalize()
        for cam in rig.cameras:
            if not cam.is_normalized():
                return TestOutput(False, "failed at {}".format(cam.id))
        return TestOutput(True, "success")

    def test_save_to_file(self, filename):
        rig = self.rig
        rig.save_to_file(filename)
        try:
            f = open(filename, "r")
            f.close()
        except IOError:
            return TestOutput(False, "failed")

        saved_rig = Rig(filename)
        os.remove(filename)
        for cam, saved_cam in zip(rig.cameras, saved_rig.cameras):
            expected = cam.__dict__
            actual = saved_cam.__dict__
        return check_failed_attributes(expected, actual)


class TestOutput:

    __slots__ = ["result", "message"]

    def __init__(self, result, message):
        self.result = result
        self.message = message
