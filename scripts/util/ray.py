#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

import numpy as np
from logger import Logger
from matrix_operations import normalize_vector

log = Logger(__name__)
log.logger.propagate = False


class Ray:

    """The Ray class represents a parameterized line

    Attributes:
        origin (numpy 1x3 array) : xyz coordinates of the origin point of the ray
        direction (numpy 1x3 array) : direction of the line as a unit vector
    """

    def __init__(self, origin, direction):
        log.check_eq(len(origin), len(direction), "dimensions do not match")
        direction = normalize_vector(direction)
        self.origin = np.asarray(origin)
        self.direction = np.asarray(direction)

    def point_at(self, depth):
        return self.origin + self.direction * depth
