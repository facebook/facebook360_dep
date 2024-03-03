#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Common functions used across the UI tabs.

The UI shares several common functions across its tabs. Unlike dep_util, this file
contains functions that specifically reference elements in the tab. This means, if
further extension of the UI is pursued, this file should be reserved for common
functions that are *explicitly* tied to the UI and dep_util for functions that could
be used in contexts outside the UI.
"""

import collections
import datetime
import glob
import os
import shutil
import subprocess
import sys

from PyQt5 import QtCore, QtWidgets

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "aws"))
sys.path.append(os.path.join(dir_scripts, "render"))
sys.path.append(os.path.join(dir_scripts, "util"))

import dep_util
import glog_check as glog
import scripts.render.config as config
from log_reader import LogReader
from scripts.aws.create import (
    get_render_pid,
    get_staging_info,
    has_render_flag,
    run_ssh_command,
)
from scripts.aws.util import AWSUtil
from scripts.render.network import LAN
from scripts.util.system_util import (
    get_flags,
    get_flags_from_flagfile,
    image_type_paths,
    run_command,
)
from slider_image_thresholds import SliderWidget

script_dir = os.path.dirname(os.path.realpath(__file__))
scripts_dir = os.path.abspath(os.path.join(script_dir, os.pardir))
dep_dir = os.path.join(scripts_dir, os.pardir)
dep_bin_dir = os.path.join(dep_dir, "build", "bin")
dep_res_dir = os.path.join(dep_dir, "res")
dep_flags_dir = os.path.join(dep_res_dir, "flags")
os.makedirs(dep_flags_dir, exist_ok=True)

source_root = os.path.join(dep_dir, "source")
depth_est_src = os.path.join(source_root, "depth_estimation")
render_src = os.path.join(source_root, "render")
render_scripts = os.path.join(scripts_dir, "render")

type_color_var = "color_variance"
type_fg_mask = "fg_mask"

threshold_sliders = {
    # attr: type, printed name, slider index, max value, default value
    "noise": [type_color_var, "Noise variance", 1, 1.5e-3, 4e-5],
    "detail": [type_color_var, "Detail variance", 2, 2e-2, 1e-3],
    "blur": [type_fg_mask, "Blur radius", 1, 20, 2],
    "closing": [type_fg_mask, "Closing size", 2, 20, 4],
    "thresh": [type_fg_mask, "Threshold", 3, 1, 3e-2],
}


def init(parent):
    """Sets up all the UI global internals (logs, data, and flags) and any
    tab specific components.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    parent.is_refreshing_data = True
    parent.initialize_paths()
    parent.set_default_top_level_paths()
    parent.setup_logs()
    parent.setup_data()
    parent.setup_flags()
    if "retrieve_missing_flagfiles" in dir(parent):
        parent.retrieve_missing_flagfiles()
    if "add_default_flags" in dir(parent):
        parent.add_default_flags()
    if "setup_thresholds" in dir(parent):
        parent.setup_thresholds()
    if "add_data_type_validators" in dir(parent):
        parent.add_data_type_validators()
    if "setup_farm" in dir(parent):
        parent.setup_farm()
    if "update_run_button_text" in dir(parent):
        parent.update_run_button_text()
    parent.is_refreshing_data = False


def setup_aws_config(parent):
    """Sets up the configuration of the Kubernetes cluster.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    if parent.parent.is_aws:
        create_flagfile = os.path.join(
            parent.path_flags, parent.app_name_to_flagfile[parent.app_aws_create]
        )
        if os.path.exists(create_flagfile):
            create_flags = get_flags_from_flagfile(create_flagfile)
            if "cluster_size" in create_flags:
                spin_num_workers = getattr(
                    parent.dlg, f"spin_{parent.tag}_farm_num_workers", None
                )
                spin_num_workers.setValue(int(create_flags["cluster_size"]))
            if "instance_type" in create_flags:
                dd_ec2 = getattr(parent.dlg, f"dd_{parent.tag}_farm_ec2", None)
                dd_ec2.setCurrentText(create_flags["instance_type"])


def setup_farm(parent):
    """Sets up the UI to interact with a LAN cluster.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    initialize_farm_groupbox(parent)
    ip_begin, _ = parent.parent.ui_flags.master.rsplit(".", 1)
    parent.lan = LAN(f"{ip_begin}.255")


def get_tooltip(parent, app_name):
    """Gets the help tooltip display of a binary.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        app_name (str): Name of the binary.

    Returns:
        str: Help from the binary.
    """
    dir = scripts_dir if app_name.endswith(".py") else dep_bin_dir
    tooltip = dep_util.get_tooltip(os.path.join(dir, app_name))
    if not tooltip:
        parent.log_reader.log_warning(f"Cannot get tooltip for: {app_name}")
    return tooltip


def initialize_paths(parent):
    """Initializes paths for scripts and flags depending on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    tag = parent.tag
    parent.app_name_to_flagfile = {}

    if tag in ["bg", "depth", "export"]:
        parent.app_name = "render/render.py"
        if tag in ["depth", "export"]:
            parent.app_aws_clean = "aws/clean.py"
            parent.app_aws_create = "aws/create.py"
            parent.app_name_to_flagfile[parent.app_aws_clean] = "clean.flags"

    if tag == "calibrate":
        parent.app_name = "Calibration"
        parent.flagfile_basename = "calibration.flags"
    elif tag == "bg":
        parent.flagfile_basename = "render_background.flags"
    elif tag == "depth":
        parent.flagfile_basename = "render_depth.flags"
        parent.app_name_to_flagfile[parent.app_aws_create] = "aws_create_video.flags"
    elif tag == "export":
        parent.flagfile_basename = "render_export.flags"
        parent.app_name_to_flagfile[parent.app_aws_create] = "aws_create_export.flags"
        parent.app_aws_download_meshes = "aws/download_meshes.py"
        parent.app_name_to_flagfile[parent.app_aws_download_meshes] = (
            "download_meshes.flags"
        )

    parent.app_name_to_flagfile[parent.app_name] = parent.flagfile_basename
    parent.tooltip = get_tooltip(parent, parent.app_name)
    parent.is_refreshing_data = False
    parent.is_process_killed = False
    parent.threshs_tooltip = "Click and drag to pan, scroll to zoom in and out"
    parent.script_dir = script_dir


def setup_logs(parent):
    """Sets up logging system for dialog on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.

    Returns:
        LogReader: Reader configured for the current tab.
    """
    tag = parent.tag
    qt_text_edit = getattr(parent.dlg, f"text_{tag}_log", None)
    qt_tab_widget = getattr(parent.dlg, f"w_{tag}_preview", None)
    tab_idx = qt_tab_widget.count() - 1  # log is always the last tab
    ts = dep_util.get_timestamp("%Y%m%d%H%M%S.%f")
    name = parent.__class__.__name__
    log_file = os.path.join(parent.path_logs, f"{name}_{ts}")
    log_reader = LogReader(qt_text_edit, parent, log_file)
    log_reader.set_tab_widget(qt_tab_widget, tab_idx)
    return log_reader


def setup_flagfile_tab(parent):
    """Sets up the flags according to the corresponding flagfile on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    tag = parent.tag
    dlg = parent.dlg
    qt_text_edit = getattr(dlg, f"text_{tag}_flagfile_edit", None)
    qt_btn_save = getattr(dlg, f"btn_{tag}_flagfile_save", None)
    qt_text_edit.textChanged.connect(parent.on_changed_flagfile_edit)
    qt_btn_save.clicked.connect(parent.save_flag_file)
    qt_btn_save.setEnabled(False)


