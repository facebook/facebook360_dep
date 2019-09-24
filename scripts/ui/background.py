#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Background tab for the UI.

Defines the interaction model for the background tab in the UI. This tab cannot
be run in isolation and expects a very particular structure defined in dep.ui.
UI extensions to the tab can be made by modifying the QT structure and adding
corresponding functionality here.

Example:
    To see an instance of Background, refer to the example in dep.py:

        >>> background = Background(self) # self refers to the overall QT UI object
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

from scripts.util.system_util import image_type_paths, merge_lists


class Background:

    """Tab in UI responsible for computing depth estimates on the background frame.

    Attributes:
        dlg (App(QDialog)): Main dialog box for the app initialized by QT.
        log_reader (LogReader(QtCore.QObject)): Object to interact with UI logging system.
        output_dirs (list[str]): List of directories where outputs will be saved.
        parent (App(QDialog)): Object corresponding to the parent UI element.
        path_rig_json (str): Path to the rig JSON.
        tag (str): Semantic name for the tab.
    """

    def __init__(self, parent):
        """Initializes the Background tab.

        Args:
            parent (App(QDialog)): Object corresponding to the parent UI element.
        """
        self.parent = parent
        self.tag = "bg"
        self.dlg = parent.dlg
        common.init(self)

    def add_data_type_validators(self):
        """Adds validators for UI elements."""
        dlg = self.dlg
        elems = [dlg.val_bg_options_res, dlg.dd_bg_bg_frame_bg]
        for elem in elems:
            dep_util.set_integer_validator(elem)

    def setup_farm(self):
        """Sets up local nature of background render."""
        self.is_aws = False

    def initialize_paths(self):
        """Initializes paths for scripts and flags."""
        common.initialize_paths(self)

    def update_noise_detail(self, noise, detail):
        """Updates noise/detail thresholds interaction.

        Args:
            noise (float): Noise threshold.
            detail (float): Detail threshold.
        """
        common.update_noise_detail(self, noise, detail)

    def switch_ui_elements_for_processing(self, state):
        """Switches element interaction when processing.

        Args:
            state (bool): State to which elements should be changed to (i.e.
                True for enabled, False for disabled)
        """
        gb = self.dlg.gb_bg_bg
        common.switch_ui_elements_for_processing(self, gb, state)

    def sync_with_s3(self):
        """Syncs data available locally with the S3 bucket."""
        gb = self.dlg.gb_bg_bg
        subdirs = [
            self.parent.path_flags,
            image_type_paths["background_disp"],
            image_type_paths["background_disp_levels"],
        ]
        common.sync_with_s3(self, gb, subdirs)

    def update_frame_names(self):
        """Updates dropdowns to account for available frames (on S3 or locally)."""
        data_types = ["bg"]
        image_types = ["disparity"]
        verify_data.update_frame_names(
            self.parent, data_types=data_types, image_types=image_types, update_s3=False
        )

        if self.parent.is_aws:
            # Background is only rendered in cache when using S3 but it will be
            # synced to S3. Add it to the S3 frame list
            self.parent.frames_bg_disparity_s3 = merge_lists(
                self.parent.frames_bg_disparity_s3, self.parent.frames_bg_disparity
            )

    def on_process_finished(self, exitCode, exitStatus, p_id):
        """Callback event handler for a process completing.

        Args:
            exitCode (int): Return code from running the process.
            exitStatus (str): Description message of process exit.
            p_id (str): PID of completed process.
        """
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
        self.output_dirs = [self.path_bg_disparity, self.path_bg_disparity_levels]

    def update_thresholds_color_variance(self):
        """Updates the displayed thresholds for color variance."""
        common.update_thresholds_color_variance(self, self.path_bg_color_levels)

    def run_thresholds_after_wait(self, type):
        """Computes the threshold and displays after a delay.

        Args:
            type (str): Type of threshold to run (either "color_variance" or "fg_mask").
        """
        common.run_thresholds_after_wait(self, type)

    def save_flag_file(self):
        """Saves flagfile from the UI to disk."""
        common.save_flag_file(self, self.flagfile_fn)

    def retrieve_missing_flagfiles(self):
        """Copies the missing flagfiles to project for local modification."""
        common.retrieve_missing_flagfiles(self)

    def add_default_flags(self):
        """Retrieves the default flags to the local flagfile."""
        common.add_default_flags(self)

    def update_flagfile_edit(self, flagfile_fn, switch_to_flag_tab=True):
        """Updates the edit box for the flagfile.

        Args:
            flagfile_fn (str): Name of the flagfile.
            switch_to_flag_tab (bool, optional): Whether or not to switch tabs after updating.
        """
        common.update_flagfile_edit(self, flagfile_fn, switch_to_flag_tab)

    def get_files(self, tag):
        """Retrieves file names corresponding to the desired attribute.

        Args:
            tag (str): Semantic name for the attribute.

        Returns:
            list[str]: List of file names.

        Raises:
            Exception: If a tag is requested with no associated files.
        """
        if tag == "frame_bg":
            p = self.parent
            return sorted(merge_lists(p.frames_bg_color, p.frames_bg_color_levels))
        elif tag == "camera":
            return self.parent.cameras
        else:
            raise Exception(f"Invalid tag: {tag}")

    def update_buttons(self, gb):
        """Enables buttons and dropdowns according to whether or not data is present.

        Args:
            gb (QtWidgets.QGroupBox): Group box for the tab.
        """
        common.update_buttons(self, gb)

    def refresh_data(self):
        """Updates UI elements to be in sync with data on disk."""
        common.refresh_data(self)

    def setup_project(self, mkdirs=False, run_thresholds=True):
        """Retrieves any missing flagfiles and sets the default flags for the tab.

        Args:
            mkdirs (bool, optional): Whether or not to make the directories where the
                outputs are saved by default.
            run_thresholds (bool, optional): Whether or not to run thresholds on start.
        """
        common.setup_project(self, mkdirs)
        if run_thresholds:
            self.run_thresholds("color_variance")

    def refresh(self):
        """Resets the UI tab to its start state."""
        self.setup_project(run_thresholds=False)

    def update_thresholds(self, type):
        """Updates flagfile threshold values per UI elements.

        Args:
            type (str): Type of threshold to run (either "color_variance" or "fg_mask").
        """
        gb = self.dlg.gb_bg_bg
        common.update_thresholds(self, gb, type)

    def on_changed_slider(self, slider, value):
        """Callback event handler for changes to a slider UI element.

        Args:
            slider (QtWidgets.QSlider): Slider UI element.
            value (int/float): Value of the slider element.
        """
        common.on_changed_slider(self, slider, value)

    def run_thresholds(self, type):
        """Runs thresholding based on values in the UI and update UI display.

        Args:
            type (str): Type of threshold to run (either "color_variance" or "fg_mask").
        """
        common.run_thresholds(self, type)

    def run_process(self):
        """Runs the default binary associated with the tab."""
        gb = self.dlg.gb_bg_bg
        common.run_process(self, gb)

    def cancel_process(self):
        """Stops a running process."""
        common.cancel_process(self)

    def on_changed_flagfile_edit(self):
        """Callback event handler for flagfile edits."""
        caller = sys._getframe().f_back.f_code.co_name
        if caller == "main":
            # Modified by typing on it
            btn = self.dlg.btn_bg_flagfile_save
            if not btn.isEnabled():
                btn.setEnabled(True)

    def get_logs(self):
        common.popup_logs_locations(self)

    def setup_flags(self):
        """Sets up the flags according to the corresponding flagfile."""
        common.setup_flagfile_tab(self)
        self.dlg.label_bg_flagfile_tooltip.setToolTip(self.tooltip)

    def update_flags_from_data(self, flags):
        """Updates flags from the UI elements.

        Args:
            flags (dict[str, _]): Flags corresponding to the tab default binary.
        """
        dlg = self.dlg
        flags["log_dir"] = self.path_logs
        flags["input_root"] = self.parent.path_project
        flags["output_root"] = self.path_background
        flags["rig"] = getattr(self, "path_rig_json", "")
        flags["resolution"] = dlg.val_bg_options_res.text()
        flags["first"] = dlg.dd_bg_bg_frame_bg.currentText()
        flags["last"] = flags["first"]
        flags["force_recompute"] = dlg.cb_bg_recompute.isChecked()

        # Background is a single frame, will run just locally
        flags["master"] = ""
        flags["workers"] = ""
        flags["username"] = ""
        flags["password"] = ""
        flags["cloud"] = ""

        # Depth estimation
        flags["disparity_type"] = "background_disp"
        flags["partial_coverage"] = dlg.cb_bg_options_partial_360.isChecked()
        flags["var_noise_floor"] = dlg.label_bg_threshs_1_color_variance.text()
        flags["var_high_thresh"] = dlg.label_bg_threshs_2_color_variance.text()

    def update_data_from_flags(self, flags):
        """Updates UI elements from the flags.

        Args:
            flags (dict[str, _]): Flags corresponding to the tab default binary.
        """
        dlg = self.dlg
        dropdowns = [["first", dlg.dd_bg_bg_frame_bg]]
        values = [["resolution", dlg.val_bg_options_res]]
        checkboxes = [
            ["partial_coverage", dlg.cb_bg_options_partial_360],
            ["force_recompute", dlg.cb_bg_recompute],
        ]
        labels = [
            ["var_noise_floor", dlg.label_bg_threshs_1_color_variance],
            ["var_high_thresh", dlg.label_bg_threshs_2_color_variance],
        ]
        common.update_data_from_flags(
            self,
            flags,
            dropdowns=dropdowns,
            values=values,
            checkboxes=checkboxes,
            labels=labels,
        )

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

    def on_changed_dropdown(self, gb, dd):
        """Callback event handler for changed dropdown.

        Args:
            gb (QtWidgets.QGroupBox): Group box for the tab.
            dd (QtWidgets.QComboBox): Dropdown UI element.
        """
        common.on_changed_dropdown(self, gb, dd)

    def on_changed_line_edit(self, gb, le):
        """Callback event handler for changed line edit.

        Args:
            gb (QtWidgets.QGroupBox): Group box for the tab.
            le (QtWidgets.QLineEdit): Line edit UI element.
        """
        common.on_changed_line_edit(self, gb, le)

    def on_state_changed_partial_360(self):
        """Callback event handler for changed "partial coverage" checkbox."""
        common.on_state_changed_partial_360(self)

    def on_state_changed_recompute(self):
        """Callback event handler for changed "recompute" checkbox."""
        common.on_state_changed_recompute(self)

    def on_changed_preview(self):
        """Callback event handler for changed image previews."""
        common.on_changed_preview(self)

    def populate_dropdowns(self, gb):
        """Populates the dropdowns in the tab.

        Args:
            gb (QtWidgets.QGroupBox): Group box for the tab.
        """
        common.populate_dropdowns(self, gb)

    def setup_data(self):
        """Sets up callbacks and initial UI element statuses."""
        callbacks = {}
        dlg = self.dlg
        dlg.cb_bg_options_partial_360.setChecked(False)
        dlg.cb_bg_recompute.setChecked(False)
        callbacks = {
            dlg.cb_bg_options_partial_360: self.on_state_changed_partial_360,
            dlg.cb_bg_recompute: self.on_state_changed_recompute,
        }
        common.setup_data(self, callbacks)

    def setup_thresholds(self):
        """Sets necessary thresholds apps for the tab."""
        common.setup_thresholds(self, ["color_variance"])

    def setup_thresholds_color_variance(self):
        """Sets color variance thresholds apps for the tab."""
        common.setup_thresholds_color_variance(self)

    def disable_tab_if_no_data(self):
        """Prevents navigation to the tab if the required data is not present."""
        common.disable_tab_if_no_data(self, self.dlg.btn_bg_bg_run)
