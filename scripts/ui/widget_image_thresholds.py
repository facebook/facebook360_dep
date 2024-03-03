#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Widgets that fronts the interactive thresholding UI elements.

ForegroundMasks and ColorVariance serve as the back-end for the interactive ImageViewer UI
element, tied together in this class.

Example:
    To see an instance of the ComboBoxAutocomplete class, refer to the DepthEstimation tab:

    (From dep.ui)
        <widget class="ImageWidget" name="w_depth_threshs_image_fg_mask" native="true">
          <layout class="QGridLayout" name="gridLayout_105">
           ...
        </widget>
"""

import numpy as np
from color_variance import ColorVariance
from dep_util import convert_image_to_pixmap
from foreground_mask import ForegroundMask
from PyQt5 import QtGui, QtWidgets


class ImageWidget(QtWidgets.QWidget):
    """Element in the UI responsible for displaying interactive thresholds (both
    foreground mask and color variance).

    Attributes:
        color_var (ColorVariance): Instance of ColorVariance UI element.
        fg_mask (ForegroundMask): Instance of ForegroundMask UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        image_viewer (ImageViewer): Instance of ImageViewer UI element.
    """

    def __init__(self, parent=None):
        """Initializes the ImageWidget UI element.

        Args:
            parent (App(QDialog), optional): Object corresponding to the parent UI element.
        """
        super().__init__(parent)
        self.color_var = ColorVariance()
        self.fg_mask = ForegroundMask()
        self.gb = None
        self.image_viewer = None

    def set_zoom_level(self, zoom):
        """Updates the UI display to a given zoom.

        Args:
            zoom (float): Zoom of the image display.
        """
        if self.image_viewer:
            self.image_viewer.set_zoom_level(zoom)

    def set_image_viewer(self, image_viewer):
        """Updates viewer instance.

        Args:
            image_viewer (ImageViewer): Instance to be used to display images.
        """
        self.image_viewer = image_viewer

    def update_thresholds(self, noise=-1, detail=-1, blur=-1, closing=-1, thresh=-1):
        """Displays image with new thresholds.

        Args:
            noise (float, optional): Upper threshold on variance, above which is noise.
            detail (float): Lower threshold on variance, for enough texture.
            blur (int, optional): Gaussian blur radius.
            closing (int, optional): Closure (for sealing holes).
            thresh (int, optional): Threshold applied to segment foreground and background

        Returns:
            bool: Whether the operation succeeded.
        """
        if noise >= 0 or detail >= 0:
            image = self.color_var.apply_thresholds(noise, detail)
        elif blur >= 0 or closing >= 0 or thresh >= 0:
            image = self.fg_mask.apply_thresholds(blur, closing, thresh)
        else:
            return False

        if type(image) is not np.ndarray:
            return False

        self.image_viewer.set_pixmap(convert_image_to_pixmap(image))
        return True
