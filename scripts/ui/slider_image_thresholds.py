#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Slider class for interacting with the threshold displays.

Example:
    To see an instance of the SliderWidget class, refer to common.py:

        slider = SliderWidget(type, attr, name, printed, hs, label, max, default)
"""

import dep_util
from PyQt5 import QtWidgets


class SliderWidget(QtWidgets.QWidget):

    """Slider class for interacting with the threshold displays.

    Attributes:
        attr (class.attribute): Attribute being controlled by the slider.
        count (int): Discrete number of steps in the range.
        default (float): Starting value.
        hs (QtWidgets.QSlider): UI element tied to the widget.
        label (str): Label displayed next to the slider.
        max (float): Maximum of the slider value range.
        name (str): Internal name of the slider object.
        printed_name (str): Displayed slider name.
        type (Union[ColorVariance, ForegroundMask]): Which threshold app to interact with.
    """

    def __init__(
        self,
        type,
        attr,
        qt_label_name,
        printed_name,
        qt_slider,
        qt_label,
        max,
        default,
        parent=None,
    ):
        """Initializes slider class for interacting with the threshold displays.

        Args:
            type (Union[ColorVariance, ForegroundMask]): Which threshold app to interact with.
            attr (class.attribute): Attribute being controlled by the slider.
            qt_label_name (str): Internal name of the slider object.
            printed_name (str): Displayed slider name.
            qt_slider (QtWidgets.QSlider): UI element tied to the widget.
            qt_label (str): Label displayed next to the slider.
            max (float): Maximum of the slider value range.
            default (float): Starting value.
            parent (App(QDialog), optional): Object corresponding to the parent UI element.
        """
        super().__init__(parent)
        self.type = type  # color_variance, fg_mask
        self.attr = attr  # noise, detail, blur, closing, thresh
        self.name = qt_label_name
        self.printed_name = printed_name
        self.hs = qt_slider
        self.label = qt_label
        self.max = max
        self.default = default
        self.count = 100
        self.hs.setEnabled(False)
        self.label.clear()

    def setup(self, callback):
        """Sets up the slider class with default values and a changed value event.

        Args:
            callback (func : float -> float): Callback associated with updating value.
        """
        self.name.setText(self.printed_name)
        self.hs.setMinimum(0)
        self.hs.setMaximum(self.count - 1)
        self.hs.setEnabled(True)
        dep_util.disconnect(self.hs.valueChanged)
        self.hs.valueChanged.connect(
            lambda value: callback(self, self.slider_to_val(value))
        )
        label_text = self.label.text()
        if label_text != "":
            self.set_slider(float(label_text))
        self.hs.show()
        self.label.show()

    def hide(self):
        """Hides the slider."""
        self.hs.hide()
        self.label.hide()

    def set_name(self, text):
        """Sets the name of the slider.

        Args:
            text (str): Name of the slider.
        """
        self.name.setText(text)

    def set_label(self, text, notation="{}"):
        """Sets the label of the slider.

        Args:
            text (str): Label of the slider.
            notation (str, optional): Formatted notation for the display.
        """
        self.label.setText(notation.format(text))

    def get_label_text(self):
        """Finds the label of the slider.

        Returns:
            str: Label of the slider.
        """
        return self.label.text()

    def set_slider(self, val):
        """Updates the slider value.

        Args:
            val (float): Value corresponding to an attribute.
        """
        self.hs.setValue(self.val_to_slider(val))

    def set_slider_and_label(self, val):
        """Updates the slider value and adjust its text.

        Args:
            val (float): Value corresponding to an attribute.

        Returns:
            float: Value set to the slider.
        """
        if not val:
            val = self.default
        self.set_label(str(val))
        self.set_slider(float(val))
        return val

    def slider_to_val(self, slider_val):
        """Converts from a slider value to the corresponding attribute value.

        Args:
            slider_val (float): Value corresponding to the slider.

        Returns:
            float: Value corresponding to the attribute.
        """
        return slider_val * self.max / self.count

    def val_to_slider(self, val):
        """Converts from an attribute value to the corresponding value in the slider.

        Args:
            val (float): Value corresponding to the attribute.

        Returns:
            float: Value corresponding to the slider.
        """
        return val / self.max * self.count