def setup_file_explorer(parent):
    """Creates the file explorer rooted  on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    dlg = parent.dlg
    parent.fs_tree = dlg.tree_file_explorer
    path = parent.path_project
    parent.fs_model, parent.fs_tree = dep_util.setup_file_explorer(parent.fs_tree, path)
    parent.fs_tree.clicked.connect(lambda: preview_file(parent))


def preview_file(parent):
    """Displays the file and its label on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    dlg = parent.dlg
    frame = dlg.label_preview_image
    label = dlg.label_preview_path
    project = parent.path_project
    prefix = f"{project}/"
    dep_util.preview_file(parent.fs_model, parent.fs_tree, frame, label, prefix)


def switch_ui_elements_for_processing(parent, gb, state):
    """Switches element interaction when processing on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        state (str): Identifier of the callback state.
    """
    # Buttons
    parent.update_buttons(gb)

    # Switch all other sections, except the file explorer
    dlg = parent.dlg
    for gbi in dlg.findChildren(QtWidgets.QGroupBox):
        if gbi != gb and not gbi.objectName().endswith("_file_explorer"):
            gbi.setEnabled(state)

    # Switch current group box elements
    prefixes = ["cb_", "dd_", "val_", "label_"]
    dep_util.switch_objects_prefix(gb, prefixes, state)

    # Switch tabs that are not image preview or log
    for w in dlg.findChildren(QtWidgets.QWidget):
        name = w.objectName()
        ignore = name.endswith("_preview") or name.endswith("_log")
        if name.startswith("tab_") and not ignore:
            w.setEnabled(state)

    # Switch other sections
    for s in parent.parent.sections:
        if s != parent:
            dep_util.set_tab_enabled(parent.dlg.w_steps, s.tag, state)


def cancel_process(parent):
    """Stops a running process on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    running_render = False  # Render has to be explicitly killed since it runs detached
    if parent.is_farm and parent.is_aws:
        processes = parent.log_reader.get_processes()
        for process in processes:
            if process == "run_aws_create" or process.startswith("run_export"):
                running_render = True

    if running_render:
        aws_util = AWSUtil(
            parent.path_aws_credentials, region_name=parent.parent.aws_util.region_name
        )
        _, ip_staging = get_staging_info(aws_util, parent.path_aws_ip_file)
        if ip_staging:
            render_pid = get_render_pid(parent.path_aws_key_fn, ip_staging)
            if render_pid is not None:
                run_ssh_command(
                    parent.path_aws_key_fn, ip_staging, f"kill -9 {render_pid}"
                )

    parent.log_reader.kill_all_processes()
    parent.is_process_killed = True

    if "reset_run_button_text" in dir(parent):
        parent.reset_run_button_text()


def is_cloud_running_process(parent):
    """Checks if a render process is being run on the cloud"""
    key_fn = parent.path_aws_key_fn
    if not parent.is_aws or not parent.is_farm or not os.path.isfile(key_fn):
        return False

    aws_util = AWSUtil(
        parent.path_aws_credentials, region_name=parent.parent.aws_util.region_name
    )
    _, ip_staging = get_staging_info(
        aws_util, parent.path_aws_ip_file, start_instance=False
    )
    if not ip_staging:
        return False

    tag = parent.tag
    if tag not in ["depth", "export"]:
        return False

    flag = "run_depth_estimation"
    value = tag == "depth"
    return has_render_flag(key_fn, ip_staging, flag, value)


def sync_with_s3(parent, gb, subdirs):
    """Synchronizes data from the local directory to S3.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        subdirs (list[str]): Local path to be synced.
    """
    run_silently = not parent.parent.ui_flags.verbose
    cmds = []

    parent.log_reader.log_notice(f"Syncing frames with S3...")

    for subdir in subdirs:
        local = os.path.join(config.DOCKER_INPUT_ROOT, subdir)
        remote = os.path.join(parent.parent.ui_flags.project_root, subdir)

        if "_levels" in subdir:
            locals = [
                os.path.join(local, f"level_{l}") for l in range(len(config.WIDTHS))
            ]
        else:
            locals = [local]

        # Tar frames
        tar_app_path = os.path.join(scripts_dir, "util", "tar_frame.py")
        for local_i in locals:
            frames = dep_util.get_frame_list(local_i)
            if not frames:
                if not run_silently:
                    print(glog.yellow(f"No frames found for S3 syncing in {local_i}"))
                continue
            for frame in frames:
                cmds.append(f"python3.7 {tar_app_path} --src={local_i} --frame={frame}")
        cmds.append(f"aws s3 sync {local} {remote} --exclude '*' --include '*.tar'")

    p_id = f"sync_results_s3_{parent.tag}"
    cmd_and = " && ".join(cmds)
    cmd = f'/bin/sh -c "{cmd_and}"'
    start_process(parent, cmd, gb, p_id, run_silently)


def on_process_finished(parent, p_id):
    """Callback event handler for a process completing on the specified tab.

    Args:
        p_id (str): PID of completed process.
    """
    if not p_id or p_id.startswith("run"):
        parent.log_reader.remove_processes()
    else:
        parent.log_reader.remove_process(p_id)
    parent.refresh_data()

    if p_id.startswith("run") and "_export_" not in p_id:
        if "update_frame_names" in dir(parent):
            parent.update_frame_names()

        if "sync_with_s3" in dir(parent) and not parent.is_process_killed:
            if parent.parent.is_aws:
                parent.sync_with_s3()

    if len(parent.log_reader.get_processes()) == 0:
        # Re-enable UI elements
        switch_ui_elements_for_processing(parent, parent.log_reader.gb, True)

    # We may have data to enable other tabs
    if p_id.startswith("run"):
        [s.refresh_data() for s in parent.parent.sections if s != parent]

    if "update_run_button_text" in dir(parent):
        parent.update_run_button_text()

    parent.is_process_killed = False


def populate_dropdown(parent, gb, dd):
    """Populates a dropdown on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        dd (QtWidgets.QComboBox): Dropdown UI element.
    """
    project = parent.parent.path_project
    t = dep_util.remove_prefix(gb.objectName(), "gb_")
    dd_prev_text = dd.currentText() if dd.count() > 0 else ""
    tag = dep_util.remove_prefix(dd.objectName(), f"dd_{t}_")
    ps = parent.get_files(tag)
    dep_util.populate_dropdown(dd, ps, f"{project}/")
    dep_util.update_qt_dropdown(dd, dd_prev_text, add_if_missing=False)


def populate_dropdowns(parent, gb, dd_first=None):
    """Populates the dropdowns on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        dd_first (list[QtWidgets.QGroupBox], optional): Dropdowns to populate first.
    """
    if not dd_first:
        dd_first = []
    for dd in dd_first:
        populate_dropdown(parent, gb, dd)
    for dd in gb.findChildren(QtWidgets.QComboBox):
        if dd not in dd_first:
            populate_dropdown(parent, gb, dd)


def refresh_data(parent):
    """Updates UI elements to be in sync with data on disk on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    tag = parent.tag
    dlg = parent.dlg
    tab = getattr(dlg, f"t_{tag}", None)

    if tag in ["bg", "depth", "export"]:
        parent.path_rig_json = get_calibrated_rig_json(parent)

    if tag == "depth":
        parent.update_bg_checkbox()

    # This locks the dropdown callbacks while we re-populate them
    parent.is_refreshing_data = True
    for gb in tab.findChildren(QtWidgets.QGroupBox):
        gb.setEnabled(True)
        parent.populate_dropdowns(gb)
        parent.update_buttons(gb)
    if "flagfile_fn" in dir(parent):
        sync_data_and_flagfile(parent, parent.flagfile_fn)
    parent.disable_tab_if_no_data()
    parent.is_refreshing_data = False


