#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Front end display for the threshold interaction models for the UI.

Defines the interaction model for both the color variance and foreground masks boxes.
This is used in the depth estimation tab indirectly through the image thresholds widget.

Example:
    To see an instance of ImageViewer, refer to the Background tab:

    (From dep.ui)
        <item row="0" column="0">
         <widget class="ImageViewer" name="w_bg_image_viewer_color_variance" native="true"/>
        </item>
"""

from PyQt5 import QtCore, QtGui, QtWidgets


class ImageViewer(QtWidgets.QGraphicsView):

    """Front end display for the threshold interaction models for the UI.

    Attributes:
        factor (float): Ratio of the image size to the scene size.
        pixmap_item (QtWidgets.QGraphicsPixmapItem): Image being thresholded.
        scene (QtWidgets.QGraphicsScene): UI portioned off to image display.
        zoom_level (float): Zoom of the displayed image.
    """

    def __init__(self, parent=None):
        """Initializes the front-end for thresholds visualization with default style and
        no displayed image.

        Args:
            parent (App(QDialog), optional): Object corresponding to the parent UI element.
        """
        super(ImageViewer, self).__init__(parent)
        self.scene = QtWidgets.QGraphicsScene(self)
        self.pixmap_item = QtWidgets.QGraphicsPixmapItem()
        self.scene.addItem(self.pixmap_item)
        self.setScene(self.scene)
        self.setDragMode(QtWidgets.QGraphicsView.ScrollHandDrag)
        self.setStyleSheet("background:transparent;")
        self.zoom_level = -1
        self.factor = -1

    def fit_in_view(self):
        """Adjust zoom to fit the image on screen."""
        r_pixmap = QtCore.QRectF(self.pixmap_item.pixmap().rect())
        if not r_pixmap.isNull():
            self.setSceneRect(r_pixmap)
            if not self.pixmap_item.pixmap().isNull():
                bb = self.transform().mapRect(QtCore.QRectF(0, 0, 1, 1))
                self.scale(1 / bb.width(), 1 / bb.height())
                r_view = self.viewport().rect()
                r_scene = self.transform().mapRect(r_pixmap)
                w_ratio = r_view.width() / r_scene.width()
                h_ratio = r_view.height() / r_scene.height()
                self.factor = min(w_ratio, h_ratio)
                self.scale(self.factor, self.factor)

    def set_zoom_level(self, zoom):
        """Adjust the displayed zoom.

        Args:
            zoom (float): Zoom level to use.
        """
        self.zoom_level = zoom

    def set_pixmap(self, pixmap=None):
        """Updates the displayed image to a given QT image.

        Args:
            QtGui.QPixmap: Identical image in QT image format.
        """
        pm = pixmap
        if not pm or pm.isNull():
            pm = QtGui.QPixmap()
        self.pixmap_item.setPixmap(pm)

        if self.zoom_level <= 0:
            self.zoom_level = 0
            self.fit_in_view()

    def update_params(self, event):
        """Handles update events to adjust the displayed thresholds accordingly.

        Args:
            event (QEvent): Event instance being handled.
        """
        if event.angleDelta().y() > 0:
            self.factor = 1.25
            self.zoom_level += 1
        else:
            self.factor = 0.75
            self.zoom_level -= 1

    def scale_image(self):
        """Scales the displayed image according to the zoom."""
        if self.zoom_level > 0:
            self.scale(self.factor, self.factor)
        else:
            self.zoom_level = 0
            self.fit_in_view()

    def wheelEvent(self, event):
        """Event handler for a mouse wheel scroll (causes zoom).

        Args:
            event (QEvent): Event instance being handled.
        """
        if not self.pixmap_item.pixmap().isNull():
            self.update_params(event)
            self.scale_image()
