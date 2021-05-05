#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Back end for the foreground masks interaction model for the UI.

Defines the interaction model for the foreground masks box. This is used in
the depth estimation tab to segment the image into foreground and background
by manipulating blur, closing, and threshold.

Example:
    To see an instance of ForegroundMask, refer to the example in widget_image_thresholds.py:

        >>> foreground_mask = ForegroundMask()
"""

import cv2
import dep_util
import numpy as np


class ForegroundMask:

    """Back-end for the interactive element to visualize foreground mask thresholds.

    Attributes:
        blur (int): Gaussian blur radius.
        closing (int): Closure (for sealing holes).
        image_8bit_bg (np.array[uint8]): Thresholded background image (loaded as 8-bit).
        image_8bit_fg (np.array[uint8]): Thresholded foreground image (loaded as 8-bit).
        image_float_bg (np.array[float]): Thresholded background image (loaded as floats).
        image_float_fg (np.array[float]): Thresholded foreground image (loaded as floats).
        image_unchanged_bg (np.array[_]): Thresholded background image (loaded unchanged from disk).
        image_unchanged_fg (np.array[_]): Thresholded foreground image (loaded unchanged from disk).
        ready (bool): Whether or not the element is ready to display.
        thresh (int): Threshold applied to segment foreground and background
    """

    def __init__(self, parent=None):
        """Initializes the back-end for foreground mask thresholds visualization with default
        parameters and no images.

        Args:
            parent (App(QDialog), optional): Object corresponding to the parent UI element.
        """
        self.image_float_fg = None
        self.image_float_bg = None
        self.image_8bit_fg = None
        self.image_8bit_bg = None
        self.image_unchanged_fg = None
        self.image_unchanged_bg = None
        self.blur = -1
        self.closing = -1
        self.thresh = -1
        self.ready = True

    def reset_params(self):
        """Sets blur, closing, and threshold to default values."""
        self.blur = -1
        self.closing = -1
        self.thresh = -1

    def set_images(self, filename_bg, filename_fg, res=2048):
        """Updates the foreground and background images per images on disk.

        Args:
            filename_bg (str): Path to the background image.
            filename_fg (str): Path to the foreground image.
            res (int, optional): Resolution of both images (assumed to be the same).
        """
        self.reset_params()

        # Load and resize images
        self.image_unchanged_bg = dep_util.load_image_resized(filename_bg, res)
        self.image_unchanged_fg = dep_util.load_image_resized(filename_fg, res)

        self.image_float_bg = dep_util.convert_to_float(self.image_unchanged_bg)
        self.image_float_fg = dep_util.convert_to_float(self.image_unchanged_fg)
        self.image_8bit_bg = (255 * self.image_float_bg).astype("uint8")
        self.image_8bit_fg = (255 * self.image_float_fg).astype("uint8")

    def generate_fg_mask(self, image_bg, image_fg, blur, closing, thresh):
        """Creates a segmented (0, 255) image of the background vs. foreground.

        Args:
            image_bg (np.array[_]): Image of the background frame.
            image_fg (np.array[_]): Image of the foreground frame.
            blur (int): Gaussian blur radius.
            closing (int): Closure (for sealing holes).
            thresh (int): Threshold applied to segment foreground and background

        Returns:
            np.array[uint8]: Segmented image with 0 marking the background and 255 the foreground.
        """
        blur_dims = (2 * blur + 1, 2 * blur + 1)
        bg_blur = cv2.GaussianBlur(image_bg, blur_dims, 0)
        fg_blur = cv2.GaussianBlur(image_fg, blur_dims, 0)

        # mask = ||template - frame||^2 > threshold
        diff = cv2.absdiff(bg_blur, fg_blur)
        mask = np.sum(diff ** 2, axis=2) ** (1.0 / 2) > thresh
        mask = np.array(mask, dtype=np.uint8)

        # Fill holes
        if closing > 0:
            element = cv2.getStructuringElement(cv2.MORPH_RECT, (closing, closing))
            mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, element)

        return mask

    def apply_thresholds(self, blur=-1, closing=-1, thresh=-1):
        """Displays colored visualization of the foreground masking.

        Args:
            blur (int, optional): Gaussian blur radius.
            closing (int, optional): Closure (for sealing holes).
            thresh (int, optional): Threshold applied to segment foreground and background

        Returns:
            np.array[uint8, uint8, uint8]: Colored image where green represents the foreground.
        """
        if (
            type(self.image_8bit_fg) is not np.ndarray
            or type(self.image_8bit_bg) is not np.ndarray
        ):
            return None

        if blur >= 0:
            self.blur = int(blur)
        if closing >= 0:
            self.closing = int(closing)
        if thresh >= 0:
            self.thresh = thresh

        # Ignore if we don't have values for all the parameters we need
        if self.blur < 0 or self.closing < 0 or self.thresh < 0:
            return None

        if not self.ready:
            return None

        mask = self.generate_fg_mask(
            self.image_float_bg,
            self.image_float_fg,
            self.blur,
            self.closing,
            self.thresh,
        )

        mask_rgb = np.stack((mask,) * 3, axis=-1)
        mask_rgb[mask > 0] = [0, 255, 0]  # green

        # Overlay mask on top of color for visualization purposes
        return cv2.addWeighted(self.image_8bit_fg, 1, mask_rgb, 0.5, 0)
