#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Calibration tab for the UI.

Defines the interaction model for the background tab in the UI. This tab cannot
be run in isolation and expects a very particular structure defined in dep.ui.
UI extensions to the tab can be made by modifying the QT structure and adding
corresponding functionality here.

Example:
    To see an instance of Calibration, refer to the example in dep.py:

        >>> calibration = Calibration(self) # self refers to the overall QT UI object
"""

import os
import sys

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "render"))
sys.path.append(os.path.join(dir_scripts, "util"))

import common
import dep_util
import verify_data
from scripts.util.system_util import run_command


class Calibration:

    """Tab in UI responsible for running feature-less calibration.

    Attributes:
        dlg (App(QDialog)): Main dialog box for the app initialized by QT.
        log_reader (LogReader(QtCore.QObject)): Object to interact with UI logging system.
        parent (App(QDialog)): Object corresponding to the parent UI element.
        tag (str): Semantic name for the tab.
    """

    def __init__(self, parent):
        """Initializes the Calibration tab.

        Args:
            parent (App(QDialog)): Object corresponding to the parent UI element.
        """
        self.parent = parent
        self.tag = "calibrate"
        self.dlg = parent.dlg
        common.init(self)

    def setup_farm(self):
        """Sets up local nature of background render."""
        self.is_aws = False

    def initialize_paths(self):
        """Initializes paths for scripts and flags."""
        common.initialize_paths(self)

    def switch_ui_elements_for_processing(self, state):
        """Switches element interaction when processing.

        Args:
            state (bool): State to which elements should be changed to (i.e.
                True for enabled, False for disabled)
        """
        gb = self.dlg.gb_calibrate_calibrate
        common.switch_ui_elements_for_processing(self, gb, state)

    def on_process_finished(self, exitCode, exitStatus, p_id):
        """Callback event handler for a process completing.

        Args:
            exitCode (int): Return code from running the process.
            exitStatus (str): Description message of process exit.
            p_id (str): PID of completed process.
        """
        if self.parent.is_aws:
            remote_rigs = os.path.join(self.parent.ui_flags.project_root, "rigs/")
            run_command(f"aws s3 sync {self.path_rigs} {remote_rigs}")
        common.on_process_finished(self, p_id)

    def setup_logs(self):
        """Sets up logging system for dialog on the current tab."""
        self.log_reader = common.setup_logs(self)

    def set_default_top_level_paths(self, mkdirs=False):
        """Defines class referenceable attributes for paths. See common for full set
        of definitions.

        Args:
            mkdirs (bool, optional): Whether or not to make the defined directories.
        """
        verify_data.set_default_top_level_paths(self, mkdirs)

    def get_uncalibrated_jsons(self):
        """Finds uncalibrated rigs in the project.

        Returns:
            list[str]: List sorted by filename of uncalibrated rig files.
        """
        ps = dep_util.get_files_ext(self.path_rigs, "json")
        files = [x for x in ps if "calibrated" not in x]
        return sorted(files, key=lambda s: s.casefold())

    def get_frame_names(self):
        """Gets frames in selected calibration directory.

        Returns:
            list[str]: Sorted list of 6-digit zero-padded frame names
                 in the directory.

        Raises:
            Exception: If a directory other than background color or color
                is requested.
        """
        dir = self.dlg.dd_calibrate_calibrate_color.currentText()
        if not dir:
            return []
        dir = os.path.join(self.parent.path_project, dir)
        if dir == self.path_bg_color:
            return self.parent.frames_bg_color
        elif dir == self.path_video_color:
            return self.parent.frames_video_color
        else:
            raise Exception(f"Invalid directory to get frames from: {dir}")

    def get_valid_color_paths(self):
        """Gets the paths for valid color directories (either background
            or video or both).

        Returns:
            list[str]: Paths to the directories with valid frames.
        """
        ps = []
        if len(self.parent.frames_bg_color) > 0:
            ps.append(self.path_bg_color)
        if len(self.parent.frames_video_color) > 0:
            ps.append(self.path_video_color)
        return ps

    def get_files(self, tag):
        """Retrieves file names corresponding to the desired attribute.

        Args:
            tag (str): Semantic name for the attribute.

        Returns:
            list[str]: List of file names.

        Raises:
            Exception: If a tag is requested with no associated files.
        """
        if tag == "rigs_res":
            return self.get_uncalibrated_jsons()
        elif tag == "color":
            return self.get_valid_color_paths()
        elif tag == "frame":
            return self.get_frame_names()
        else:
            raise Exception(f"Invalid tag: {tag}")

    def update_buttons(self, gb):
        """Enables buttons and dropdowns according to whether or not data is present.

        Args:
            gb (QtWidgets.QGroupBox): Group box for the tab.
        """
        common.update_buttons(self, gb)

    def update_flags_from_data(self, flags):
        """Updates flags from the UI elements.

        Args:
            flags (dict[str, _]): Flags corresponding to the tab default binary.
        """
        dlg = self.dlg
        project = self.parent.path_project

        rig_in = os.path.join(project, dlg.dd_calibrate_rig_rigs_res.currentText())
        rig_stem = os.path.splitext(os.path.basename(rig_in))[0]
        rig_out = os.path.join(self.path_rigs, f"{rig_stem}_calibrated.json")

        flags["log_dir"] = self.path_logs
        flags["rig_in"] = rig_in
        flags["rig_out"] = rig_out
        flags["matches"] = os.path.join(self.path_calibration, "matches.json")
        flags["color"] = os.path.join(
            project, dlg.dd_calibrate_calibrate_color.currentText()
        )
        flags["frame"] = dlg.dd_calibrate_calibrate_frame.currentText()

    def update_data_from_flags(self, flags):
        """Updates UI elements from the flags.

        Args:
            flags (dict[str, _]): Flags corresponding to the tab default binary.
        """
        dropdowns = [["rig_in", self.dlg.dd_calibrate_rig_rigs_res]]
        common.update_data_from_flags(self, flags, dropdowns=dropdowns)

    def update_flagfile_edit(self, flagfile_fn, switch_to_flag_tab=True):
        """Updates the edit box for the flagfile.

        Args:
            flagfile_fn (str): Name of the flagfile.
            switch_to_flag_tab (bool, optional): Whether or not to switch tabs after updating.
        """
        common.update_flagfile_edit(self, flagfile_fn, switch_to_flag_tab)

    def refresh_data(self):
        """Updates UI elements to be in sync with data on disk."""
        common.refresh_data(self)

    def setup_project(self, mkdirs=False):
        """Retrieves any missing flagfiles and sets the default flags for the tab.

        Args:
            mkdirs (bool, optional): Whether or not to make the directories where the
                outputs are saved by default.
        """
        common.setup_project(self, mkdirs)

    def refresh(self):
        """Resets the UI tab to its start state."""
        self.setup_project()

    def update_data_or_flags(
        self, flagfile_fn, flagfile_from_data, switch_to_flag_tab=False
    ):
        """Updates the flagfile from the UI elements or vice versa.

        Args:
            flagfile_fn (str): Name of the flagfile.
            flagfile_from_data (bool): Whether to load the flagfile from the data (True) or
                vice versa (False).
            switch_to_flag_tab (bool, optional): Whether or not to switch tabs after updating.
        """
        common.update_data_or_flags(
            self, flagfile_fn, flagfile_from_data, switch_to_flag_tab
        )

    def update_flagfile(self, flagfile_fn):
        """Updates the flagfile from UI elements.

        Args:
            flagfile_fn (str): Name of the flagfile.
        """
        common.update_flagfile(self, flagfile_fn)

    def save_flag_file(self):
        """Saves flagfile from the UI to disk."""
        common.save_flag_file(self, self.flagfile_fn)

    def retrieve_missing_flagfiles(self):
        """Copies the missing flagfiles to project for local modification."""
        common.retrieve_missing_flagfiles(self)

    def add_default_flags(self):
        """Retrieves the default flags to the local flagfile."""
        common.add_default_flags(self)

    def run_process(self):
        """Runs the default binary associated with the tab."""
        gb = self.dlg.gb_calibrate_calibrate
        common.run_process(self, gb)

    def cancel_process(self):
        """Stops a running process."""
        common.cancel_process(self)

    def on_changed_flagfile_edit(self):
        """Callback event handler for flagfile edits."""
        caller = sys._getframe().f_back.f_code.co_name
        if caller == "main":
            # Modified by typing on it
            btn = self.dlg.btn_calibrate_flagfile_save
            if not btn.isEnabled():
                btn.setEnabled(True)

    def get_logs(self):
        common.popup_logs_locations(self)

    def setup_flags(self):
        """Sets up the flags according to the corresponding flagfile."""
        common.setup_flagfile_tab(self)
        self.dlg.label_calibrate_flagfile_tooltip.setToolTip(self.tooltip)

    def on_changed_dropdown(self, gb, dd):
        """Callback event handler for changed dropdown.

        Args:
            gb (QtWidgets.QGroupBox): Group box for the tab.
            dd (QtWidgets.QComboBox): Dropdown UI element.
        """
        if not self.is_refreshing_data:
            if "calibrate_color" in dd.objectName():
                dd = self.dlg.dd_calibrate_calibrate_frame
                common.call_force_refreshing(
                    self, common.populate_dropdown, self, gb, dd
                )
        common.on_changed_dropdown(self, gb, dd)

    def on_changed_preview(self):
        """Callback event handler for changed image previews."""
        common.on_changed_preview(self)

    def populate_dropdowns(self, gb):
        """Populates the dropdowns in the tab.

        Args:
            gb (QtWidgets.QGroupBox): Group box for the tab.
        """
        if gb == self.dlg.gb_calibrate_calibrate:
            dds_pri = [self.dlg.dd_calibrate_calibrate_color]
        else:
            dds_pri = []
        common.populate_dropdowns(self, gb, dds_pri)

    def setup_data(self):
        """Sets up callbacks and initial UI element statuses."""
        common.setup_data(self)

    def disable_tab_if_no_data(self):
        """Prevents navigation to the tab if the required data is not present."""
        common.disable_tab_if_no_data(self, self.dlg.btn_calibrate_calibrate_run)