def update_flagfile_edit(parent, flagfile_fn, switch_to_flag_tab=False):
    """Updates the edit box for the flagfile on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        flagfile_fn (str): Name of the flagfile.
        switch_to_flag_tab (bool, optional): Whether or not to switch tabs after updating.
    """
    if not os.path.isfile(flagfile_fn):
        return

    tag = parent.tag
    dlg = parent.dlg
    text = getattr(dlg, f"text_{tag}_flagfile_edit", None)
    preview = getattr(dlg, f"w_{tag}_preview", None)

    text.setPlainText(open(flagfile_fn).read())
    if switch_to_flag_tab:
        preview.setCurrentIndex(1)


def update_data_or_flags(
    parent, flagfile_fn, flagfile_from_data, switch_to_flag_tab=False
):
    """Updates the flagfile from the UI elements or vice versa on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        flagfile_fn (str): Name of the flagfile.
        flagfile_from_data (bool): Whether to load the flagfile from the data (True) or
            vice versa (False).
        switch_to_flag_tab (bool, optional): Whether or not to switch tabs after updating.
    """
    if not flagfile_fn:
        return

    flags = get_flags_from_flagfile(flagfile_fn)
    if flagfile_from_data:
        parent.update_flags_from_data(flags)
    else:
        parent.update_data_from_flags(flags)

    if flagfile_from_data:
        # Overwrite flag file
        sorted_flags = collections.OrderedDict(sorted(flags.items()))
        dep_util.write_flagfile(flagfile_fn, sorted_flags)

        # Refresh flagfile edit window
        parent.update_flagfile_edit(flagfile_fn, switch_to_flag_tab)


def sync_data_and_flagfile(
    parent, flagfile_fn, set_label=True, switch_to_flag_tab=False
):
    """Synchronizes displayed UI elements and contents of the flagfile.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        flagfile_fn (str): Name of the flagfile.
        set_label (bool, optional): Whether or not to update the flagfile label in the UI.
        switch_to_flag_tab (bool, optional): Whether or not to switch tabs after updating.
    """
    tag = parent.tag
    dlg = parent.dlg
    label = getattr(dlg, f"label_{tag}_flagfile_path", None)

    flagfile = os.path.basename(flagfile_fn)
    label.setText(flagfile)

    # flag file to data first, then data to flag file for missing info
    flagfile_from_data = False
    parent.update_data_or_flags(flagfile_fn, flagfile_from_data, switch_to_flag_tab)
    parent.update_data_or_flags(flagfile_fn, not flagfile_from_data, switch_to_flag_tab)


def disable_tab_if_no_data(parent, btn_run):
    """Prevents navigation to the tab if the required data is not present on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        btn_run (QtWidgets.QPushButton): UI button for tab switch.
    """
    if not btn_run.isEnabled():
        dep_util.set_tab_enabled(parent.dlg.w_steps, parent.tag, enabled=False)


def setup_project(parent, mkdirs=False):
    """Retrieves any missing flagfiles and sets the default flags on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        mkdirs (bool, optional): Whether or not to make the defined directories.
    """
    parent.is_refreshing_data = True
    parent.log_reader.log_header()
    parent.refresh_data()
    parent.is_refreshing_data = False


def save_flag_file(parent, flagfile_fn):
    """Saves flagfile from the UI to disk on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        flagfile_fn (str): Name of the flagfile.
    """
    if not os.path.isfile(flagfile_fn):
        return

    tag = parent.tag
    dlg = parent.dlg
    text_edit = getattr(dlg, f"text_{tag}_flagfile_edit", None)
    btn_save = getattr(dlg, f"btn_{tag}_flagfile_save", None)

    with open(flagfile_fn, "w") as f:
        f.write(text_edit.toPlainText())
    f.close()

    # Disable save button
    btn_save.setEnabled(False)

    # Update corresponding groupbox
    flagfile_from_data = False  # flagfile to data
    parent.update_data_or_flags(flagfile_fn, flagfile_from_data)


def update_flagfile(parent, flagfile_fn):
    """Updates the edit box for the flagfile on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        flagfile_fn (str): Name of the flagfile.
    """
    parent.update_data_or_flags(flagfile_fn, flagfile_from_data=True)


