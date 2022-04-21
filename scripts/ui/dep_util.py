#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Utility functions for use across the UI.

The UI shares several common functions across its tabs and used in its internals,
such as:
- Write a flagfile
- Checking the existence of images

These functions amongst others are provided as utilities.
"""

import datetime
import glob
import json
import os
import re
import subprocess
import sys

import cv2
import numpy as np
from PyQt5 import QtCore, QtWidgets
from PyQt5.QtCore import Qt
from PyQt5.QtGui import QImage, QIntValidator, QPixmap
from PyQt5.QtWidgets import QFileDialog, QFileSystemModel, QHeaderView, QMessageBox

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "render"))
sys.path.append(os.path.join(dir_scripts, "util"))

import scripts.render.glog_check as glog
from scripts.util.system_util import get_flags_from_flagfile, list_only_visible_files


def browse_dir(caption="Select directory"):
    """Creates a dialog allowing directory selection.

    Args:
        caption (str, optional): Caption to prefix the displayed directory.

    Returns:
        QtWidgets.QFileDialog: Dialog with the selected directory.
    """
    return QFileDialog().getExistingDirectory(None, caption=caption)


def browse_file(caption="Select file", filter=""):
    """Creates a dialog allowing file selection.

    Args:
        caption (str, optional): Caption to prefix the displayed dialog.
        filter (str, optional): Regex filter to select a subset of files.

    Returns:
        QtWidgets.QFileDialog: Dialog with the selected directory.
    """
    file = QFileDialog().getOpenFileName(None, caption=caption, filter=filter)

    # Cancelling the dialog returns empty tuple
    return "" if file == ("", "") else file[0]


def setup_file_explorer(tree, path=""):
    """Creates the file explorer rooted at a particular path.

    Args:
        tree (QTreeView): Tree to be populated tied to the file explorer.
        path (str, optional): Path to the root of the project.

    Returns:
        tuple[QFileSystemModel, QTreeView]: File system UI element and tied populated
            file tree.
    """
    tree.setEnabled(os.path.isdir(path))
    model = QFileSystemModel()
    model.setRootPath(path)
    tree.setModel(model)
    tree.setRootIndex(model.index(path))

    # Resize column 0 (name) to content length, and stretch the rest to the size of the widget
    tree.header().setSectionResizeMode(0, QHeaderView.ResizeToContents)
    for i in range(1, model.columnCount()):
        tree.header().setSectionResizeMode(i, QHeaderView.Stretch)

    tree.setSortingEnabled(True)
    tree.sortByColumn(0, Qt.AscendingOrder)

    return model, tree


def convert_image_to_pixmap(image: np.ndarray):
    """Converts from cv2 image to a QT image format.

    Args:
        image (np.ndarray): cv2 read image.

    Returns:
        QtGui.QPixmap: Identical image in QT image format.
    """
    h, w, _ = image.shape
    bpl = 3 * w
    image = QImage(image.data, w, h, bpl, QImage.Format_RGB888).rgbSwapped()
    return QPixmap(image)


def preview_file(model, tree, frame, label, prefix=""):
    """Displays the file and its label on the UI.

    Args:
        model (QFileSystemModel): File explorer model.
        tree (QTreeView): Populated tree tied to the file explorer.
        frame (QtWidgets.QLabel): Image display UI element.
        label (QtWidgets.QLabel): Text label for the display UI element.
        prefix (str, optional): Prefix for label displayed.

    Returns:
        bool: Success of the image preview.
    """
    file_path = model.filePath(tree.currentIndex())
    if not os.path.isfile(file_path):
        return False
    img = cv2.imread(file_path, cv2.IMREAD_UNCHANGED)
    if img.dtype.char in np.typecodes["Float"]:
        img = (img * 255).astype("uint8")
    pixmap = convert_image_to_pixmap(img)
    if pixmap:
        label_geom = frame.frameGeometry()
        w = label_geom.width()
        h = label_geom.height()
        frame.setPixmap(pixmap.scaled(w, h, QtCore.Qt.KeepAspectRatio))
        label.setText(remove_prefix(file_path, prefix))
        return True
    return False


def grab_flag_value_from_file(flagfile_fn, flag_name):
    """Parse a flag value from a flagfile.

    Args:
        flagfile_fn (str): Path to the flagfile.
        flag_name (str): Name of the flag.

    Returns:
        _: Value corresponding to the flag. If the flag does not exist in
            the flagfile, an empty result will be given.
    """
    flags = get_flags_from_flagfile(flagfile_fn)
    return flags[flag_name] if flag_name in flags else ""


def extract_flag_from_string(str, var_name):
    """Parse a flag value from a string formatted as a flagfile is.

    Args:
        str (str): String expected to be in the format of a flagfile, i.e.
            --a=a.val --b=b.val
        flag_name (str): Name of the flag.

    Returns:
        _: Value corresponding to the flag. If the flag does not exist in
            the flagfile, an empty result will be given.
    """
    m = re.findall(f"--{var_name}=(.*)", str)
    return m[0] if len(m) == 1 else ""


def write_flagfile(flagfile_fn, flags):
    """Write a flags dictionary to disk in gflags format.

    Args:
        flagfile_fn (str): Path to the flagfile.
        flags (dict[str, _]): Flags and their corresponding values.
    """
    with open(flagfile_fn, "w") as f:
        for flag, value in flags.items():
            f.write(f"--{flag}={value}\n")
    f.close()


def update_flagfile(flagfile_fn, flag_name, flag_value):
    """Updates a flagfile on disk to a new value.

    Args:
        flagfile_fn (str): Path to the flagfile.
        flag_name (str): Name of the flag.
        flag_value (_): New value of the flag.
    """
    flags = get_flags_from_flagfile(flagfile_fn)
    flags[flag_name] = flag_value
    write_flagfile(flagfile_fn, flags)


def grab_variances_from_flagfile(flagfile):
    """Parses variances from a flagfile.

    Args:
        flagfile (str): Path to the flagfile.

    Returns:
        tuple[float, float]: Noise and threshold variances.
    """
    var_noise_floor = grab_flag_value_from_file(flagfile, "var_noise_floor")
    var_high_thresh = grab_flag_value_from_file(flagfile, "var_high_thresh")
    return var_noise_floor, var_high_thresh


def camel_to_snake(str):
    """Convert from a camel case string to snake case.

    Args:
        str (str): Camel case string to be converted.

    Returns:
        str: Snake case string with same contents.
    """
    s1 = re.sub("(.)([A-Z][a-z]+)", r"\1_\2", str)
    return re.sub("([a-z0-9])([A-Z])", r"\1_\2", s1).lower()


def get_first_file_path(dir, ext=".*"):
    """Gets the first file in a directory.

    Args:
        dir (str): Path to the local directory.
        ext (str, optional): Regex of the files to be filtered from the directory.

    Returns:
        str: Name of the first file matching the extension. An empty result
            is returned if no such file exists.
    """
    for filename in glob.iglob(f"{dir}/**/*{ext}", recursive=True):
        if os.path.isfile(filename):
            return filename
    return ""


def get_first_image_path(dir):
    """Gets the first file in a directory corresponding to an image.

    Args:
        dir (str): Path to the local directory.

    Returns:
        str: Name of the first image file. An empty result is returned
            if no such file exists.
    """
    for filename in glob.iglob(f"{dir}/**/*.*", recursive=True):
        if QPixmap(filename):
            return filename
    return ""


def get_level_image_path(dir, cam_id, frame, level=0):
    """Gets the path to a resized level image assuming the standard structure.

    Args:
        dir (str): Path to the local directory.
        cam_id (str): Name of the camera.
        frame (str): Name of the frame (0-padded, six digits).
        level (int, optional): Level of the resized image.
    Returns:
        str: Name of the image file matching the description. An empty result is returned
            if no such file exists.
    """
    for filename in glob.iglob(
        f"{dir}/level_{level}/{cam_id}/{frame}.*", recursive=True
    ):
        if QPixmap(filename):
            return filename
    return ""


def get_image_path(dir, cam_id, frame):
    """Gets the path to a full-size image assuming the standard structure.

    Args:
        dir (str): Path to the local directory.
        cam_id (str): Name of the camera.
        frame (str): Name of the frame (0-padded, six digits).
    Returns:
        str: Name of the image file matching the description. An empty result is returned
            if no such file exists.
    """
    for filename in glob.iglob(f"{dir}/{cam_id}/{frame}.*", recursive=True):
        if QPixmap(filename):
            return filename
    return ""


def listdir_nohidden(dir):
    """Gets the non-hidden files of a directory.

    Args:
        dir (str): Path to the local directory.

    Returns:
        list[str]: Filenames in the directory that are not hidden.
    """
    return [f for f in os.listdir(dir) if not f.startswith(".")]


def is_dir_empty(dir):
    """Whether or not there exist non-hidden files in a directory.

    Args:
        dir (str): Path to the local directory.

    Returns:
        bool: If any non-hidden file exists in the directory.
    """
    return len(listdir_nohidden(dir)) == 0


def remove_prefix(str, prefix):
    """Removes a prefix from a string.

    Args:
        str (str): Any string to be separated from a prefix.
        prefix (str): Part to be stripped off the front.

    Returns:
        str: String with the prefix removed. If the string doesn't start
            with the specified prefix, this is a no-op.
    """
    if str.startswith(prefix):
        return str[len(prefix) :]
    return str


def get_files_ext(dir, ext, needle=""):
    """Returns list of file in dir with given extension, and optionally
    filters entries that match needle.

    Args:
        dir (str): Path to the local directory.
        ext (TYPE): Extension of files to be returned.
        needle (str, optional): Regex filter to select a subset of files.

    Returns:
        list[str]: Filenames matching the criteria passed in.
    """
    return list(glob.iglob(f"{dir}/*{needle}*.{ext}", recursive=False))


def check_image_existence(image_dir, recursive=True):
    """Check if we have at least one image in the directory.

    Args:
        image_dir (str): Path to the local directory.
        recursive (bool, optional): Whether or not to check through all contained dirs.

    Returns:
        str: Extension of the first file encountered.
    """
    types = {".bin", ".exr", ".jpg", ".jpeg", ".pfm", ".png", ".tif", ".tiff"}
    paths = glob.iglob(f"{image_dir}/**", recursive=recursive)
    for p in paths:  # returns the type if it exists
        _, ext = os.path.splitext(p)
        if ext in types:
            return ext
    return ""


def get_stem(p):
    """Gets the name of a file without its extension.

    Args:
        p (str): File path.

    Returns:
        str: Basename of the file (i.e. without its extension).
    """
    return os.path.splitext(os.path.basename(p))[0]


def get_frame_list(dir):
    """Gets a list of frames by picking the stem of any file with an extension, recursively.

    Args:
        dir (str): Directory containing frame files.

    Returns:
        list[str]: List of frames.
    """
    return list({get_stem(f) for f in get_files_ext(dir, "*", "*/")})


def get_cam_ids_from_json(json_fn):
    """Finds camera names assuming the standard rig JSON format.

    Args:
        json_fn (str): Path to a valid rig json.

    Returns:
        list[str]: Names of cameras. If an invalid JSON is passed in, an empty
            result is returned.
    """
    ids = []
    if not os.path.isfile(json_fn):
        return ids
    rig = json.load(open(json_fn))
    if "cameras" not in rig:
        return ids
    for cam in rig["cameras"]:
        if "id" not in cam:
            continue
        ids.append(cam["id"])
    return ids


def get_dict_value_no_prefix(dict, key, prefix):
    """Finds the value in a dictionary with the prefix stripped.

    Args:
        dict (dict[str, str]): Any map of values.
        key (str): Key whose value is to be stripped.
        prefix (str): Prefix to remove from the key's value.

    Returns:
        str: Value with the prefix stripped.
    """
    return remove_prefix(dict.get(key, ""), prefix)


def get_qt_button_suffix(gb, suffix):
    """Gets the button associated with a tab and suffix.

    Args:
        gb (QtWidgets.QGroupBox): Group box for the tab.
        suffix (str): Text appended to UI element search query.

    Returns:
        QtWidgets.QPushButton: Button matching btn_{tag}_{suffix}.
    """
    t = remove_prefix(gb.objectName(), "gb_")
    return gb.findChild(QtWidgets.QPushButton, f"btn_{t}_{suffix}")


def get_qt_lineedit_suffix(gb, suffix):
    """Gets the line edit associated with a tab and suffix.

    Args:
        gb (QtWidgets.QGroupBox): Group box for the tab.
        suffix (str): Text appended to UI element search query.

    Returns:
        QtWidgets.QLineEdit: LineEdit matching val_{tag}_{suffix}.
    """
    t = remove_prefix(gb.objectName(), "gb_")
    return gb.findChild(QtWidgets.QLineEdit, f"val_{t}_{suffix}")


def get_qt_dropdown_suffix(gb, suffix):
    """Gets the dropdown associated with a tab and suffix.

    Args:
        gb (QtWidgets.QGroupBox): Group box for the tab.
        suffix (str): Text appended to UI element search query.

    Returns:
        QtWidgets.QComboBox: Dropdown matching dd_{tag}_{suffix}.
    """
    t = remove_prefix(gb.objectName(), "gb_")
    return gb.findChild(QtWidgets.QComboBox, f"dd_{t}_{suffix}")


def get_qt_checkbox_suffix(gb, suffix):
    """Gets the checkbox associated with a tab and suffix.

    Args:
        gb (QtWidgets.QGroupBox): Group box for the tab.
        suffix (str): Text appended to UI element search query.

    Returns:
        QtWidgets.QCheckBox: Dropdown matching cb_{suffix}.
    """
    return gb.findChild(QtWidgets.QCheckBox, f"cb_{suffix}")


def get_qt_textedit_suffix(gb, suffix):
    """Gets the textedit associated with a tab and suffix.

    Args:
        gb (QtWidgets.QGroupBox): Group box for the tab.
        suffix (str): Text appended to UI element search query.

    Returns:
        QtWidgets.QPlainTextEdit: Textedit matching text_{tag}_{suffix}.
    """
    t = remove_prefix(gb.objectName(), "gb_")
    return gb.findChild(QtWidgets.QPlainTextEdit, f"text_{t}_{suffix}")


def update_qt_dropdown(dd, value, add_if_missing=False):
    """Adds a new value to a dropdown if requested or invalidates request.

    Args:
        dd (QtWidgets.QComboBox): Dropdown UI element.
        value (str): Value being queried from the dropdown.
        add_if_missing (bool, optional): Whether or not to add the value to the
            dropdown if it does not exist (True) or reject the request (False).

    Returns:
        str: Error message (if any) to display.
    """
    if not value:
        return "empty value"

    i = dd.findText(value, QtCore.Qt.MatchFixedString)
    if i >= 0:  # We do not do UI verification on AWS
        dd.setCurrentIndex(i)
        return ""
    elif add_if_missing:
        dd.addItem(value)
        dd.setCurrentIndex(dd.findText(value, QtCore.Qt.MatchFixedString))
        return ""
    else:
        error = f"Invalid value {value}"
        if dd.count() > 0:
            items_str = "\n".join([dd.itemText(i) for i in range(dd.count())])
            error += f". Choose from\n{items_str}"
        return error


def update_qt_dropdown_from_flags(flags, key, prefix, dd):
    """Updates values in dropdowns from a flagfile with prefixes stripped.

    Args:
        flags (dict[str, _]): Flags and their corresponding values.
        key (str): Key whose value is to be updated.
        prefix (str): Prefix to remove from the key's value.
        dd (QtWidgets.QComboBox): Dropdown UI element.

    Returns:
        str: Error message (if any) to display.
    """
    val_flag = flags.get(key, "")
    if not val_flag:
        return ""

    if val_flag.startswith(prefix) and not os.path.exists(val_flag):
        return f"Could not update {key}: non-existing value {val_flag}"

    error = update_qt_dropdown(dd, remove_prefix(val_flag, prefix))
    if error != "":
        return f"Could not update {key} with value {val_flag}: {error}"

    return ""


def update_qt_lineedit_from_flags(flags, key, prefix, le):
    """Updates values in LineEdits from a flagfile with prefixes stripped.

    Args:
        flags (dict[str, _]): Flags and their corresponding values.
        key (str): Key whose value is to be updated.
        prefix (str): Prefix to remove from the key's value.
        le (QtWidgets.QLineEdit): LineEdit UI element.
    """
    value = get_dict_value_no_prefix(flags, key, prefix)
    if value:
        le.setText(value)
        le.setCursorPosition(0)


def update_qt_checkbox_from_flags(flags, key, prefix, cb):
    """Updates values in checkboxes from a flagfile with prefixes stripped.

    Args:
        flags (dict[str, _]): Flags and their corresponding values.
        key (str): Key whose value is to be updated.
        prefix (str): Prefix to remove from the key's value.
        cb (QtWidgets.QCheckBox): Checkbox UI element.

    Returns:
        str: Error message (if any) to display.
    """
    value = get_dict_value_no_prefix(flags, key, prefix)
    error = ""
    if value:
        value = value.lower()
        if value == "true":
            cb.setChecked(True)
        elif value == "false":
            cb.setChecked(False)
        else:
            error = f"Invalid value for {key}: {value}. Use true or false."
    return error


def update_qt_label_from_flags(flags, key, prefix, label):
    """Updates values in labels from a flagfile with prefixes stripped.

    Args:
        flags (dict[str, _]): Flags and their corresponding values.
        key (str): Key whose value is to be updated.
        prefix (str): Prefix to remove from the key's value.
        label (QtWidgets.QLabel): Label UI element.
    """
    value = get_dict_value_no_prefix(flags, key, prefix)
    if value:
        label.setText(value)


def setup_read_only_checkbox(checkbox):
    """Creates checkbox that cannot be modified.

    Args:
        checkbox (QtWidgets.QCheckBox): Checkbox UI element.
    """
    checkbox.setAttribute(Qt.WA_TransparentForMouseEvents, True)
    checkbox.setFocusPolicy(Qt.NoFocus)


def disconnect(signal):
    """Disconnect all signals, ignore if not connected.

    Args:
        signal (signal.signal): Computer signals sent to this process.
    """
    while True:
        try:
            signal.disconnect()
        except TypeError:
            break


def scale_image(image, scale):
    """Resizes an image retaining its aspect ratio.

    Args:
        image (np.array[_]): Any image loaded through cv2.
        scale (float): Ratio to resize image.

    Returns:
        np.array[_]: Scaled image (resized with inter-area interpolation).
    """
    h, w, _ = image.shape
    dims = (round(scale * w), round(scale * h))
    return cv2.resize(image, dims, interpolation=cv2.INTER_AREA)


def load_image_resized(filename, width):
    """Resizes an image when loading it from disk, retaining its aspect ratio.

    Args:
        filename (str): Path to the image on disk.
        width (int): Desired width of the image.

    Returns:
        np.array[_]: Scaled image (resized with inter-area interpolation).
    """
    image = cv2.imread(filename, cv2.IMREAD_UNCHANGED)
    _, w, _ = image.shape
    scale = int(width) / w
    return scale_image(image, scale)


def convert_to_float(image):
    """Converts the image to an equivalent float representation.

    Args:
        image (np.array[_]): Any valid cv2 image.

    Returns:
        np.array[_]: Floating point equivalent representation of the input image. The
            original image is returned if it is already given in floating point.
    """
    if image.dtype.char in np.typecodes["Float"]:
        return image
    return image / np.iinfo(image.dtype).max


def run_subprocess(cmd):
    """Runs a process synchronously in the terminal.

    Args:
        cmd (str): Command to be executed.

    Returns:
        str: Output of the executed command. An error is returned if it failed.
    """
    try:
        sp = subprocess.run(cmd, stdout=subprocess.PIPE)
    except subprocess.CalledProcessError as e:
        return f"Error calling {' '.join(cmd)}: {e.output}"
    return sp.stdout.decode("utf-8").splitlines()


def animate_resize(object, size_start, size_end, duration_ms=250):
    """Create resize animation on the UI.

    Args:
        object (App(QDialog)): UI element to be resized.
        size_start (QSize): QT tuple of the window size at start.
        size_end (QSize): QT tuple of the window size after resizing.
        duration_ms (int, optional): Length of the animation in ms.
    """
    object.animation = QtCore.QPropertyAnimation(object, b"size")
    object.animation.setDuration(duration_ms)
    object.animation.setStartValue(size_start)
    object.animation.setEndValue(size_end)
    object.animation.start()


def switch_objects_prefix(parent, prefixes, state):
    """Toggle objects starting with any number of prefixes.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        prefixes (list[str]): Prefixes to be used for filtering elements.
        state (bool): Whether or not the elements should be enabled.
    """
    for o in parent.findChildren(QtWidgets.QWidget):
        if o.objectName().startswith(tuple(prefixes)):
            o.setEnabled(state)


def get_tooltip(app_path):
    """Creates the contents of a help tooltip.

    Args:
        app_path (str): Path to the application binary.

    Returns:
        str: Help displayed when running --help on the binary.
    """
    if not os.path.isfile(app_path):
        return ""
    else:
        help_lines = run_subprocess([app_path, "--help"])
        return "\n".join(help_lines)


def populate_dropdown(dd, ps, prefix=""):
    """Populates the dropdown with an appended prefix.

    Args:
        dd (QtWidgets.QComboBox): Dropdown UI element.
        ps (list[str]): Contents of the dropdown.
        prefix (str, optional): Prefix for all the list elements.
    """
    dd.clear()
    for i, p in enumerate(ps):
        dd.addItem(remove_prefix(p, prefix))
        if dd.objectName().endswith("_farm_workers"):
            dd.model().item(i).setCheckable(True)


def switch_tab(tab_widget, suffix):
    """Switches the active tab.

    Args:
        tab_widget (QtWidgets.QTabWidget): Widget for the tab being switched to.
        suffix (str): Filters UI elements by those ending with this string.
    """
    for i in range(tab_widget.count()):
        if tab_widget.widget(i).objectName().endswith(suffix):
            tab_widget.setCurrentIndex(i)
            return


def set_tab_enabled(tab_widget, suffix, enabled):
    """Toggles the interactivity of a tab.

    Args:
        tab_widget (QtWidgets.QTabWidget): Widget for the tab being made active.
        suffix (str): Filters UI elements by those ending with this string.
        enabled (bool): Whether or not it is enabled.
    """
    for i in range(tab_widget.count()):
        if tab_widget.widget(i).objectName().endswith(suffix):
            tab_widget.setTabEnabled(i, enabled)
            return


def is_tab_enabled(tab_widget, suffix):
    """Determines the interactivity of a tab.

    Args:
        tab_widget (QtWidgets.QTabWidget): Widget for the tab being made active.
        suffix (str): Filters UI elements by those ending with this string.

    Returns:
        bool: Whether or not the tab is interactable.
    """
    for i in range(tab_widget.count()):
        if tab_widget.widget(i).objectName().endswith(suffix):
            return tab_widget.isTabEnabled(i)
    return False


def get_timestamp(format="%Y%m%d%H%M%S"):
    """Formatted current time.

    Args:
        format (str, optional): Format to parse the time.

    Returns:
        str: Current time in the specified format.
    """
    return datetime.datetime.now().strftime(format)


def is_host_up(hostname):
    """Whether or not a host is up. Useful only in LAN farms.

    Args:
        hostname (str): IP of the host.

    Returns:
        bool: Whether the host is pingable.
    """
    return os.system(f"ping -c 1 {hostname}") == 0


def set_integer_validator(qt_element):
    """Create validator to ensure the input is an int.

    Args:
        qt_element (QtElement): Any element with input.
    """
    qt_element.setValidator(QIntValidator())


def popup_message(parent, text, title="", icon=QMessageBox.Information):
    """Creates a pop-up with the desired text.

    Args:
        parent (App(QDialog)): Object corresponding to the parent UI element.
        text (str): Text displayed in the message.
        title (str, optional): Text in the message header.
        icon (QMessageBox.Icon, optional): Icon in the message header.
    """
    msgBox = QMessageBox(parent)
    msgBox.setIcon(icon)
    msgBox.setWindowTitle(title)
    msgBox.setText(text)
    msgBox.exec()


def get_local_frame_width(dir):
    """Finds the width of an image.

    Args:
        dir (str): Path to a local directory.

    Returns:
        int: Camera image width.
    """
    if not os.path.exists(dir):
        return -1
    frames = list_only_visible_files(dir)
    if not frames:
        return -1
    img = cv2.imread(os.path.join(dir, frames[0]), cv2.IMREAD_UNCHANGED)
    return img.shape[1]


def set_full_size_widths(parent):
    camera_ref = parent.cameras[0]
    for t in ["bg", "video"]:
        p = getattr(parent, f"path_{t}_color", None)
        p_local = os.path.join(p, camera_ref)
        full_size_width = get_local_frame_width(p_local)
        if full_size_width < 0:
            full_size_width = parent.rig_width
        setattr(parent, f"{t}_full_size_width", full_size_width)
        print(glog.green(f"Local {t} full-size width: {full_size_width}"))
