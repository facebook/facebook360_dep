#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Back end for the color variance interaction model for the UI.

Defines the interaction model for the color variance box. This is used in both
the background and depth estimation tabs currently to display regions that will
be used for depth estimation (i.e. regions with high variance/texture).

Example:
    To see an instance of ColorVariance, refer to the example in widget_image_thresholds.py:

        >>> color_variance = ColorVariance()
"""

import cv2
import dep_util
import numpy as np


class ColorVariance:
    """Back-end for the interactive element to visualize color variance thresholds.

    Attributes:
        detail (float): Lower threshold on variance, for enough texture.
        image_8bit (np.array[uint8]): Image to be thresholded (loaded as 8-bit).
        image_float (np.array[float32]): Image to be thresholded (loaded as float).
        image_unchanged (np.array[_]): Image to be thresholded (loaded unchanged).
        image_var (float): Variance of the image.
        min_var (float): min_variance desired in a patch to be used for depth estimation.
        noise (float): Upper threshold on variance, above which is noise.
        ready (bool): Whether or not the element is ready to display.
        scale_var (int): Scale to apply to image variance.
    """

    def __init__(self, parent=None):
        """Element in UI responsible for visualizing variances in color images.

        Args:
            parent (App(QDialog), optional): Object corresponding to the parent UI element.
        """
        self.image_float = None
        self.image_8bit = None
        self.image_unchanged = None
        self.min_var = 1 / 12 / 65025
        self.noise = -1
        self.detail = -1
        self.scale_var = -1
        self.ready = True

    def reset_params(self):
        """Resets to default values for interaction."""
        self.noise = -1
        self.detail = -1

    def compute_image_variance(self, image_float):
        """Computes variance on a single-dimension image.

        Args:
            image_float (np.array[float32]): 1D floating point image

        Returns:
            float: Variance across the image.
        """
        win = 3
        mean, mean_of_squares = (
            cv2.boxFilter(x, -1, (win, win), borderType=cv2.BORDER_REFLECT)
            for x in (image_float, image_float * image_float)
        )
        var = mean_of_squares - mean * mean
        return var.sum(axis=2) / 3

    def set_image(self, filename, res=2048):
        """Updates parameters of the display to an image on disk.

        Args:
            filename (str): Path to an image file.
            res (int, optional): Resolution of the image.
        """
        self.reset_params()

        # Load and resize image
        image = cv2.imread(filename, cv2.IMREAD_UNCHANGED)
        h, w, _ = image.shape
        scale = int(res) / w
        self.image_unchanged = dep_util.scale_image(image, scale)
        self.scale_var = scale**2

        self.image_float = dep_util.convert_to_float(self.image_unchanged)
        self.image_8bit = (255 * self.image_float).astype("uint8")
        self.image_var = self.compute_image_variance(self.image_float)

    def apply_thresholds(self, noise=-1, detail=-1):
        """Updates the UI display to new noise and detail thresholds.

        Args:
            noise (float, optional): Upper threshold on variance, above which is noise.
            detail (float, optional): Lower threshold on variance, for enough texture.

        Returns:
            np.array[int, int, int]: Colored threshold image, with blue and magenta
                resp. marking areas of noise and blue areas with sufficient texture.
        """
        if type(self.image_8bit) is not np.ndarray:
            return None

        if noise >= 0:
            self.noise = noise
        if detail >= 0:
            self.detail = detail

        # Ignore if we don't have values for all the parameters we need
        if self.noise < 0 or self.detail < 0:
            return None

        noise_show = max(self.noise * self.scale_var, self.min_var)
        detail_show = max(self.detail, noise_show)
        image_marked = self.image_8bit.copy()
        image_marked[self.image_var < noise_show] = [255, 0, 0]  # blue
        image_marked[self.image_var > detail_show] = [255, 0, 255]  # magenta
        return image_marked