def retrieve_missing_flagfiles(parent):
    """Copies the missing flagfiles to project for local modification on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    tag = parent.tag
    if tag == "calibrate":
        ff_base = "calibration.flags"
    elif tag in ["bg", "depth", "export"]:
        ff_base = "render.flags"

    ffs_expected = [[ff_base, parent.flagfile_fn]]
    if tag in ["depth", "export"]:
        ff_aws_create = os.path.join(
            parent.path_flags, parent.app_name_to_flagfile[parent.app_aws_create]
        )
        ffs_expected.append(["aws_create.flags", ff_aws_create])
    for ff_src_rel, ff_dst_abs in ffs_expected:
        if not os.path.isfile(ff_dst_abs):
            ff_src_abs = os.path.join(dep_flags_dir, ff_src_rel)
            os.makedirs(os.path.dirname(ff_dst_abs), exist_ok=True)
            shutil.copyfile(ff_src_abs, ff_dst_abs)
            update_flagfile(parent, ff_dst_abs)


def add_default_flags(parent):
    """Retrieves the default flags to the local flagfile on the specified tab from
    either the source or scripts binaries.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    default_flags = {}

    tag = parent.tag
    if tag in ["bg", "depth"]:
        default_flags.update(
            {
                os.path.join(depth_est_src, "DerpCLI.cpp"): {
                    "max_depth_m",
                    "min_depth_m",
                    "resolution",
                    "var_high_thresh",
                    "var_noise_floor",
                }
            }
        )

    if tag == "depth":
        default_flags.update(
            {
                os.path.join(render_scripts, "setup.py"): {"do_temporal_filter"},
                os.path.join(depth_est_src, "TemporalBilateralFilter.cpp"): {
                    "time_radius"
                },
                os.path.join(render_src, "GenerateForegroundMasks.cpp"): {
                    "blur_radius",
                    "morph_closing_size",
                    "threshold",
                },
            }
        )
    elif tag == "export":
        default_flags.update(
            {
                os.path.join(render_src, "SimpleMeshRenderer.cpp"): {"width"},
                os.path.join(render_src, "ConvertToBinary.cpp"): {"output_formats"},
            }
        )

    flagfile_fn = os.path.join(parent.path_flags, parent.flagfile_basename)
    flags = get_flags_from_flagfile(flagfile_fn)
    for source in default_flags:
        if os.path.isfile(source):
            source_flags = get_flags(source)
        else:
            source_flags
        desired_flags = default_flags[source]
        for source_flag in source_flags:
            flag_name = source_flag["name"]

            # Only add the default flag if not already present in current flags
            if flag_name in desired_flags:
                if flag_name not in flags or flags[flag_name] == "":
                    flags[flag_name] = source_flag["default"]

    # Add run flags
    if tag == "bg":
        flags["run_generate_foreground_masks"] = False
        flags["run_precompute_resizes"] = True
        flags["run_depth_estimation"] = True
        flags["run_convert_to_binary"] = False
        flags["run_fusion"] = False
        flags["run_simple_mesh_renderer"] = False
        flags["use_foreground_masks"] = False
    elif tag == "depth":
        flags["run_depth_estimation"] = True
        flags["run_precompute_resizes"] = True
        flags["run_precompute_resizes_foreground"] = True
        flags["run_convert_to_binary"] = False
        flags["run_fusion"] = False
        flags["run_simple_mesh_renderer"] = False
    elif tag == "export":
        flags["run_generate_foreground_masks"] = False
        flags["run_precompute_resizes"] = False
        flags["run_precompute_resizes_foreground"] = False
        flags["run_depth_estimation"] = False

    # Overwrite flag file
    sorted_flags = collections.OrderedDict(sorted(flags.items()))
    dep_util.write_flagfile(flagfile_fn, sorted_flags)


def get_calibrated_rig_json(parent):
    """Finds calibrated rig in the project.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.

    Returns:
        str: Name of the calibrated rig (assumes the rig contains "_calibrated.json").
    """
    has_log_reader = "log_reader" in dir(parent)
    ps = dep_util.get_files_ext(parent.path_rigs, "json", "calibrated")
    if len(ps) == 0:
        if has_log_reader:
            parent.log_reader.log_warning(f"No rig files found in {parent.path_rigs}")
        return ""
    if len(ps) > 1:
        ps_str = "\n".join(ps)
        if has_log_reader:
            parent.log_reader.log_warning(
                f"Too many rig files found in {parent.path_rigs}:\n{ps_str}"
            )
        return ""
    return ps[0]


def update_run_button_text(parent, btn):
    """Updates the text of the Run button depending on the existance of a process
    running on the cloud
    """
    text_run_btn = "Run"
    if is_cloud_running_process(parent):
        text_run_btn = "Re-attach"
    btn.setText(text_run_btn)


def update_buttons(parent, gb, ignore=None):
    """Enables buttons and dropdowns according to whether or not data is present on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        ignore (list[QtWidgets.QGroupBox], optional): Buttons to not update.

    Returns:
        tuple[bool, bool, bool]: Whether or not the UI is currently running a process and if it
            has all its dropdowns.
    """
    if not ignore:
        ignore = []

    has_all_dropdowns = True
    for dd in gb.findChildren(QtWidgets.QComboBox):
        if not dd.currentText() and dd not in ignore:
            has_all_dropdowns = False
            break

    has_all_values = True
    for v in gb.findChildren(QtWidgets.QLineEdit):
        if v.objectName() and not v.text() and v not in ignore:
            has_all_values = False
            break

    is_running = parent.log_reader.is_running()
    for btn in gb.findChildren(QtWidgets.QPushButton):
        btn_name = btn.objectName()
        if btn in ignore:
            continue
        if btn_name.endswith("_run"):
            btn.setEnabled(not is_running and has_all_dropdowns and has_all_values)
        elif btn_name.endswith("_cancel"):
            btn.setEnabled(is_running)
        elif btn_name.endswith("_threshs"):
            btn.setEnabled(not is_running and has_all_dropdowns)
        elif btn_name.endswith("_view"):
            btn.setEnabled(not is_running)
        elif btn_name.endswith("_download_meshes"):
            btn.setEnabled(not is_running)
    return is_running, has_all_dropdowns, is_running


def on_changed_dropdown(parent, gb, dd):
    """Callback event handler for changed dropdown on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        dd (QtWidgets.QComboBox): Dropdown UI element.
    """
    if not parent.is_refreshing_data:
        name = dd.objectName()
        if not name.endswith(
            "_farm_ec2"
        ):  # farm_ec2 dropdowns are not used in flagfile
            parent.update_flagfile(parent.flagfile_fn)

        # Check if we need to update the threshold image
        if name.endswith(("_camera", "_frame_bg", "_first")):
            # Check if we are already in a threshold tab, else default to color variance
            tag = parent.tag
            tab_widget = getattr(parent.dlg, f"w_{tag}_preview", None)
            tab_idx = tab_widget.currentIndex()
            if tab_widget.widget(tab_idx).objectName().endswith("_fg_mask"):
                type = type_fg_mask
            else:
                type = type_color_var

            if "run_thresholds" in dir(parent):
                parent.run_thresholds(type)


def on_changed_line_edit(parent, gb, le):
    """Callback event handler for changed line edit on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        le (_): Ignore
    """
    if not parent.is_refreshing_data:
        parent.update_buttons(gb)
        parent.update_flagfile(parent.flagfile_fn)


