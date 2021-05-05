#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Depth estimation tab for the UI.

Defines the interaction model for the background tab in the UI. This tab cannot
be run in isolation and expects a very particular structure defined in dep.ui.
UI extensions to the tab can be made by modifying the QT structure and adding
corresponding functionality here.

Example:
    To see an instance of depth estimation, refer to the example in dep.py:

        >>> depth_estimation = DepthEstimation(self) # self refers to the overall QT UI object
"""

import os
import sys

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "aws"))
sys.path.append(os.path.join(dir_scripts, "render"))
sys.path.append(os.path.join(dir_scripts, "util"))

import common
import dep_util
import scripts.render.config as config
import verify_data
from scripts.util.system_util import image_type_paths, merge_lists


class DepthEstimation:

    """Tab in UI responsible for depth estimates on video frames.

    Attributes:
        aws_staging_ip (str): IP of the staging machine (used for starting the Kubernetes cluster).
        dlg (App(QDialog)): Main dialog box for the app initialized by QT.
        is_farm (bool): Whether or not this is a farm render.
        is_refreshing_data (bool): Whether or not the data is currently being updated.
        log_reader (LogReader(QtCore.QObject)): Object to interact with UI logging system.
        output_dirs (list[str]): List of directories where outputs will be saved.
        parent (App(QDialog)): Object corresponding to the parent UI element.
        path_rig_json (str): Path to the rig JSON.
        tag (str): Semantic name for the tab.
    """

    def __init__(self, parent):
        """Initializes the DepthEstimation tab.

        Args:
            parent (App(QDialog)): Object corresponding to the parent UI element.
        """
        self.parent = parent
        self.tag = "depth"
        self.dlg = parent.dlg
        common.init(self)

    def add_data_type_validators(self):
        """Adds validators for UI elements."""
        dlg = self.dlg
        elems = [
            dlg.val_depth_options_res,
            dlg.dd_depth_video_frame_bg,
            dlg.dd_depth_video_first,
            dlg.dd_depth_video_last,
        ]
        for elem in elems:
            dep_util.set_integer_validator(elem)

    def setup_farm(self):
        """Sets up a Kubernetes farm for AWS renders."""
        common.setup_farm(self)

    def initialize_paths(self):
        """Initializes paths for scripts and flags."""
        self.aws_staging_ip = ""
        common.initialize_paths(self)

    def update_noise_detail(self, noise, detail):
        """Updates noise/detail thresholds interaction.

        Args:
            noise (float): Noise threshold.
            detail (float): Detail threshold.
        """
        common.update_noise_detail(self, noise, detail)

    def update_fg_masks_thresholds(self, blur, closing, thresh):
        """Updates thresholds and display for the foreground masking.

        Args:
            blur (int, optional): Gaussian blur radius.
            closing (int, optional): Closure (for sealing holes).
            thresh (int, optional): Threshold applied to segment foreground and background
        """
        common.update_fg_masks_thresholds(self, blur, closing, thresh)

    def switch_ui_elements_for_processing(self, state):
        """Switches element interaction when processing.

        Args:
            state (bool): State to which elements should be changed to (i.e.
                True for enabled, False for disabled)
        """
        gb = self.dlg.gb_depth_video
        common.switch_ui_elements_for_processing(self, gb, state)

    def update_frame_names(self):
        """Updates dropdowns to account for available frames (on S3 or locally)."""
        update_s3 = self.is_aws and self.is_farm
        update_local = not update_s3
        image_types = ["disparity"]
        verify_data.update_frame_names(
            self.parent,
            image_types=image_types,
            update_s3=update_s3,
            update_local=update_local,
        )

        if self.is_aws:
            # Frame was rendered in cache but it will be synced to S3.
            # Add it to the S3 frame list
            self.parent.frames_video_disparity_s3 = merge_lists(
                self.parent.frames_video_disparity_s3,
                self.parent.frames_video_disparity,
            )

    def sync_with_s3(self):
        """Syncs data available locally with the S3 bucket."""
        if not self.is_farm:  # rendered locally
            gb = self.dlg.gb_depth_video
            subdirs = [
                self.parent.path_flags,
                image_type_paths["video_disp"],
                image_type_paths["video_disp_levels"],
            ]
            common.sync_with_s3(self, gb, subdirs)

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
        self.output_dirs = [self.path_video_disparity, self.path_video_disparity_levels]

    def update_thresholds_color_variance(self):
        """Updates the displayed thresholds for color variance."""
        common.update_thresholds_color_variance(
            self, self.path_video_color_levels, labels=("_first")
        )

    def update_thresholds_fg_mask(self):
        """Updates thresholds and display for the foreground masking using values from UI."""
        paths_color = [self.path_bg_color_levels, self.path_video_color_levels]
        common.update_thresholds_fg_mask(self, paths_color)

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

    def get_color_frames(self, color_type, suffix):
        frames = getattr(self.parent, f"frames_{color_type}_color{suffix}", None)
        frames_levels = getattr(
            self.parent, f"frames_{color_type}_color_levels{suffix}", None
        )
        return sorted(merge_lists(frames, frames_levels))

    def get_files(self, tag):
        """Retrieves file names corresponding to the desired attribute.

        Args:
            tag (str): Semantic name for the attribute.

        Returns:
            list[str]: List of file names.

        Raises:
            Exception: If a tag is requested with no associated files.
        """
        s = "_s3" if self.is_aws and self.is_farm else ""
        if tag in ["first", "last"]:
            return self.get_color_frames("video", s)
        elif tag == "frame_bg":
            return self.get_color_frames("bg", s)
        elif tag == "camera":
            return self.parent.cameras
        elif tag == "workers":
            return common.get_workers(self)
        elif tag == "ec2":
            return self.ec2_instance_types_cpu if self.parent.is_aws else []
        else:
            raise Exception(f"Invalid tag: {tag}")

    def update_buttons(self, gb):
        """Enables buttons and dropdowns according to whether or not data is present.

        Args:
            gb (QtWidgets.QGroupBox): Group box for the tab.
        """
        dlg = self.dlg
        ignore = [dlg.dd_depth_farm_workers, dlg.dd_depth_farm_ec2]
        if not dlg.cb_depth_options_use_bg.isChecked():
            ignore.append(dlg.dd_depth_video_frame_bg)
        if self.is_farm and self.parent.is_aws:
            dlg.btn_depth_farm_terminate_cluster.setEnabled(
                bool(len(common.get_aws_workers()))
            )
        common.update_buttons(self, gb, ignore)

        use_bg = dlg.cb_depth_options_use_bg.isChecked()
        dlg.dd_depth_video_frame_bg.setEnabled(use_bg)

    def update_bg_checkbox(self):
        """Enables background selection if frames are present."""
        # We cannot use background if we don't have background data
        has_bg = dep_util.check_image_existence(self.path_bg_disparity) != ""
        cb_use_bg = self.dlg.cb_depth_options_use_bg
        if not has_bg:
            cb_use_bg.setChecked(False)
            cb_use_bg.setEnabled(False)
            common.call_force_refreshing(
                self,
                dep_util.update_flagfile,
                self.flagfile_fn,
                "use_foreground_masks",
                False,
            )
        else:
            cb_use_bg.setEnabled(True)

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
        common.setup_aws_config(self)

    def refresh(self):
        """Resets the UI tab to its start state."""
        self.setup_project(run_thresholds=False)

    def update_thresholds(self, type):
        """Updates flagfile threshold values per UI elements.

        Args:
            type (str): Type of threshold to run (either "color_variance" or "fg_mask").
        """
        gb = self.dlg.gb_depth_video
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

    def on_terminate_cluster(self):
        """Terminates a running cluster."""
        gb = self.dlg.gb_depth_video
        common.on_terminate_cluster(self, gb)

    def run_process(self):
        """Runs the default binary associated with the tab."""
        gb = self.dlg.gb_depth_video
        if self.is_farm and self.parent.is_aws:
            common.run_process_aws(self, gb)
        else:
            # Render
            common.run_process(self, gb)

    def cancel_process(self):
        """Stops a running process."""
        common.cancel_process(self)

    def reset_run_button_text(self):
        self.dlg.btn_depth_video_run.setText("Run")

    def update_run_button_text(self):
        """Updates the text of the Run button depending on the existance of a process
        running on the cloud
        """
        common.update_run_button_text(self, self.dlg.btn_depth_video_run)

    def on_changed_flagfile_edit(self):
        """Callback event handler for flagfile edits."""
        caller = sys._getframe().f_back.f_code.co_name
        if caller == "main":
            # Modified by typing on it
            btn = self.dlg.btn_depth_flagfile_save
            if not btn.isEnabled():
                btn.setEnabled(True)

    def on_ec2_dashboard(self):
        common.popup_ec2_dashboard_url(self)

    def get_logs(self):
        common.popup_logs_locations(self)

    def setup_flags(self):
        """Sets up the flags according to the corresponding flagfile."""
        common.setup_flagfile_tab(self)
        self.dlg.label_depth_flagfile_tooltip.setToolTip(self.tooltip)

    def replace_with_host_path(self, flags, fields):
        """Replaces all references to local paths with their default Docker mounted locations.
        This is only relevant for SMB (local farm) renders.

        Args:
            flags (dict[str, str]): Data where fields are to be replaced.
            fields (list[str]): Names of flags to be updated.
        """
        for field in fields:
            flags[field] = flags[field].replace(
                config.DOCKER_INPUT_ROOT, self.parent.path_project
            )

    def update_flags_from_data(self, flags):
        """Updates flags from the UI elements.

        Args:
            flags (dict[str, _]): Flags corresponding to the tab default binary.
        """
        dlg = self.dlg

        # General
        flags["log_dir"] = self.path_logs
        flags["input_root"] = self.parent.path_project
        flags["output_root"] = self.path_video
        flags["rig"] = getattr(self, "path_rig_json", "")
        flags["force_recompute"] = dlg.cb_depth_recompute.isChecked()

        if self.is_farm and self.parent.is_aws:
            flags["master"] = self.aws_staging_ip
            flags["workers"] = ""
            flags["cloud"] = "aws"

            rig_fn = os.path.basename(flags["rig"])
            flags["input_root"] = self.parent.ui_flags.project_root
            flags["output_root"] = os.path.join(
                self.parent.ui_flags.project_root, config.OUTPUT_ROOT_NAME
            )
            flags["rig"] = os.path.join(
                self.parent.ui_flags.project_root, "rigs", rig_fn
            )
            flags["log_dir"] = os.path.join(flags["output_root"], "logs")

        elif self.is_farm and self.parent.is_lan:
            flags["master"] = self.parent.ui_flags.master
            flags["workers"] = ",".join(dlg.dd_depth_farm_workers.checkedItems())
            flags["cloud"] = ""

            flags["username"] = self.parent.ui_flags.username
            flags["password"] = self.parent.ui_flags.password

        else:  # local
            flags["master"] = ""
            flags["workers"] = ""
            flags["cloud"] = ""

        flags["resolution"] = dlg.val_depth_options_res.text()

        flags["first"] = dlg.dd_depth_video_first.currentText()
        flags["last"] = dlg.dd_depth_video_last.currentText()

        # Foreground masks
        use_fg_masks = dlg.cb_depth_options_use_bg.isChecked()
        flags["run_generate_foreground_masks"] = dlg.cb_depth_options_use_bg.isChecked()

        # Depth estimation
        flags["partial_coverage"] = dlg.cb_depth_options_partial_360.isChecked()
        flags["use_foreground_masks"] = use_fg_masks
        flags["var_noise_floor"] = dlg.label_depth_threshs_1_color_variance.text()
        flags["var_high_thresh"] = dlg.label_depth_threshs_2_color_variance.text()
        flags["blur_radius"] = dlg.label_depth_threshs_1_fg_mask.text()
        flags["morph_closing_size"] = dlg.label_depth_threshs_2_fg_mask.text()
        flags["threshold"] = dlg.label_depth_threshs_3_fg_mask.text()
        flags["background_frame"] = (
            dlg.dd_depth_video_frame_bg.currentText() if use_fg_masks else ""
        )

        # Paths that are running w/ workers have to provide an address visible by them
        if self.parent.is_lan:
            self.replace_with_host_path(flags, {"input_root", "output_root", "rig"})

    def update_data_from_flags(self, flags):
        """Updates UI elements from the flags.

        Args:
            flags (dict[str, _]): Flags corresponding to the tab default binary.
        """
        dlg = self.dlg
        dropdowns = [
            ["first", dlg.dd_depth_video_first],
            ["last", dlg.dd_depth_video_last],
            ["background_frame", dlg.dd_depth_video_frame_bg],
        ]
        values = [["resolution", dlg.val_depth_options_res]]
        labels = [
            ["var_noise_floor", dlg.label_depth_threshs_1_color_variance],
            ["var_high_thresh", dlg.label_depth_threshs_2_color_variance],
            ["blur_radius", dlg.label_depth_threshs_1_fg_mask],
            ["morph_closing_size", dlg.label_depth_threshs_2_fg_mask],
            ["threshold", dlg.label_depth_threshs_3_fg_mask],
        ]
        checkboxes = [
            ["partial_coverage", dlg.cb_depth_options_partial_360],
            ["use_foreground_masks", dlg.cb_depth_options_use_bg],
            ["force_recompute", dlg.cb_depth_recompute],
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

    def on_state_changed_partial_360(self, state):
        """Callback event handler for changed "partial coverage" checkbox.

        Args:
            state (str): Identifier of the callback state.
        """
        common.on_state_changed_partial_360(self)

    def on_state_changed_recompute(self):
        """Callback event handler for changed "recompute" checkbox."""
        common.on_state_changed_recompute(self)

    def on_state_changed_use_bg(self, state):
        """Callback event handler for changed "use background" checkbox.

        Args:
            state (str): Identifier of the callback state.
        """
        gb = self.dlg.gb_depth_video
        common.on_state_changed_use_bg(self, gb)

    def on_changed_preview(self):
        """Callback event handler for changed image previews."""
        common.on_changed_preview(self)

    def update_frame_range_dropdowns(self):
        """Updates ranges displayed in dropdowns per available files on disk."""
        dlg = self.dlg
        gb = dlg.gb_depth_video
        dds = [dlg.dd_depth_video_first, dlg.dd_depth_video_last]
        for dd in dds:
            common.populate_dropdown(self, gb, dd)

    def on_state_changed_farm(self, state):
        """Callback event handler for changed "AWS" checkbox.

        Args:
            state (str): Identifier of the callback state.
        """
        common.on_state_changed_farm(self, state)

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
        dlg.cb_depth_options_partial_360.setChecked(False)
        dlg.cb_depth_recompute.setChecked(False)
        dlg.cb_depth_options_use_bg.setChecked(True)
        dlg.label_depth_farm_workers.setEnabled(False)
        dlg.dd_depth_farm_workers.setEnabled(False)
        dlg.gb_depth_farm.setEnabled(True)
        callbacks = {
            dlg.cb_depth_options_partial_360: self.on_state_changed_partial_360,
            dlg.cb_depth_recompute: self.on_state_changed_recompute,
            dlg.cb_depth_options_use_bg: self.on_state_changed_use_bg,
            dlg.btn_depth_farm_terminate_cluster: self.on_terminate_cluster,
            dlg.btn_depth_farm_ec2_dashboard: self.on_ec2_dashboard,
            dlg.gb_depth_farm: self.on_state_changed_farm,
        }
        common.setup_data(self, callbacks)

    def setup_thresholds(self):
        """Sets necessary thresholds apps for the tab."""
        common.setup_thresholds(self, ["color_variance", "fg_mask"])

    def setup_thresholds_color_variance(self):
        """Sets color variance thresholds apps for the tab."""
        common.setup_thresholds_color_variance(self)

    def setup_thresholds_fg_masks(self):
        """Sets up the default thresholds on foreground masks."""
        common.setup_thresholds_fg_masks(self)

    def disable_tab_if_no_data(self):
        """Prevents navigation to the tab if the required data is not present."""
        common.disable_tab_if_no_data(self, self.dlg.btn_depth_video_run)
