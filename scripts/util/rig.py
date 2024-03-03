#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

import json
import random

from camera import Camera
from logger import Logger

log = Logger(__name__)
log.logger.propagate = False

RAND_MAX = 2147483647


class Rig:
    """The Rig class represents a list of Cameras

    Attributes:
        cameras ([Camera]) : list of cameras in the rig
        comments ([string]) : list of comments
    """

    def __init__(self, json_file=None, json_string=None):
        if json_file:
            self.load_from_json_file(json_file)
        elif json_string:
            self.load_from_json_string(json_string)

    def load_from_json_file(self, json_file):
        with open(json_file) as f:
            json_string = f.read()
            log.check(json_string)  # check that JSON file was read
        self.load_from_json_string(json_string)

    def load_from_json_string(self, json_string):
        json_obj = json.loads(json_string)

        self.cameras = []
        json_cameras = json_obj["cameras"]
        for json_camera in json_cameras:
            json_camera_string = json.dumps(json_camera)
            camera = Camera(json_string=json_camera_string)
            self.cameras.append(camera)

        self.comments = []
        if "comments" in json_obj:
            json_comments = json_obj["comments"]
            for json_comment in json_comments:
                json_comment_string = json.dumps(json_comment)
                self.comments.append(json_comment_string)

    def normalize(self):
        for cam in self.cameras:
            if not cam.is_normalized():
                cam.normalize()

    def find_camera_by_id(self, id):
        for cam in self.cameras:
            if cam.id == id:
                return cam
        log.fatal(f"Camera id {id} not found")

    def save_to_file(self, filename):
        rig_json = {"cameras": []}
        for camera in self.cameras:
            camera_json = camera.serialize()
            rig_json["cameras"].append(camera_json)
        rig_json.update({"comments": self.comments})
        with open(filename, "w") as f:
            f.write(json.dumps(rig_json, sort_keys=True, indent=2))

    @classmethod
    def perturb_cameras(
        cls, cameras, pos_amount, rot_amount, principal_amount, focal_amount
    ):
        for i in range(0, len(cameras)):
            camera = cameras[i]
            if i != 0:
                camera.position = perturb_vector(camera.position, pos_amount)
                rotvec = perturb_vector(camera.get_rotation_vector(), rot_amount)
                camera.set_rotation_vector(rotvec)
            camera.principal = perturb_vector(camera.principal, principal_amount)
            if focal_amount != 0:
                scalar_focal = perturb_scalar(camera.get_scalar_focal(), focal_amount)
                camera.set_scalar_focal(scalar_focal)


def perturb_scalar(scalar, amount):
    return scalar + amount * 2 * (random.randint(0, RAND_MAX) / RAND_MAX - 0.5)


def perturb_vector(vector, amount):
    for i in range(0, len(vector)):
        vector[i] = perturb_scalar(vector[i], amount)
    return vector