def setup_groupbox(gb, callbacks):
    """Sets up callbacks for any groupboxes on the specified tab.

    Args:
        gb (QtWidgets.QGroupBox): Group box for the tab.
        callbacks (dict[QtWidgets.QGroupBox, func : QEvent -> _]): Callbacks for the UI elements.
    """
    if gb.isCheckable() and gb in callbacks:
        gb.toggled.connect(callbacks[gb])


def setup_checkboxes(gb, callbacks):
    """Sets up callbacks for any checkboxes on the specified tab.

    Args:
        gb (QtWidgets.QGroupBox): Group box for the tab.
        callbacks (dict[QtWidgets.QGroupBox, func : QEvent -> _]): Callbacks for the UI elements.
    """
    for cb in gb.findChildren(QtWidgets.QCheckBox):
        if cb in callbacks:
            cb.stateChanged.connect(callbacks[cb])


def setup_dropdowns(parent, gb):
    """Sets up callbacks for any dropdowns on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QComboBox): Group box for the tab.
    """
    if "on_changed_dropdown" in dir(parent):
        for dd in gb.findChildren(QtWidgets.QComboBox):
            dd.currentTextChanged.connect(
                lambda state, y=gb, z=dd: parent.on_changed_dropdown(y, z)
            )
            dd.activated.connect(
                lambda state, y=gb, z=dd: parent.on_changed_dropdown(y, z)
            )


def setup_lineedits(parent, gb):
    """Sets up callbacks for any line edits on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
    """
    if "on_changed_line_edit" in dir(parent):
        for le in gb.findChildren(QtWidgets.QLineEdit):
            le.textChanged.connect(
                lambda state, y=gb, z=le: parent.on_changed_line_edit(y, z)
            )


def setup_buttons(parent, gb, callbacks):
    """Sets up callbacks for any buttons on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        callbacks (dict[QtWidgets.QPushButton, func : QEvent -> _]): Callbacks for the UI elements.
    """
    for btn in gb.findChildren(QtWidgets.QPushButton):
        if btn in callbacks:
            callback = callbacks[btn]
        else:
            name = btn.objectName()
            callback = None
            if name.endswith("_refresh"):
                callback = parent.refresh
            elif name.endswith("_run"):
                callback = parent.run_process
            elif name.endswith("_cancel"):
                callback = parent.cancel_process
            elif name.endswith("_threshs"):
                callback = parent.run_thresholds
            elif name.endswith("_logs"):
                callback = parent.get_logs
            else:
                parent.log_reader.log_error(f"Cannot setup button {name}")

        if callback:
            btn.clicked.connect(callback)


def on_changed_preview(parent):
    """Callback event handler for changed image previews on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    tag = parent.tag
    tab_widget = getattr(parent.dlg, f"w_{tag}_preview", None)
    tab_idx = tab_widget.currentIndex()
    tab_name = tab_widget.widget(tab_idx).objectName()
    if "_threshs_" in tab_name:
        if tab_name.endswith("_fg_mask"):
            type = type_fg_mask
        else:
            type = type_color_var

        if not parent.is_refreshing_data:
            parent.run_thresholds(type)


def setup_preview(parent):
    """Creates preview window in the UI and connects a callback on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    tag = parent.tag
    dlg = parent.dlg
    btn_log_clear = getattr(dlg, f"btn_{tag}_log_clear", None)
    text_log = getattr(dlg, f"text_{tag}_log", None)
    preview = getattr(dlg, f"w_{tag}_preview", None)
    btn_log_clear.clicked.connect(lambda: text_log.clear())
    preview.setCurrentIndex(0)

    if "on_changed_preview" in dir(parent):
        preview.currentChanged.connect(parent.on_changed_preview)


def setup_data(parent, callbacks=None):
    """Sets up callbacks and initial UI element statuses on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        callbacks (dict[QtWidgets.QGroupBox, func : QEvent -> _]): Callbacks for the UI elements.
    """
    tag = parent.tag
    dlg = parent.dlg
    tab = getattr(dlg, f"t_{tag}", None)

    if not callbacks:
        callbacks = {}

    for gb in tab.findChildren(QtWidgets.QGroupBox):
        setup_groupbox(gb, callbacks)
        setup_checkboxes(gb, callbacks)
        setup_dropdowns(parent, gb)
        setup_lineedits(parent, gb)
        setup_buttons(parent, gb, callbacks)

    # Preview tabs
    setup_preview(parent)


def update_noise_detail(parent, noise, detail):
    """Updates noise/detail thresholds interaction on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        noise (float): Noise threshold.
        detail (float): Detail threshold.
    """
    # Modify flagfile
    parent.update_data_or_flags(
        parent.flagfile_fn, flagfile_from_data=True, switch_to_flag_tab=False
    )

    # Update flagfile edit window
    parent.update_flagfile_edit(parent.flagfile_fn, switch_to_flag_tab=False)


def update_fg_masks_thresholds(parent, blur, closing, thresh):
    """Updates thresholds and display for the foreground masking on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        blur (int, optional): Gaussian blur radius.
        closing (int, optional): Closure (for sealing holes).
        thresh (int, optional): Threshold applied to segment foreground and background
    """
    # Modify flagfile
    parent.update_data_or_flags(
        parent.flagfile_fn, flagfile_from_data=True, switch_to_flag_tab=False
    )

    # Update flagfile edit window
    parent.update_flagfile_edit(parent.flagfile_fn, switch_to_flag_tab=False)


def log_missing_image(parent, path_color, cam_id, frame):
    """Prints a warning if an image cannot be located.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        path_color (str): Path to the directory with color images.
        cam_id (str): Name of the camera.
        frame (str): Name of the frame (0-padded, six digits).
    """
    parent.log_reader.log_warning(f"Cannot find frame {cam_id}/{frame} in {path_color}")


def update_thresholds_color_variance(parent, path_color, labels=None):
    """Updates the displayed thresholds for color variance on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        path_color (str): Path to the directory with color images.
        labels (list[str], optional): Labels used to filter UI elements to update.
    """
    labels = labels if labels is not None else ("_frame_bg", "_first")
    dlg = parent.dlg
    for dd in parent.dlg.findChildren(QtWidgets.QComboBox):
        name = dd.objectName()
        if name.endswith(labels):
            frame = dd.currentText()
        elif name.endswith("_camera"):
            cam_id = dd.currentText()
    image_path = dep_util.get_level_image_path(path_color, cam_id, frame)
    if not image_path:
        log_missing_image(parent, path_color, cam_id, frame)
        return

    tag = parent.tag
    w_image = getattr(dlg, f"w_{tag}_threshs_image_{type_color_var}", None)

    # Foreground masks are generated at the finest level of the pyramid
    res = max(config.WIDTHS)
    w_image.color_var.set_image(image_path, res)

    noise = float(parent.slider_noise.get_label_text())
    detail = float(parent.slider_detail.get_label_text())
    project = parent.parent.path_project
    fn = dep_util.remove_prefix(image_path, f"{project}/")
    getattr(dlg, f"label_{tag}_threshs_filename_{type_color_var}", None).setText(fn)

    # Force update
    w_image.update_thresholds(noise=noise, detail=detail)


