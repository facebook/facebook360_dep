#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

import numpy as np
from scipy import linalg


def is_unitary(matrix, atol=None):
    # a matrix M is unitary <=> the conjugate transpose H = the inverse of M <=> H * M = I
    if atol:
        return np.allclose(
            np.eye(len(matrix)), np.matmul(matrix, matrix.T.conj()), atol, atol
        )
    else:
        return np.allclose(np.eye(len(matrix)), np.matmul(matrix, matrix.T.conj()))


def is_approx(v1, v2, tol=0):
    return linalg.norm(v1 - v2) <= tol * min(linalg.norm(v1), linalg.norm(v2))


def normalize_vector(vector):
    if linalg.norm(vector) == 0:
        return vector
    return vector / linalg.norm(vector)
