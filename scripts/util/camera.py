#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

import json
import math

import numpy as np
import numpy.polynomial.polynomial as poly
from logger import Logger
from matrix_operations import is_unitary, normalize_vector
from ray import Ray
from scipy import linalg
from scipy.spatial.transform import Rotation as R

K_NEAR_INFINITY = 1e4
INFINITY = 1e50

log = Logger(__name__)
log.logger.propagate = False


class Camera:

    """The Camera class contains all attributes of a camera such as:
        type of lens, rotations, resolution, principal, focal, etc.

    Attributes:
        id (string) : name of camera
        type (string) : type of camera lens i.e. ftheta, rectilinear, orthographic, equiosolid
        group (string) : name of group to which the camera belongs

        position (numpy 1x3 array) : position of the camera relative to center of the rig (meters)
        rotation (numpy 3x3 "matrix"/2D array) : matrix that represents the rotation of the camera
            right
            up
            backward

        resolution (numpy 1x2 array) : resolution of camera in pixels
        focal (numpy 1x2 array) : number of pixels per radian at principal point
        principal (numpy 1x2 dim array) : pixel coordinate of the optical axis

        distortion (numpy 1x3 array) : array of coefficients for the distortion polynomial
        cos_fov (float) : cosine value of the fov in radians
    """

    def __init__(self, json_file=None, json_string=None):
        if json_file:
            self.load_from_json_file(json_file)
        elif json_string:
            self.load_from_json_string(json_string)

    def load_from_json_file(self, json_file):
        with open(json_file) as f:
            json_string = f.read()
        self.load_from_json_string(json_string)

    def load_from_json_string(self, json_string):
        json_obj = json.loads(json_string)
        log.check_ge(float(json_obj["version"]), 1.0)
        self.version = float(json_obj["version"])
        self.id = str(json_obj["id"])
        self.type = str(json_obj["type"])
        if "group" in json_obj:
            self.group = str(json_obj["group"])

        self.position = deserialize_list(json_obj["origin"], 3)

        self.set_rotation(json_obj["forward"], json_obj["up"], json_obj["right"])

        self.resolution = deserialize_list(json_obj["resolution"], 2)

        self.focal = deserialize_list(json_obj["focal"], 2)

        if "principal" in json_obj:
            self.principal = deserialize_list(json_obj["principal"], 2)
        else:
            self.principal = self.resolution / 2

        if "distortion" in json_obj:
            self.set_distortion(deserialize_list(json_obj["distortion"]))
        else:
            self.set_default_distortion()

        if "fov" in json_obj:
            self.set_fov(float(json_obj["fov"]))
        else:
            self.set_default_fov()

    def serialize(self):
        json_obj = {
            "version": self.version,
            "id": self.id,
            "type": self.type,
            "origin": self.position.tolist(),
            "forward": self.forward().tolist(),
            "up": self.up().tolist(),
            "right": self.right().tolist(),
            "resolution": self.resolution.tolist(),
            "focal": self.focal.tolist(),
        }

        if hasattr(self, "group"):
            json_obj["group"] = self.group
        if not np.array_equal(self.principal, self.resolution / 2):
            json_obj["principal"] = self.principal.tolist()
        if np.any(self.__distortion):
            json_obj["distortion"] = self.__distortion.tolist()
        if not self.is_default_fov():
            json_obj["fov"] = self.get_fov()

        return json_obj

    """ Orientation """

    def forward(self):
        return np.negative(self.rotation[2])

    def backward(self):
        return self.rotation[2]

    def up(self):
        return self.rotation[1]

    def right(self):
        return self.rotation[0]

    def get_rotation(self):
        return self.rotation

    def set_rotation(self, forward, up, right):
        # TODO: might have to convert to angle axis and back to rotation matrix if there is
        #      a variance from floating point error
        log.check_lt(
            np.dot(np.cross(right, up), forward), 0, "rotation must be right-handed"
        )
        self.rotation = np.array([right, up, np.negative(forward)])

        tol = 0.001
        log.check(is_unitary(self.rotation, tol))

    def get_rotation_vector(self):
        r = R.from_dcm(self.get_rotation())
        return r.as_rotvec()

    def set_rotation_vector(self, rotvec):
        r = R.from_rotvec(rotvec)
        self.rotation = r.as_dcm()

    def get_scalar_focal(self):
        log.check_eq(self.focal[0], -self.focal[1], "pixels must be square")
        return self.focal[0]

    def set_scalar_focal(self, scalar):
        self.focal = np.asarray([scalar, -scalar])

    """ FOV """

    def get_fov(self):
        return math.acos(self.cos_fov)

    def set_fov(self, fov):
        self.cos_fov = math.cos(fov)
        log.check_ge(self.cos_fov, get_default_cos_fov(self.type))

    def set_default_fov(self):
        self.cos_fov = get_default_cos_fov(self.type)

    def is_default_fov(self):
        return self.cos_fov == get_default_cos_fov(self.type)

    def is_behind(self, point):
        return np.dot(self.backward(), point - self.position) >= 0

    def is_outside_fov(self, point):
        if self.cos_fov == -1:
            return False
        if self.cos_fov == 0:
            return self.is_behind(point)
        v = point - self.position
        dotprod = np.dot(self.forward(), v)
        v_norm_squared = math.pow(linalg.norm(v), 2)
        return (
            dotprod * abs(dotprod) <= self.cos_fov * abs(self.cos_fov) * v_norm_squared
        )

    def is_outside_image_circle(self, pixel):
        if self.is_default_fov():
            return False

        # find an edge point by projecting a point from the FOV cone
        sin_fov = math.sqrt(1 - self.cos_fov * self.cos_fov)
        edge = self.camera_to_sensor(np.array([0, sin_fov, -self.cos_fov]))

        # pixel is outside FOV if its farther from the principal than the edge point
        sensor = (pixel - self.principal) / self.focal
        return math.pow(linalg.norm(sensor), 2) >= linalg.norm(edge)

    def is_outside_sensor(self, pixel):
        pixX = pixel[0]
        pixY = pixel[1]
        resX = self.resolution[0]
        resY = self.resolution[1]
        return not (0 <= pixX and pixX < resX and 0 <= pixY and pixY < resY)

    def sees(self, point):
        if self.is_outside_fov(point):
            return (False, None)
        pixel = self.world_to_pixel(point)
        return (not self.is_outside_sensor(pixel), pixel)

    def overlap(self, camera):
        """estimate the fraction of the frame that is covered by the other camera"""
        # just brute force probeCount x probeCount points
        kProbeCount = 10
        inside = 0
        for y in range(0, kProbeCount):
            for x in range(0, kProbeCount):
                p = np.array([x, y]) * self.resolution / (kProbeCount - 1)
                if (
                    not self.is_outside_image_circle(p)
                    and camera.sees(self.point_near_infinity(p))[0]
                ):
                    inside += 1

        return inside / math.pow(kProbeCount, 2)

    """ Rescale """

    def rescale(self, resolution):
        self.principal = self.principal * (np.asarray(resolution) / self.resolution)
        self.focal = self.focal * (np.asarray(resolution) / self.resolution)
        self.resolution = np.asarray(resolution)

    def normalize(self):
        self.principal = self.principal / self.resolution
        self.focal = self.focal / self.resolution
        self.resolution = np.ones(2)

    def is_normalized(self):
        return np.array_equal(self.resolution, np.ones(2))

    """ Distortion """

    def get_distortion(self):
        return self.__distortion

    def get_distortion_max(self):
        return self.__distortion_max

    def set_default_distortion(self):
        self.__distortion = np.zeros(3)
        self.__distortion_max = INFINITY

    def set_distortion(self, distortion):
        count = distortion.size
        while distortion[count - 1] == 0:
            count -= 1
            if count == 0:
                return self.set_default_distortion()

        # distortion polynomial is x + d[0] * x^3 + d[1] * x^5 ...
        # derivative is: 1 + d[0] * 3 x^2 + d[1] * 5 x^4 ...
        # using y = x^2: 1 + d[0] * 3 y + d[1] * 5 y^2 ...
        derivative = np.empty(count + 1)
        derivative[0] = 1
        for i in range(0, count):
            derivative[i + 1] = distortion[i] * (2 * i + 3)

        # find smallest rea; root greater than zero in the derivative
        roots = poly.polyroots(derivative)
        y = INFINITY
        for root in roots:
            if np.isreal(root) and 0 < root and root < y:
                y = np.real(root)  # "convert" to real due to casting warnings

        self.__distortion = distortion
        self.__distortion_max = math.sqrt(y)

    # distortion is modeled in pixel space as:
    #   distort(r) = r + d0 * r^3 + d1 * r^5
    def distort(self, r):
        r = min(r, self.__distortion_max)
        return self.__distort_factor(r * r) * r

    def __distort_factor(self, r_squared):
        index = self.get_distortion().size - 1
        result = self.get_distortion()[index]
        while index > 0:
            index -= 1
            result = self.get_distortion()[index] + r_squared * result
        return 1 + r_squared * result

    def undistort(self, y):
        if np.count_nonzero(self.get_distortion()) == 0:
            return y
        if y >= self.distort(self.__distortion_max):
            return self.__distortion_max

        smidgen = 1.0 / K_NEAR_INFINITY
        k_max_steps = 10

        x0 = 0
        y0 = 0
        dy0 = 1
        for _step in range(0, k_max_steps):
            x1 = (y - y0) / dy0 + x0
            y1 = self.distort(x1)
            if abs(y1 - y) < smidgen:
                return x1
            dy1 = (self.distort(x1 + smidgen) - y1) / smidgen
            log.check_ge(dy1, 0, "went past a maximum")
            x0 = x1
            y0 = y1
            dy0 = dy1
        return x0  # should not happen

    """ projection """

    def world_to_pixel(self, point):
        """Compute pixel coordinates from point in rig space"""
        # transform point in rig space to camera space
        camera_point = np.matmul(self.rotation, (np.asarray(point) - self.position))
        # transform point in camera space to distorted sensor coordinates
        sensor_point = self.camera_to_sensor(camera_point)
        # transform distorted sensor coordinates to pixel coordinates
        return sensor_point * self.focal + self.principal

    def pixel_to_world(self, pixel, depth=None):
        """Compute rig coordinates from pixel and return as a parameterized line"""
        # transform from pixel to distorted sensor coordinates
        sensor_point = (pixel - self.principal) / self.focal
        # transform from distorted sensor coordinates to unit camera vector
        unit = self.sensor_to_camera(sensor_point)
        # transform from camera space to rig space
        ray = Ray(self.position, np.matmul(self.rotation.T, unit))
        if depth:
            return ray.point_at(depth)
        else:
            return ray

    def camera_to_sensor(self, camera_point):
        # FTHETA: r = theta
        # RECTILINEAR: r = tan(theta)
        # EQUISOLID: r = 2 sin(theta / 2)
        # ORTHOGRAPHIC: r = sin(theta)
        # see https://wiki.panotools.org/Fisheye_Projection
        if self.type == "FTHETA":
            # r = theta <=>
            # r = atan2(|xy|, -z)
            xy = linalg.norm(camera_point[:2])
            unit = normalize_vector(camera_point[:2])
            r = math.atan2(xy, -camera_point[2])
            return self.distort(r) * unit
        elif self.type == "RECTILINEAR":
            # r = tan(theta) <=>
            # r = |xy| / -z <=>
            # pre-distortion result is xy / -z
            xy = linalg.norm(camera_point[:2])
            unit = normalize_vector(camera_point[:2])
            if -camera_point[2] <= 0:  # outside FOV
                r = math.tan(math.pi / 2)
            else:
                r = xy / -camera_point[2]
            return self.distort(r) * unit
        elif self.type == "EQUISOLID":
            # r = 2 sin(theta / 2) <=>
            # using sin(theta / 2) = sqrt((1 - cos(theta)) / 2)
            # r = 2 sqrt((1 + z / |xyz|) / 2)
            xy = linalg.norm(camera_point[:2])
            unit = normalize_vector(camera_point[:2])
            r = 2 * math.sqrt((1 + camera_point[2] / linalg.norm(camera_point)) / 2)
            return self.distort(r) * unit
        elif self.type == "ORTHOGRAPHIC":
            # r = sin(theta) <=>
            # r = |xy| / |xyz| <=>
            # pre-distortion result is xy / |xyz|
            xy = linalg.norm(camera_point[:2])
            unit = normalize_vector(camera_point[:2])
            if -camera_point[2] <= 0:  # outside FOV
                r = math.sin(math.pi / 2)
            else:
                r = linalg.norm(camera_point[:2] / linalg.norm(camera_point))
            return self.distort(r) * unit
        else:
            log.fatal(f"unexpected type {self.type}")

    def sensor_to_camera(self, sensor_point):
        norm = linalg.norm(sensor_point)
        norm_squared = math.pow(norm, 2)

        if norm_squared == 0:
            return np.array([0, 0, -1])

        r = self.undistort(norm)
        if self.type == "FTHETA":
            theta = r
        elif self.type == "RECTILINEAR":
            theta = math.atan(r)
        elif self.type == "EQUISOLID":
            # arcsin function is undefined outside the interval [-1, 1]
            theta = 2 * math.asin(r / 2) if r <= 2 else math.pi
        elif self.type == "ORTHOGRAPHIC":
            theta = math.asin(r) if r <= 1 else math.pi / 2
        else:
            log.fatal(f"unexpected type {self.type}")

        unit = np.asarray(math.sin(theta) / norm * sensor_point)
        unit = np.append(unit, -math.cos(theta))
        return unit

    def point_near_infinity(self, pixel):
        return self.pixel_to_world(pixel, K_NEAR_INFINITY)


def get_default_cos_fov(type):
    if type == "RECTILINEAR" or type == "ORTHOGRAPHIC":
        return 0  # hemisphere
    elif type == "FTHETA" or type == "EQUISOLID":
        return -1  # sphere
    else:
        log.fatal(f"unexpected type {type}")


def deserialize_list(list, length=None):
    if length is not None:
        log.check_eq(len(list), length, "list is not the expected length")
    return np.asarray(list)