def update_thresholds_fg_mask(parent, paths_color):
    """Updates thresholds and display for the foreground masking using values from UI
    on the specified tab."

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        paths_color (list[str]): Paths to the directory with color images.
    """
    dlg = parent.dlg
    frames = [None] * 2
    for dd in parent.dlg.findChildren(QtWidgets.QComboBox):
        name = dd.objectName()
        if name.endswith("_frame_bg"):
            frames[0] = dd.currentText()
        elif name.endswith("_first"):
            frames[1] = dd.currentText()
        elif name.endswith("_camera"):
            cam_id = dd.currentText()

    bg_image_path = dep_util.get_level_image_path(paths_color[0], cam_id, frames[0])
    if not bg_image_path:
        log_missing_image(parent, paths_color[0], cam_id, frames[0])
        return

    fg_image_path = dep_util.get_level_image_path(paths_color[1], cam_id, frames[1])
    if not fg_image_path:
        log_missing_image(parent, paths_color[1], cam_id, frames[1])
        return

    tag = parent.tag
    w_image = getattr(dlg, f"w_{tag}_threshs_image_{type_fg_mask}", None)

    # Foreground masks are generated at the finest level of the pyramid
    res = max(config.WIDTHS)
    w_image.fg_mask.set_images(bg_image_path, fg_image_path, res)

    blur = float(parent.slider_blur.get_label_text())
    closing = float(parent.slider_closing.get_label_text())
    thresh = float(parent.slider_thresh.get_label_text())

    project = parent.parent.path_project
    fn_bg = dep_util.remove_prefix(bg_image_path, f"{project}/")
    fn_fg = dep_util.remove_prefix(fg_image_path, f"{project}/")
    getattr(dlg, f"label_{tag}_threshs_filename_{type_fg_mask}", None).setText(
        f"{fn_bg} vs {fn_fg}"
    )

    # Force update
    w_image.update_thresholds(blur=blur, closing=closing, thresh=thresh)


def run_thresholds_after_wait(parent, type):
    """Computes the threshold and displays after a delay on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        type (Union[ColorVariance, ForegroundMask]): Instance where thresholds
            can be run.
    """
    # Apply flag file values in case it had unsaved changes
    parent.save_flag_file()

    tag = parent.tag
    dlg = parent.dlg
    label = getattr(dlg, f"label_{tag}_threshs_tooltip_{type}", None)
    label.setToolTip(parent.threshs_tooltip)
    getattr(dlg, f"w_{tag}_threshs_image_{type}", None).set_zoom_level(0)

    if type == type_color_var:
        parent.setup_thresholds_color_variance()
        parent.update_thresholds_color_variance()
    elif type == type_fg_mask:
        parent.setup_thresholds_fg_masks()
        parent.update_thresholds_fg_mask()


def run_thresholds(parent, type):
    """Runs thresholding based on values in the UI and update UI display on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        type (Union[ColorVariance, ForegroundMask]): Instance where thresholds are run.
    """
    tag = parent.tag
    tab_widget = getattr(parent.dlg, f"w_{tag}_preview", None)
    dep_util.switch_tab(tab_widget, f"_threshs_{type}")

    # HACK: if we try to draw on a widget too quickly after switching tabs the resulting image
    # does not span all the way to the width of the widget. We can wait a few milliseconds to
    # let the UI "settle"
    parent.timer = QtCore.QTimer(parent.parent)
    parent.timer.timeout.connect(lambda: parent.run_thresholds_after_wait(type))
    parent.timer.setSingleShot(True)
    parent.timer.start(10)  # 10ms


def output_has_images(output_dirs):
    """Whether or not outputs already have results.

    Args:
        output_dirs (list[str]): List of directories where outputs will be saved.

    Returns:
        bool: Whether or not the output directories all have at least one valid file.
    """
    for d in output_dirs:
        if dep_util.get_first_file_path(d):
            return True
    return False


def run_process_check_existing_output(parent, gb, app_name, flagfile_fn, p_id):
    """Run terminal process and raise on failure.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        app_name (str): Name of the binary being executed.
        flagfile_fn (str): Name of the flagfile.
        p_id (str): PID name of the process to be run.
    """
    tag = parent.tag
    cb_recompute = getattr(parent.dlg, f"cb_{tag}_recompute", None)
    if cb_recompute is not None:
        needs_rename = cb_recompute.isChecked()
        if needs_rename:
            # Rename current output directories using timestamp and create new empty ones
            ts = dep_util.get_timestamp()
            for d in parent.output_dirs:
                if not os.path.isdir(d):
                    continue
                d_dst = f"{d}_{ts}"
                parent.log_reader.log_notice(
                    f"Saving copy of {d} to {d_dst} before re-computing"
                )
                shutil.move(d, d_dst)
                os.makedirs(d, exist_ok=True)
        run_process(parent, gb, app_name, flagfile_fn, p_id, not needs_rename)


def start_process(parent, cmd, gb, p_id, run_silently=False):
    """Runs a terminal process and disables UI element interaction.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        cmd (str): Command to run in the terminal.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        p_id (str): PID name of the process being started.
    """
    if not run_silently:
        parent.log_reader.log(f"CMD: {cmd}")
    parent.log_reader.gb = gb
    parent.log_reader.setup_process(p_id)
    parent.log_reader.start_process(p_id, cmd)

    # Switch to log tab
    tag = parent.tag
    tab_widget = getattr(parent.dlg, f"w_{tag}_preview", None)
    dep_util.switch_tab(tab_widget, "_log")

    # Disable UI elements
    parent.switch_ui_elements_for_processing(False)


def run_process(
    parent, gb, app_name=None, flagfile_fn=None, p_id="run", overwrite=False
):
    """Runs an application on the terminal, using the associated flagfile.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        app_name (str, optional): Name of the binary being executed.
        flagfile_fn (str, optional): Name of the flagfile to supply to the binary. this
            will default to the flagfile associated with the binary if unspecified.
        p_id (str, optional): PID name of the process being started.
        overwrite (bool, optional): Whether or not to overwrite the local flagfile on disk.
    """
    # Apply flag file values in case it had unsaved changes
    parent.save_flag_file()

    if not app_name:
        app_name = parent.app_name
    is_py_script = app_name.endswith(".py")

    dir = scripts_dir if is_py_script else dep_bin_dir
    app_path = os.path.join(dir, app_name)
    if not os.path.isfile(app_path):
        parent.log_reader.log_warning(f"App doesn't exist: {app_path}")
        return

    if not flagfile_fn:
        flagfile_fn = parent.flagfile_fn

    if output_has_images(parent.output_dirs) and not overwrite:
        run_process_check_existing_output(parent, gb, app_name, flagfile_fn, p_id)
        return

    cmd = f'{app_path} --flagfile="{flagfile_fn}"'
    if is_py_script:
        cmd = f"python3.7 -u {cmd}"

    start_process(parent, cmd, gb, p_id)


