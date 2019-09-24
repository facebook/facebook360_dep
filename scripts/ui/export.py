#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Export tab for the UI.

Defines the interaction model for the background tab in the UI. This tab cannot
be run in isolation and expects a very particular structure defined in dep.ui.
UI extensions to the tab can be made by modifying the QT structure and adding
corresponding functionality here.

Example:
    To see an instance of Export, refer to the example in dep.py:

        >>> export = Export(self) # self refers to the overall QT UI object
"""

import json
import os
import sys
from collections import OrderedDict

import pyvidia

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "render"))

import common
import dep_util
import scripts.render.config as config
import verify_data

from scripts.render.network import download
from scripts.util.system_util import (
    get_flags_from_flagfile,
    image_type_paths,
    merge_lists,
)


class Export:

    """Tab in UI responsible for producing meshed binaries and other final output formats
    (e.g. color and disparity equirects) and viewing them in corresponding PC and
    headset displays.

    Attributes:
        dlg (App(QDialog)): Main dialog box for the app initialized by QT.
        formats (dict[str, str]): Formats that can be rendered given the render configuration.
        is_farm (bool): Whether or not this is a farm render.
        is_host_linux_gpu (bool): Whether or not this is a Linux GPU machine or not.
        is_local_non_win (bool): Whether this is a Windows machine or not.
        is_viewer_rift_hidden (bool): Whether or not to display the "RiftViewer" button.
        is_viewer_smr_hidden (bool): Whether or not to display the "SimpleMeshRenderer" button.
        log_reader (LogReader(QtCore.QObject)): Object to interact with UI logging system.
        parent (App(QDialog)): Object corresponding to the parent UI element.
        path_rig_json (str): Path to the rig JSON.
        tag (str): Semantic name for the tab.
    """

    def __init__(self, parent):
        """Initializes the Export tab.

        Args:
            parent (App(QDialog)): Object corresponding to the parent UI element.
        """
        self.parent = parent
        self.tag = "export"
        self.dlg = parent.dlg
        common.init(self)
        self.initialize_viewer_buttons()

    def setup_aws_config(self):
        """Configures the AWS credentials."""
        common.setup_aws_config(self)

    def add_data_type_validators(self):
        """Adds validators for UI elements."""
        dlg = self.dlg
        elems = [
            dlg.val_export_options_res,
            dlg.dd_export_data_first,
            dlg.dd_export_data_last,
        ]
        for elem in elems:
            dep_util.set_integer_validator(elem)

    def initialize_viewer_buttons(self):
        """Sets up buttons to IPC callbacks on the host."""
        btn_smr_onscreen = self.dlg.btn_export_data_smr_view
        btn_riftviewer = self.dlg.btn_export_data_rift_view
        host_os = self.parent.ui_flags.host_os

        self.is_viewer_rift_hidden = False
        self.is_viewer_smr_hidden = False

        # Viewers only available if we have a local_bin path
        if self.parent.ui_flags.local_bin == "":
            btn_smr_onscreen.setEnabled(False)
            btn_riftviewer.setEnabled(False)
            self.is_viewer_rift_hidden = True
            self.is_viewer_smr_hidden = True

        # RiftViewer only available in a Windows host
        if host_os != "OSType.WINDOWS":
            btn_riftviewer.setEnabled(False)
            self.is_viewer_rift_hidden = True
        # SimpleMeshRenderer onscreen viewer only available in a non-Windows host
        else:
            btn_smr_onscreen.setEnabled(False)
            self.is_viewer_smr_hidden = True

    def setup_farm(self):
        """Sets up a Kubernetes farm for AWS renders."""
        common.setup_farm(self)

    def initialize_paths(self):
        """Initializes paths for scripts and flags."""
        common.initialize_paths(self)
        self.formats = {
            "6DoF (Meshing)": "6dof_meshing",
            "6DoF (Striping)": "6dof_striping",
        }

        # SimpleMeshRenderer only available to render when:
        # - Cloud rendering (AWS)
        # - Linux host with NVIDIA GPU
        # - non-Windows host + given local_bin flag
        host_os = self.parent.ui_flags.host_os
        is_linux = host_os == "OSType.LINUX"
        is_non_windows = host_os != "OSType.WINDOWS"
        has_local_bin = self.parent.ui_flags.local_bin != ""
        self.is_host_linux_gpu = is_linux and (pyvidia.get_nvidia_device() is not None)
        self.is_local_non_win = is_non_windows and has_local_bin
        if self.parent.is_aws or self.is_host_linux_gpu or self.is_local_non_win:
            self.formats.update(
                {
                    "Equirect color": "eqrcolor",
                    "Equirect disparity": "eqrdisp",
                    "Cubemap color": "cubecolor",
                    "Cubemap disparity": "cubedisp",
                    "180 stereo left-right": "lr180",
                    "360 stereo top-bottom": "tbstereo",
                    "3DoF top-bottom": "tb3dof",
                }
            )

    def switch_ui_elements_for_processing(self, state):
        """Switches element interaction when processing.

        Args:
            state (bool): State to which elements should be changed to (i.e.
                True for enabled, False for disabled)
        """
        gb = self.dlg.gb_export_data
        common.switch_ui_elements_for_processing(self, gb, state)

    def sync_with_s3(self):
        """Syncs data available locally with the S3 bucket."""
        if not self.is_farm:  # rendered locally
            gb = self.dlg.gb_export_data
            subdirs = [
                self.parent.path_flags,
                image_type_paths["video_bin"],
                image_type_paths["video_fused"],
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

    def get_ec2_instance_types(self):
        """Gets valid instances that can be spawned for a job.

        Returns:
            list[str]: Names of valid instance types.
        """
        if self.get_format().startswith("6dof"):
            return self.ec2_instance_types_cpu
        else:
            return self.ec2_instance_types_gpu

    def get_valid_types(self):
        """Checks which render types are valid.

        Returns:
            list[str]: Subset of ("background", "video") depending on if the corresponding
                inputs are present.
        """
        # We need both full-size color and disparity for a type to be valid
        type_paths = OrderedDict(
            {
                "color": [self.path_video_color, self.path_video_disparity],
                "background_color": [self.path_bg_color, self.path_bg_disparity],
            }
        )
        ps = []
        for type, paths in type_paths.items():
            if all(dep_util.check_image_existence(p) != "" for p in paths):
                ps.append(type)
        return ps

    def get_frame_names(self):
        """Finds all the frames in a local directory.

        Returns:
            list[str]: Sorted list of frame names in the directory.
        """
        s = "_s3" if self.is_aws and self.is_farm else ""
        frames_bin = getattr(self.parent, f"frames_bin{s}", None)
        data_type = self.dlg.dd_export_data_type.currentText()
        if not data_type:
            return frames_bin
        t = "bg" if data_type == "background_color" else "video"
        frames_color = getattr(self.parent, f"frames_{t}_color{s}", None)
        frames_disp = getattr(self.parent, f"frames_{t}_disparity{s}", None)

        return sorted(merge_lists(frames_color, frames_disp, frames_bin))

    def get_files(self, tag):
        """Retrieves file names corresponding to the desired attribute.

        Args:
            tag (str): Semantic name for the attribute.

        Returns:
            list[str]: List of file names.

        Raises:
            Exception: If a tag is requested with no associated files.
        """
        if tag in ["first", "last"]:
            return self.get_frame_names()
        elif tag == "type":
            return self.get_valid_types()
        elif tag == "format":
            return list(self.formats)
        elif tag == "file_type":
            return ["png", "jpeg", "tif", "exr"]
        elif tag == "workers":
            return common.get_workers(self)
        elif tag == "ec2":
            return self.get_ec2_instance_types() if self.parent.is_aws else []
        else:
            raise Exception(f"Invalid tag: {tag}")

    def update_buttons(self, gb):
        """Enables buttons and dropdowns according to whether or not data is present.

        Args:
            gb (QtWidgets.QGroupBox): Group box for the tab.
        """
        dlg = self.dlg
        ignore = [dlg.dd_export_farm_workers, dlg.dd_export_farm_ec2]

        if self.is_viewer_rift_hidden:
            ignore.append(dlg.btn_export_data_rift_view)
        if self.is_viewer_smr_hidden:
            ignore.append(dlg.btn_export_data_smr_view)
        if self.parent.is_aws:
            dlg.btn_export_farm_terminate_cluster.setEnabled(
                bool(len(common.get_aws_workers()))
            )
        common.update_buttons(self, gb, ignore)

        if not self.is_viewer_rift_hidden:
            has_fused = dep_util.check_image_existence(self.path_fused) != ""
            dlg.btn_export_data_rift_view.setEnabled(has_fused)

    def get_format(self, flags_from_data=True):
        """Gets format to populate the flagfile.

        Args:
            flags_from_data (dict[str, _]): Flags and their values, typically
                pulled from the UI.

        Returns:
            str: Format in the UI that should populate the flagfile.
        """
        format_text = self.dlg.dd_export_data_format.currentText()
        formats = self.formats
        if format_text:
            return formats[format_text] if flags_from_data else format_text
        else:
            vals = formats.values() if flags_from_data else formats
            return next(iter(vals))

    def get_color_scale(self, dst_width, color_type):
        if color_type == "background_color":
            src_width = self.parent.bg_full_size_width
        else:
            src_width = self.parent.video_full_size_width
        if not src_width or not dst_width:
            return 1.0
        return float(dst_width) / float(src_width)

    def update_flags_from_data(self, flags):
        """Updates flags from the UI elements.

        Args:
            flags (dict[str, _]): Flags corresponding to the tab default binary.
        """
        dlg = self.dlg
        rig_fn = getattr(self, "path_rig_json", "")
        flags["rig"] = rig_fn
        flags["input_root"] = self.parent.path_project
        flags["output_root"] = self.path_video
        flags["log_dir"] = self.path_logs
        flags["width"] = dlg.val_export_options_res.text()
        flags["first"] = dlg.dd_export_data_first.currentText()
        flags["last"] = dlg.dd_export_data_last.currentText()
        flags["force_recompute"] = dlg.cb_export_recompute.isChecked()

        color_type = dlg.dd_export_data_type.currentText()
        if color_type == "background_color":
            flags["color"] = self.path_bg_color
            flags["disparity"] = self.path_bg_disparity
            flags["color_type"] = "background_color"
            flags["disparity_type"] = "background_disp"
        else:
            flags["color"] = self.path_video_color
            flags["disparity"] = self.path_video_disparity
            flags["color_type"] = "color"
            flags["disparity_type"] = "disparity"

        if self.is_farm and self.parent.is_aws:
            flags["master"] = ""
            flags["workers"] = ""
            flags["cloud"] = "aws"

            rig_bn = os.path.basename(flags["rig"])
            flags["input_root"] = self.parent.ui_flags.project_root
            flags["output_root"] = os.path.join(
                self.parent.ui_flags.project_root, config.OUTPUT_ROOT_NAME
            )
            flags["rig"] = os.path.join(
                self.parent.ui_flags.project_root, "rigs", rig_bn
            )
            flags["log_dir"] = os.path.join(flags["output_root"], "logs")

        elif self.is_farm and self.parent.is_lan:
            flags["master"] = self.parent.ui_flags.master
            flags["workers"] = ",".join(dlg.dd_export_farm_workers.checkedItems())
            flags["cloud"] = ""

            flags["username"] = self.parent.ui_flags.username
            flags["password"] = self.parent.ui_flags.password

        else:  # local
            flags["master"] = ""
            flags["workers"] = ""
            flags["cloud"] = ""

        flags["file_type"] = dlg.dd_export_data_file_type.currentText()
        flags["format"] = self.get_format()
        is6dof = flags["format"].startswith("6dof")
        if is6dof:
            flags["format"] = "6dof"
        flags["run_convert_to_binary"] = (
            is6dof and "Meshing" in dlg.dd_export_data_format.currentText()
        )
        flags["run_fusion"] = (
            is6dof and "Striping" in dlg.dd_export_data_format.currentText()
        )
        flags["run_simple_mesh_renderer"] = not is6dof
        flags["color_scale"] = self.get_color_scale(flags["width"], color_type)

        if self.parent.is_aws:
            create_flagfile = os.path.join(
                self.path_flags, self.app_name_to_flagfile[self.app_aws_create]
            )
            if os.path.exists(create_flagfile):
                create_flags = get_flags_from_flagfile(create_flagfile)
                if "cluster_size" in create_flags:
                    dlg.spin_export_farm_num_workers.setValue(
                        int(create_flags["cluster_size"])
                    )
                if "instance_type" in create_flags:
                    dlg.dd_export_farm_ec2.setCurrentText(create_flags["instance_type"])

    def get_format_from_value(self, value):
        """Gets format to populate the UI from the flagfile. This is needed since the
        format displayed on the UI and in the internal pipeline differ (for readability).

        Args:
            value (str): Value from the flagfile (i.e. eqrcolor).

        Returns:
            str: Format to be displayed in the UI (i.e. Equirect Color).
        """
        for k, v in self.formats.items():
            if v == value:
                return k
        return ""

    def update_data_from_flags(self, flags):
        """Updates UI elements from the flags.

        Args:
            flags (dict[str, _]): Flags corresponding to the tab default binary.
        """
        dlg = self.dlg
        dropdowns = [
            ["color_type", dlg.dd_export_data_type],
            ["first", dlg.dd_export_data_first],
            ["last", dlg.dd_export_data_last],
            ["file_type", dlg.dd_export_data_file_type],
        ]
        values = [["width", dlg.val_export_options_res]]
        checkboxes = [["force_recompute", dlg.cb_export_recompute]]
        common.update_data_from_flags(
            self, flags, dropdowns=dropdowns, values=values, checkboxes=checkboxes
        )

        # Special case: format
        if flags["format"] in self.formats.values():
            val = self.get_format_from_value(flags["format"])
            dep_util.update_qt_dropdown(dlg.dd_export_data_format, val)

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
        common.setup_aws_config(self)

    def refresh(self):
        """Resets the UI tab to its start state."""
        self.setup_project()

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

    def _ipc_call(self, ipc_fn):
        """Makes an IPC to the host. This is currently done through a Watchdog system
        running on the host tied to specific files and us touching the corresponding
        files from within the container.

        Args:
            ipc_fn (str): Path to the file being monitored on the host.
        """
        if self.is_farm and self.parent.is_aws:
            local_fused_dir = os.path.join(
                config.DOCKER_OUTPUT_ROOT, image_type_paths["fused"]
            )
            if (
                not os.path.exists(local_fused_dir)
                or len(os.listdir(local_fused_dir)) == 0
            ):
                remote_fused_dir = os.path.join(
                    self.parent.ui_flags.project_root,
                    config.OUTPUT_ROOT_NAME,
                    image_type_paths["fused"],
                )
                download(src=remote_fused_dir, dst=local_fused_dir, filters=["*"])

        docker_ipc_path = os.path.join(config.DOCKER_IPC_ROOT, ipc_fn)
        with open(docker_ipc_path, "w"):
            os.utime(docker_ipc_path, None)

    def activate_ipc(self, ipc_name):
        """Runs an application. This only works if the app is set up with a corresponding
        IPC file that is monitored by the Watchdog (defined in config.py).

        Args:
            ipc_name (str): Name of the binary (must be a case in config.get_app_name).
                Otherwise this is a no-op.
        """
        app_name = config.get_app_name(ipc_name)
        self.log_reader.log_notice(
            f"Executing {app_name} in host. Check terminal for logs."
        )
        self._ipc_call(ipc_name)

    def is_local_render(self):
        """Whether or not the current render is configured to be performed locally.

        Returns:
            bool: If the render will be performed locally.
        """
        return (
            (not self.parent.is_aws or not self.is_farm)
            and not self.is_host_linux_gpu
            and self.is_local_non_win
            and self.get_format() != "6dof"
        )

    def run_process(self):
        """Runs the default binary associated with the tab."""
        gb = self.dlg.gb_export_data
        p_id = f"run_{self.tag}_{self.get_format()}"
        if self.is_farm and self.parent.is_aws:
            common.run_process_aws(self, gb, p_id=p_id)
        elif self.is_local_render():
            # Render non-6DoF through host call
            self.activate_ipc(config.DOCKER_SMR_IPC)
        else:
            # Render
            common.run_process(self, gb, p_id=p_id)

    def cancel_process(self):
        """Stops a running process."""
        common.cancel_process(self)

    def reset_run_button_text(self):
        self.dlg.btn_export_data_run.setText("Run")

    def update_run_button_text(self):
        """Updates the text of the Run button depending on the existance of a process
        running on the cloud
        """
        common.update_run_button_text(self, self.dlg.btn_export_data_run)

    def on_changed_flagfile_edit(self):
        """Callback event handler for flagfile edits."""
        caller = sys._getframe().f_back.f_code.co_name
        if caller == "main":
            # Modified by typing on it
            btn = self.dlg.btn_export_flagfile_save
            if not btn.isEnabled():
                btn.setEnabled(True)

    def on_ec2_dashboard(self):
        """Callback event handler for clicking the "EC2 Dashboard" button."""
        common.popup_ec2_dashboard_url(self)

    def on_download_meshes(self):
        """Callback event handler for clicking the "Download Meshes" button."""
        gb = self.dlg.gb_export_data
        common.on_download_meshes(self, gb)

    def get_logs(self):
        """Callback event handler for clicking the "Logs" button."""
        common.popup_logs_locations(self)

    def setup_flags(self):
        """Sets up the flags according to the corresponding flagfile."""
        common.setup_flagfile_tab(self)
        self.dlg.label_export_flagfile_tooltip.setToolTip(self.tooltip)

    def update_viewer_buttons(self):
        """Only enable viewer buttons based on what is possible from the host, i.e.
        RiftViewer will be disabled on any non-Windows computer.
        """
        btn_smr_onscreen = self.dlg.btn_export_data_smr_view
        btn_riftviewer = self.dlg.btn_export_data_rift_view
        if self.get_format() != "6dof":
            btn_smr_onscreen.setEnabled(False)
            btn_riftviewer.setEnabled(False)
        else:
            if not self.is_viewer_smr_hidden:
                btn_smr_onscreen.setEnabled(True)
            if not self.is_viewer_rift_hidden:
                btn_riftviewer.setEnabled(True)

    def update_ec2_dropdown(self):
        """Displays only valid values in the dropdown for EC2 instance types."""
        gb = self.dlg.gb_export_farm
        dd = self.dlg.dd_export_farm_ec2
        common.call_force_refreshing(self, common.populate_dropdown, self, gb, dd)

    def update_file_type(self):
        """Enables/disables file type depending on the format"""
        is6dof = self.get_format().startswith("6dof")
        self.dlg.dd_export_data_file_type.setEnabled(not is6dof)

    def update_label_resolution(self):
        format = self.get_format()
        if format.startswith("6dof"):
            label = "per camera, max recommended 3072"
        elif format.startswith("cube"):
            label = "per cubeface"
        else:
            label = "equirect"
        label = f"Output resolution ({label})"
        self.dlg.label_export_options_res.setText(label)

    def on_state_changed_recompute(self):
        """Callback event handler for clicking the "Re-compute" checkbox."""
        common.on_state_changed_recompute(self)

    def update_frame_range_dropdowns(self):
        """Updates ranges displayed in dropdowns per available files on disk."""
        dlg = self.dlg
        gb = dlg.gb_export_data
        dds = [dlg.dd_export_data_first, dlg.dd_export_data_last]
        for dd in dds:
            common.call_force_refreshing(self, common.populate_dropdown, self, gb, dd)

    def on_changed_dropdown(self, gb, dd):
        """Callback event handler for changed dropdown.

        Args:
            gb (QtWidgets.QGroupBox): Group box for the tab.
            dd (QtWidgets.QComboBox): Dropdown UI element.
        """
        common.on_changed_dropdown(self, gb, dd)
        self.update_viewer_buttons()

        if dd.objectName().endswith("_data_format"):
            self.update_ec2_dropdown()
            self.update_file_type()
            self.update_label_resolution()
        elif dd.objectName().endswith("_data_type"):
            self.update_frame_range_dropdowns()

    def on_changed_line_edit(self, gb, le):
        """Callback event handler for changed line edit.

        Args:
            gb (QtWidgets.QGroupBox): Group box for the tab.
            le (QtWidgets.QLineEdit): Line edit UI element.
        """
        common.on_changed_line_edit(self, gb, le)

    def on_state_changed_alpha_blend(self):
        """Callback event handler for changed alpha value."""
        if not self.is_refreshing_data:
            self.update_flagfile(self.flagfile_fn)

    def on_state_changed_farm(self, state):
        """Callback event handler for changed "AWS" checkbox.

        Args:
            state (str): Identifier of the callback state.
        """
        common.on_state_changed_farm(self, state)

    def on_terminate_cluster(self):
        """Terminates a running cluster."""
        gb = self.dlg.gb_export_data
        common.on_terminate_cluster(self, gb)

    def on_changed_preview(self):
        """Callback event handler for changed image previews."""
        common.on_changed_preview(self)

    def populate_dropdowns(self, gb):
        """Populates the dropdowns in the tab.

        Args:
            gb (QtWidgets.QGroupBox): Group box for the tab.
        """
        if gb == self.dlg.gb_export_data:
            dds_pri = [self.dlg.dd_export_data_type]
        else:
            dds_pri = []
        common.populate_dropdowns(self, gb, dds_pri)

    def setup_data(self):
        """Sets up callbacks and initial UI element statuses."""
        dlg = self.dlg
        dlg.cb_export_recompute.setChecked(False)
        dlg.label_export_farm_workers.setEnabled(False)
        dlg.dd_export_farm_workers.setEnabled(False)
        dlg.cb_export_alpha_blend.setChecked(True)
        dlg.gb_export_farm.setEnabled(True)
        dlg.btn_export_data_download_meshes.setEnabled(self.parent.is_aws)
        callbacks = {
            dlg.cb_export_recompute: self.on_state_changed_recompute,
            dlg.btn_export_data_smr_view: (
                lambda: self.activate_ipc(config.DOCKER_SMR_ONSCREEN_IPC)
            ),
            dlg.btn_export_data_rift_view: (
                lambda: self.activate_ipc(config.DOCKER_RIFT_VIEWER_IPC)
            ),
            dlg.cb_export_alpha_blend: self.on_state_changed_alpha_blend,
            dlg.btn_export_farm_terminate_cluster: self.on_terminate_cluster,
            dlg.btn_export_farm_ec2_dashboard: self.on_ec2_dashboard,
            dlg.btn_export_data_download_meshes: self.on_download_meshes,
            dlg.gb_export_farm: self.on_state_changed_farm,
        }
        common.setup_data(self, callbacks)

    def disable_tab_if_no_data(self):
        """Prevents navigation to the tab if the required data is not present."""
        common.disable_tab_if_no_data(self, self.dlg.btn_export_data_run)
