#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""QT ComboBox class (selectable dropdown) with checkboxes and helper classes.

Example:
    To see an instance of the ComboBoxCheckable class, refer to the DepthEstimation tab:

    (From dep.ui)
        <widget class="ComboBoxCheckable" name="dd_depth_farm_workers">
          <property name="font">
          ...
        </widget>

"""

from PyQt5 import QtCore, QtGui, QtWidgets


class ComboBoxCheckable(QtWidgets.QComboBox):

    """UI dropdown element with checkbox selection."""

    def __init__(self, parent=None):
        """Creates default and connects components of the dropdown.

        Args:
            parent (App(QDialog), optional): Object corresponding to the parent UI element.
        """
        super(ComboBoxCheckable, self).__init__(parent)
        self.setView(QtWidgets.QListView(self))
        self.setItemDelegate(CheckDelegate(self))
        self.setModel(CheckableComboBoxModel(self))

    def checkedItems(self):
        """Gets selected items.

        Returns:
            list[str]: Text corresponding to checked items.
        """
        checkedItems = []
        for index in range(self.count()):
            item = self.model().item(index)
            if item.checkState() == QtCore.Qt.Checked:
                checkedItems.append(item.text())
        return checkedItems


class CheckableComboBoxModel(QtGui.QStandardItemModel):

    """Back-end for the UI dropdown element with checkbox selection."""

    def __init__(self, parent=None):
        """Initializes the specialized UI dropdown back-end.

        Args:
            parent (App(QDialog), optional): Object corresponding to the parent UI element.
        """
        super(CheckableComboBoxModel, self).__init__(parent)

    def flags(self, index):
        """Finds status of an item in the dropdown.

        Args:
            index (int): Item in the dropdown being referenced.

        Returns:
            bool: Whether or not the selection is marked.
        """
        return ~QtCore.Qt.ItemIsSelectable | QtCore.Qt.ItemIsEnabled


class CheckDelegate(QtWidgets.QStyledItemDelegate):

    """Helper class to represent items in the specialized UI dropdown element."""

    def editorEvent(self, event, model, option, index):
        """Callback event handler for updating selection in the dropdown.

        Args:
            event (QEvent): Event instance being handled.
            model (QtGui.QStandardItemModel): Back-end instance to determine selection state.
            option (QStyleOptionViewItem): Configuration for displaying the widget.
            index (QModelIndex): Item in the dropdown being referenced.

        Returns:
            bool: Success of the update operation.
        """
        if event.type() == QtCore.QEvent.MouseButtonRelease:
            val = index.data(QtCore.Qt.CheckStateRole)
            new_val = (
                QtCore.Qt.Checked if val == QtCore.Qt.Unchecked else QtCore.Qt.Unchecked
            )
            model.setData(index, new_val, QtCore.Qt.CheckStateRole)
            return True
        return QtWidgets.QStyledItemDelegate.editorEvent(
            self, event, model, option, index
        )