def update_thresholds(parent, gb, type):
    """Updates the displayed thresholds for either color variance or foreground masks.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        type (Union[ColorVariance, ForegroundMask]): Instance where thresholds
            can be run.
    """
    if type == type_color_var:
        noise = parent.slider_noise.get_label_text()
        detail = parent.slider_detail.get_label_text()
        parent.update_noise_detail(noise, detail)
    elif type == type_fg_mask:
        blur = parent.slider_blur.get_label_text()
        closing = parent.slider_closing.get_label_text()
        thresh = parent.slider_thresh.get_label_text()
        parent.update_fg_masks_thresholds(blur, closing, thresh)

    # Update buttons
    parent.update_buttons(gb)


def on_state_changed_partial_360(parent):
    """Callback event handler for changed "partial coverage" checkbox on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    if not parent.is_refreshing_data:
        parent.update_flagfile(parent.flagfile_fn)


def on_state_changed_recompute(parent):
    """Callback event handler for changed "recompute" checkbox on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    if not parent.is_refreshing_data:
        parent.update_flagfile(parent.flagfile_fn)


def on_state_changed_use_bg(parent, gb):
    """Callback event handler for changed "use background" checkbox on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
    """
    if not parent.is_refreshing_data:
        parent.update_buttons(gb)
        parent.update_flagfile(parent.flagfile_fn)


def on_state_changed_farm(parent, state):
    """Callback event handler for changed "AWS" checkbox on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        state (str): Identifier of the callback state.
    """
    parent.is_farm = state > 0
    if not parent.is_refreshing_data:
        if "update_frame_range_dropdowns" in dir(parent):
            parent.update_frame_range_dropdowns()
        if "update_run_button_text" in dir(parent):
            parent.update_run_button_text()
        parent.update_flagfile(parent.flagfile_fn)


def setup_thresholds(parent, types):
    """Sets necessary thresholds apps on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        type (Union[ColorVariance, ForegroundMask]): Instance where thresholds
            can be run.
    """
    tag = parent.tag
    dlg = parent.dlg
    for attr in threshold_sliders:
        type, printed, num, max, default = threshold_sliders[attr]
        if type in types:
            name = getattr(dlg, f"label_{tag}_threshs_{num}_name_{type}", None)
            hs = getattr(dlg, f"hs_{tag}_threshs_{num}_{type}", None)
            label = getattr(dlg, f"label_{tag}_threshs_{num}_{type}", None)
            slider = SliderWidget(type, attr, name, printed, hs, label, max, default)
            setattr(parent, f"slider_{attr}", slider)

    for type in types:
        w_image = getattr(dlg, f"w_{tag}_threshs_image_{type}", None)
        w_viewer = getattr(dlg, f"w_{tag}_image_viewer_{type}", None)
        w_image.set_image_viewer(w_viewer)


def setup_thresholds_color_variance(parent):
    """Sets color variance thresholds apps on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    for slider in [parent.slider_noise, parent.slider_detail]:
        slider.setup(callback=parent.on_changed_slider)


def setup_thresholds_fg_masks(parent):
    """Sets up the default thresholds on foreground masks on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    for slider in [parent.slider_blur, parent.slider_closing, parent.slider_thresh]:
        slider.setup(callback=parent.on_changed_slider)


def update_data_from_flags(
    parent,
    flags,
    dropdowns=None,
    values=None,
    checkboxes=None,
    labels=None,
    prefix=None,
):
    """Updates UI elements from the flags on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        flags (dict[str, _]): Flags and their corresponding values.
        dropdowns (list[QtWidgets.QComboBox], optional): Dropdowns in the tab.
        values (dict[QtWidgets.QLineEdit, _], optional): Map from UI elements to values.
        checkboxes (list[QtWidgets.QCheckBox], optional): Checkboxes in the tab.
        labels (list[QtWidgets.QLabel], optional): Labels in the tab.
        prefix (str, optional): Prefix to append to values in the population of tab values.
    """
    if not dropdowns:
        dropdowns = []
    if not values:
        values = []
    if not checkboxes:
        checkboxes = []
    if not labels:
        labels = []

    flagfile = parent.flagfile_basename
    if not prefix:
        prefix = f"{parent.parent.path_project}/"

    for key, dd in dropdowns:
        error = dep_util.update_qt_dropdown_from_flags(flags, key, prefix, dd)
        if error:
            parent.log_reader.log_warning(f"{flagfile}: {error}")

    for key, val in values:
        dep_util.update_qt_lineedit_from_flags(flags, key, prefix, val)

    for key, cb in checkboxes:
        error = dep_util.update_qt_checkbox_from_flags(flags, key, prefix, cb)
        if error:
            parent.log_reader.log_warning(f"{flagfile}: {error}")

    for key, label in labels:
        dep_util.update_qt_label_from_flags(flags, key, prefix, label)


def get_notation(parent, attr):
    """Gets standard format for attribute on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        attr (str): Name of the attribute.

    Returns:
        str: Format string corresponding to the display notation.
    """
    if attr in ["noise", "detail", "thresh"]:
        notation = "{:.3e}"
    elif attr in ["blur", "closing"]:
        notation = "{:d}"
    else:
        parent.log_reader.log_error(f"Invalid slider attr: {attr}")
    return notation


def on_changed_slider(parent, slider, value):
    """Callback event handler for changes to a slider UI element on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        slider (QtWidgets.QSlider): Slider UI element.
        value (int/float): Value of the slider element.
    """
    type = slider.type
    attr = slider.attr
    notation = get_notation(parent, attr)
    if notation == "{:d}":
        value = int(value)
    slider.set_label(value, notation)
    tag = parent.tag
    w_image = getattr(parent.dlg, f"w_{tag}_threshs_image_{type}", None)
    if w_image.update_thresholds(**{attr: value}):
        # Update thresholds in flagfile
        parent.update_thresholds(type)


