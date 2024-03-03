#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""QT ComboBox class (selectable dropdown) with typable autocomplete.

Example:
    To see an instance of the ComboBoxAutocomplete class, refer to the Calibration tab:

    (From dep.ui)
        <widget class="ComboBoxAutocomplete" name="dd_calibrate_calibrate_frame">
            <property name="font">
            ...
        </widget>

"""

from PyQt5.QtCore import QSortFilterProxyModel, Qt
from PyQt5.QtWidgets import QComboBox, QCompleter


class ComboBoxAutocomplete(QComboBox):
    """UI dropdown element with autocomplete.

    Attributes:
        completer (QtWidgets.QCompleter): Autocomplete instance tied to the dropdown items.
        proxy_model (QtWidgets.QSortFilterProxyModel): Configurations of the autocomplete.
    """

    def __init__(self, parent=None):
        """Creates default configurations (case insensitive) and constructs the UI element.

        Args:
            parent (App(QDialog), optional): Object corresponding to the parent UI element.
        """
        super(ComboBoxAutocomplete, self).__init__(parent)

        self.setFocusPolicy(Qt.StrongFocus)
        self.setEditable(True)

        # Setup proxy model to sort and filter data passed between model and view
        self.proxy_model = QSortFilterProxyModel(self)
        self.proxy_model.setFilterCaseSensitivity(Qt.CaseInsensitive)
        self.proxy_model.setSourceModel(self.model())

        # Setup completer
        self.completer = QCompleter(self.proxy_model, self)
        self.completer.setCompletionMode(QCompleter.UnfilteredPopupCompletion)
        self.setCompleter(self.completer)

        # Connect autocompletion signals
        self.lineEdit().textEdited.connect(self.proxy_model.setFilterFixedString)
        self.completer.activated.connect(self.on_completer_activated)

    # Override
    def on_completer_activated(self, text):
        """Callback event handler for a query in the autocomplete.

        Args:
            text (str): Query text.
        """
        if not text:
            return
        index = self.findText(text)
        self.setCurrentIndex(index)
        self.activated[str].emit(self.itemText(index))

    # Override
    def setModel(self, model):
        """Updates the configuration model.

        Args:
            model (QtWidgets.QSortFilterProxyModel): New configuration to update to.
        """
        super(ComboBoxAutocomplete, self).setModel(model)
        self.proxy_model.setSourceModel(model)
        self.completer.setModel(self.proxy_model)

    # Override
    def setModelColumn(self, column):
        """Updates the column being autocompleted.

        Args:
            column (int): Column from the data used for checking suggestions.
        """
        self.completer.setCompletionColumn(column)
        self.proxy_model.setFilterKeyColumn(column)
        super(ComboBoxAutocomplete, self).setModelColumn(column)