def initialize_farm_groupbox(parent):
    """Sets up the farm render box for the project path, i.e. AWS is displayed if
    rendering on an S3 project path and LAN if on a SMB drive.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    tag = parent.tag
    dlg = parent.dlg
    gb_farm = getattr(dlg, f"gb_{tag}_farm", None)
    grid_s3 = getattr(dlg, f"w_{tag}_farm_s3", None)
    grid_lan = getattr(dlg, f"w_{tag}_farm_lan", None)
    parent.is_aws = parent.parent.is_aws
    parent.is_lan = parent.parent.is_lan
    if not parent.is_aws and not parent.is_lan:
        gb_farm.hide()
    elif parent.is_aws:
        grid_lan.hide()
    elif parent.is_lan:
        grid_s3.hide()

    parent.ec2_instance_types_cpu = []
    parent.ec2_instance_types_gpu = []
    if parent.is_aws:
        # Get list of EC2 instances
        client = parent.parent.aws_util.session.client("ec2")
        ts = client._service_model.shape_for("InstanceType").enum
        ts = [t for t in ts if not t.startswith(config.EC2_UNSUPPORTED_TYPES)]
        parent.ec2_instance_types_cpu = [t for t in ts if t.startswith("c")]
        parent.ec2_instance_types_gpu = [t for t in ts if t.startswith(("p", "g"))]

    # Check if flagfile has farm attributes
    flagfile_fn = os.path.join(parent.path_flags, parent.flagfile_basename)
    flags = get_flags_from_flagfile(flagfile_fn)
    parent.is_farm = False
    for farm_attr in ["master", "workers", "cloud"]:
        if flags[farm_attr] != "":
            parent.is_farm = True
            break
    call_force_refreshing(parent, gb_farm.setChecked, parent.is_farm)


def show_resources(parent):
    """Displays resources used in the container.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.

    Returns:
        str: Resources (memory and CPU) being used.
    """
    return run_command("top -b -n 1")


def show_aws_resources(parent):
    """Displays resources used across the AWS cluster.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.

    Returns:
        src: Resources (memory and CPU) being used in the farm.
    """
    return "\n".join(parent.parent.aws_util.ec2_get_running_instances())


def get_aws_workers():
    """Get names of the instances in the AWS farm.

    Returns:
        list[str]: Instances IDs of EC2 instances in the farm.
    """
    with open(config.DOCKER_AWS_WORKERS) as f:
        lines = f.readlines()
    return lines


def set_aws_workers(workers):
    """Sets names of the instances in the AWS farm.

    Args:
        workers (list[str]): Instance IDs of EC2 instances in the farm.
    """
    with open(config.DOCKER_AWS_WORKERS, "w") as f:
        f.writelines([worker.id for worker in workers])


def popup_ec2_dashboard_url(parent):
    """Displays a link to the EC2 dashboard in a popup on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    region = parent.parent.aws_util.region_name
    prefix = f"{region}." if region else ""
    url = f"https://{prefix}console.aws.amazon.com/ec2#Instances"
    dep_util.popup_message(parent.parent, url, "EC2 Dashboard")


def popup_logs_locations(parent):
    """Displays the path to local logs in a popup on the specified tab.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
    """
    logs = [parent.log_reader.log_file]
    logs_workers = glob.iglob(f"{parent.path_logs}/Worker-*", recursive=False)
    for log in logs_workers:
        ts_log = datetime.datetime.fromtimestamp(os.path.getmtime(log))
        if ts_log > parent.parent.ts_start:
            logs.append(log)
    project = parent.parent.path_project
    logs = [dep_util.remove_prefix(l, f"{project}/") for l in logs]

    dep_util.popup_message(parent.parent, "\n".join(logs), "Logs")


def run_process_aws(parent, gb, p_id=None):
    """Runs the process to create a cluster on AWS and perform the render job.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
    """
    flags = {}
    flags["key_dir"] = os.path.dirname(parent.path_aws_key_fn)
    flags["key_name"] = os.path.splitext(os.path.basename(parent.path_aws_key_fn))[0]
    flags["csv_path"] = parent.path_aws_credentials
    flags["ec2_file"] = parent.path_aws_ip_file
    spin_num_workers = getattr(parent.dlg, f"spin_{parent.tag}_farm_num_workers", None)
    flags["cluster_size"] = int(spin_num_workers.value())
    flags["region"] = parent.parent.aws_util.region_name
    dd_ec2 = getattr(parent.dlg, f"dd_{parent.tag}_farm_ec2", None)
    flags["instance_type"] = dd_ec2.currentText()
    flags["tag"] = parent.tag

    # Overwrite flag file
    app_name = parent.app_aws_create
    flagfile_fn = os.path.join(parent.path_flags, parent.app_name_to_flagfile[app_name])
    dep_util.write_flagfile(flagfile_fn, flags)

    if not p_id:
        p_id = "run_aws_create"
    run_process(parent, gb, app_name, flagfile_fn, p_id)


def on_download_meshes(parent, gb):
    """Downloads meshes from S3. This is a no-op if not an S3 project.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
    """
    if not parent.parent.is_aws:
        return

    subdir = image_type_paths["video_bin"]
    flags = {}
    flags["csv_path"] = parent.path_aws_credentials
    flags["local_dir"] = os.path.join(config.DOCKER_INPUT_ROOT, subdir)
    flags["s3_dir"] = os.path.join(parent.parent.ui_flags.project_root, subdir)
    flags["verbose"] = parent.parent.ui_flags.verbose
    flags["watch"] = True  # NOTE: watchdog sometimes gets stale file handles in Windows

    # Overwrite flag file
    app_name = parent.app_aws_download_meshes
    flagfile_fn = os.path.join(parent.path_flags, parent.app_name_to_flagfile[app_name])
    dep_util.write_flagfile(flagfile_fn, flags)

    p_id = "download_meshes"
    run_process(parent, gb, app_name, flagfile_fn, p_id)


def on_terminate_cluster(parent, gb):
    """Terminates a running AWS cluster. This is a no-op if no cluster is up.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        gb (QtWidgets.QGroupBox): Group box for the tab.
    """
    flags = {}
    flags["key_dir"] = os.path.dirname(parent.path_aws_key_fn)
    flags["key_name"] = os.path.splitext(os.path.basename(parent.path_aws_key_fn))[0]
    flags["csv_path"] = parent.path_aws_credentials
    flags["ec2_file"] = parent.path_aws_ip_file
    flags["region"] = parent.parent.aws_util.region_name

    # Overwrite flag file
    flagfile_fn = os.path.join(
        parent.path_flags, parent.app_name_to_flagfile[parent.app_aws_clean]
    )
    dep_util.write_flagfile(flagfile_fn, flags)

    app_name = parent.app_aws_clean
    p_id = "terminate_cluster"
    run_process(parent, gb, app_name, flagfile_fn, p_id)


def get_workers(parent):
    """Finds workers in a LAN farm.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.

    Returns:
        list[str]: IPs of workers in the local farm.
    """
    if parent.parent.ui_flags.master == config.LOCALHOST:
        return []
    else:
        return parent.lan.scan()


def call_force_refreshing(parent, fun, *args):
    already_refreshing = parent.is_refreshing_data
    if not already_refreshing:
        parent.is_refreshing_data = True
    fun(*args)
    if not already_refreshing:
        parent.is_refreshing_data = False
