#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Utility functions for verifying inputs for the UI.

The UI requires some files to be present on execution, such as:
- An uncalibrated rig
- Color images (either full-size or resized levels)

Functions for checking the presence of these (in the cases of both
AWS and local renders) are provided amongst other similar checks.
"""

import glob
import json
import os
import posixpath
import sys
import tarfile

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "render"))
sys.path.append(os.path.join(dir_scripts, "util"))

import scripts.render.glog_check as glog
from scripts.render.resize import resize_frames
from scripts.util.system_util import list_only_visible_files, merge_lists

global verbose


def log(msg):
    """Prints logs if in verbose mode.

    Args:
        msg (str): Message to be displayed.
    """
    if verbose:
        print(msg)


def get_stem(p):
    """Gets the name of a file without its extension.

    Args:
        p (str): File path.

    Returns:
        str: Basename of the file (i.e. without its extension).
    """
    return os.path.splitext(os.path.basename(p))[0]


def check_image_existence(image_dir, sample_frame_name, recursive=True):
    """Check if we have at least one image in the directory.

    Args:
        image_dir (str): Path to the local directory.
        sample_frame_name (str): Name of the frame being checked. If None is passed in,
            the existence of any image file is checked.
        recursive (bool, optional): Whether or not to check through all contained dirs.

    Returns:
        str: Extension of the first file encountered.
    """
    types = {".bin", ".exr", ".jpg", ".jpeg", ".pfm", ".png", ".tif", ".tiff"}
    paths = glob.iglob(f"{image_dir}/**", recursive=recursive)
    for p in paths:  # returns the type if it exists
        _, ext = os.path.splitext(p)
        if ext in types:
            frame_name = get_stem(p)
            if sample_frame_name is None or frame_name == sample_frame_name:
                return ext
    return ""


def get_rigs(parent):
    """Gets filenames corresponding to the project rigs.

    Args:
        parent: class instance

    Returns:
        list[str]: Rig filenames (assumed to be named *.json).
    """
    log(glog.green(f"Looking for rigs in {parent.path_rigs}..."))
    ps = list(glob.iglob(f"{parent.path_rigs}/**.json", recursive=False))
    if len(ps) == 0:
        log(glog.yellow(f"No rigs found in {parent.path_rigs}"))
    return ps


def get_rigs_s3(parent):
    """Downloads a calibrated rig from S3 if it exists.

    Args:
        parent: class instance

    Returns:
        str: Local filename of the calibrated rig.
    """
    path_rigs_s3 = posixpath.join(parent.project_root, "rigs")

    log(glog.green(f"Downloading rigs from {path_rigs_s3}..."))
    parent.aws_util.s3_cp(
        f"{path_rigs_s3}/", f"{parent.path_rigs}/", run_silently=not verbose
    )

    # If there are rigs in S3 they should now be downloaded to local directory
    return get_rigs(parent)


def have_data(parent, dirs, is_levels=False):
    """Checks that the directories expected to have input images are non-empty.

    Args:
        parent: class instance
        dirs (list[str]): Directories to be verified.
        is_levels (bool, optional): Whether or not the levels are being used
            instead of full-size images (usually only relevant in AWS renders).

    Returns:
        dict[str, bool]: Map of each directory and whether or not it is non-empty.
    """
    have_data = {}
    for dir in dirs:
        dir_key = dir
        if is_levels:
            # We need level 0 for thresholds
            dir = posixpath.join(dir, "level_0")
        log(glog.green(f"Looking for valid images in {dir}..."))
        sample_frame = None
        if parent.s3_sample_frame and (
            dir == parent.path_video_color
            or dir.startswith(parent.path_video_color_levels)
        ):
            sample_frame = parent.s3_sample_frame

        have_data[dir_key] = check_image_existence(dir, sample_frame) != ""
        if not have_data[dir_key]:
            log(glog.yellow(f"No valid images found in {dir}"))
    return have_data


def have_color(parent, is_levels=False):
    """Checks that the color directories have images.

    Args:
        parent: class instance
        is_levels (bool, optional): Whether or not to use full-size (False) or
            level_0 images (True).

    Returns:
        dict[str, bool]: Map of color directories and whether or not it is non-empty.
    """
    dirs = parent.paths_color_levels if is_levels else parent.paths_color
    return have_data(parent, dirs, is_levels)


def have_disparity(parent, is_levels=False):
    """Checks that disparity directories have images.

    Args:
        parent: class instance
        is_levels (bool, optional): Whether or not to use full-size (False) or
            level_0 images (True).

    Returns:
        dict[str, bool]: Map of disparity directory and whether or not it is non-empty.
    """
    dirs = parent.paths_disparity_levels if is_levels else parent.paths_disparity
    return have_data(parent, dirs, is_levels)


def unpack_tars(parent, dir_local, recursive=True):
    needle = "*/" if recursive else "*"
    tar_files = list(glob.iglob(f"{dir_local}/*{needle}*.tar", recursive=False))
    for tar_file in tar_files:
        log(glog.green(f"Unpacking {tar_file}..."))
        t = tarfile.open(tar_file)
        t.extractall(os.path.dirname(tar_file))
        t.close()
        os.remove(tar_file)


def download_frame_s3(parent, dir_s3, dir_local, frames_s3_names, is_levels=False):
    frame_s3_name_first = frames_s3_names[0]
    s3_sample_frame = parent.s3_sample_frame
    if s3_sample_frame:
        if s3_sample_frame in frames_s3_names:
            frame_s3_name = s3_sample_frame
        else:
            log(glog.yellow(f"Cannot find {s3_sample_frame} in {frames_s3_names}"))
            return
    else:
        frame_s3_name = frame_s3_name_first

    fn = f"{frame_s3_name}.tar"
    if is_levels:
        recursive = True
        levels = parent.aws_util.s3_ls(dir_s3, run_silently=not verbose)
        levels = list(filter(None, levels))  # removes empty results from ls
        t = "levels"
        srcs = [posixpath.join(dir_s3, level, fn) for level in levels]
        dsts = [posixpath.join(dir_local, level, fn) for level in levels]
    else:
        recursive = False
        t = "full-size"
        srcs = [posixpath.join(dir_s3, fn)]
        dsts = [posixpath.join(dir_local, fn)]
    exclude = None
    include = None

    print(glog.green(f"Downloading {fn} {t} from {dir_s3}..."))
    for src, dst in zip(srcs, dsts):
        parent.aws_util.s3_cp(
            src, dst, exclude, include, recursive=False, run_silently=not verbose
        )
    unpack_tars(parent, dir_local, recursive)


def get_data_s3(parent, have_data_in, is_levels=False, is_disp=False):
    """Checks if we have color images in S3, and downloads a sample frame.

    Args:
        parent: class instance
        have_data_in dict[str, bool]: Map of directories and whether or not they
            are non-empty.
        is_levels (bool, optional): Whether or not to use full-size (False) or
            level_0 images (True).
        is_disp (bool, optional): Whether to download disparity images (True) or
            color (False).

    Returns:
        dict[str, bool]: Map of local data directory and whether or not it is non-empty.

    Raises:
        Exception: If attempting to get data from S3 on a non-AWS render.
    """
    if not parent.is_aws:
        raise Exception(f"Not an S3 project: {parent.project_root}")
    path_project_s3 = parent.project_root
    for dir_local, has_data in have_data_in.items():
        if has_data:
            # We already have local color for this type
            continue
        dir_s3 = dir_local.replace(parent.path_project, path_project_s3)
        dir_s3_frames = posixpath.join(dir_s3, "level_0") if is_levels else dir_s3
        frames_s3_names = get_s3_frame_names(parent, dir_s3_frames)
        if len(frames_s3_names) == 0:
            continue

        download_frame_s3(parent, dir_s3, dir_local, frames_s3_names, is_levels)

    # If there are frames in S3 one of them should now be downloaded to local directory
    return (
        have_disparity(parent, is_levels) if is_disp else have_color(parent, is_levels)
    )


def resize_local_frame(parent, dir_full, dir_level, rig_ref):
    glog.check(
        len(parent.cameras) > 0,
        f"No cameras found. Cannot resize local frame {dir_full}",
    )
    dir_cam = posixpath.join(dir_full, parent.cameras[0])
    frames = list_only_visible_files(dir_cam)
    glog.check_gt(len(frames), 0, f"No frames found in {dir_cam}")
    if parent.s3_sample_frame and dir_full == parent.path_video_color:
        frame_name = parent.s3_sample_frame
    else:
        frame_name, _ = os.path.splitext(sorted(frames)[0])
    frame_num = int(frame_name)

    log(glog.green(f"Resizing full-size frame {frame_name} in {dir_full}..."))
    with open(rig_ref, "r") as f:
        rig = json.load(f)
        resize_frames(dir_full, dir_level, rig, frame_num, frame_num)


def get_cameras(parent, rig_fn):
    """Finds the camera names in the captured project.

    Args:
        parent: class instance
        rig_fn (str): Path to the rig. If no path is provided, cameras are determined
            by the directory structure.

    Returns:
        list[str]: Names of the cameras.
    """
    if rig_fn:
        with open(rig_fn, "r") as f:
            rig = json.load(f)
            return [camera["id"] for camera in rig["cameras"]]
    else:
        for p in parent.paths_color + parent.paths_color_levels:
            if p in parent.paths_color_levels:
                p = posixpath.join(p, "level_0")
            cameras = list_only_visible_files(p)
            if len(cameras) > 0:
                return cameras
    return []


def get_rig_width(parent, rig_fn):
    """Finds the camera image width.

    Args:
        parent: class instance
        rig_fn (str): Path to the rig

    Returns:
        int: Camera image width.
    """
    with open(rig_fn, "r") as f:
        rig = json.load(f)
        return int(rig["cameras"][0]["resolution"][0])


def is_frame(name):
    """Whether or not the name is a valid (expected) frame name.

    Args:
        name (str): Frame name to be tested.

    Returns:
        bool: Whether or not this is valid (i.e. if it can be cast as an int).
    """
    try:
        # Verify that the name corresponds to a number
        int(get_stem(name))
    except Exception:
        return False
    return True


def get_local_frame_names(dir):
    """Finds all the frames in a directory.

    Args:
        dir (str): Path to a local directory.

    Returns:
        list[str]: Sorted list of frame names in the directory. If an invalid directory
            is passed in, an empty result is returned.
    """
    if os.path.isdir(dir):
        log(glog.green(f"Looking for local frames in {dir}"))
        frames = list_only_visible_files(dir)
        return [get_stem(f) for f in frames if is_frame(f)]
    return []


def get_s3_frame_names(parent, dir):
    """Finds all the frames in an S3 directory.

    Args:
        parent: class instance
        dir (str): Path to the S3 directory being scanned.

    Returns:
        list[str]: Sorted list of frame names in the directory.
    """
    if not dir.startswith("s3://"):
        path_project_s3 = parent.project_root
        dir_s3 = dir.replace(parent.path_project, path_project_s3)
    else:
        dir_s3 = dir
    log(glog.green(f"Looking for S3 frames in {dir_s3}"))
    frames = parent.aws_util.s3_ls(dir_s3, run_silently=not verbose)
    frames = [f for f in frames if f.endswith(".tar")]
    return sorted(get_stem(f) for f in frames if is_frame(f))


def get_frame_names(parent, dir, is_cache=True):
    """Finds all the frames in a local directory.

    Args:
        parent: class instance
        dir (str): Path to the local directory being scanned.
        is_cache (bool): Whether or not to check the cache for getting frame names.

    Returns:
        list[str]: Sorted list of frame names in the directory.
    """
    if parent.is_aws:
        if is_cache:
            frame_names = get_local_frame_names(dir)
        else:
            frame_names = get_s3_frame_names(parent, dir)
    else:
        frame_names = get_local_frame_names(dir)
    return sorted(f for f in frame_names if is_frame(f))


def print_frame_range(parent, suffix):
    """Displays frame range.

    Args:
        parent: class instance
        suffix (str): Prefixed text to display before the frames.
    """
    ff = getattr(parent, f"frames_{suffix}", None)
    if not ff:
        return
    elif len(ff) == 0:
        frame_range = ""
    elif len(ff) == 1:
        frame_range = f"{ff[0]}"
    else:
        frame_range = f"{ff[0]}, {ff[-1]}"
    log(glog.green(f"Frames ({suffix}): [{frame_range}]"))


def download_s3_disparities(parent):
    """Download disparities (both full size and level) from the S3 bucket.

    Args:
        parent: class instance
    """
    have_disp = have_disparity(parent)
    if not all(have_disp.values()):
        get_data_s3(parent, have_disp)

    have_level_disp = have_disparity(parent, is_levels=True)
    if not all(have_level_disp.values()):
        get_data_s3(parent, have_level_disp, is_levels=True)


def update_frame_names(
    parent, data_types=None, image_types=None, update_local=True, update_s3=True
):
    """Updates frame names for given data types

    Args:
        parent: class instance
        data_types (list[str]): List of data types.
        image_types (list[str]): List of image types.
    """
    global verbose
    verbose = parent.verbose

    log(glog.green("Getting frame names..."))
    glog.check(len(parent.cameras) > 0, "No cameras found!")
    camera_ref = parent.cameras[0]
    if not data_types:
        data_types = ["bg", "video"]
    if not image_types:
        image_types = ["color", "color_levels", "disparity", "disparity_levels", "bin"]
    for t in data_types:
        for d in image_types:
            if t == "bg" and d == "bin":
                continue
            suffix = f"{t}_{d}" if d != "bin" else d
            p = getattr(parent, f"path_{suffix}", None)
            if "_levels" in d:
                p = posixpath.join(p, "level_0")

            if update_local:
                p_local = posixpath.join(p, camera_ref)
                setattr(
                    parent,
                    f"frames_{suffix}",
                    get_frame_names(parent, p_local, is_cache=True),
                )
                print_frame_range(parent, suffix)
            if update_s3 and parent.is_aws:
                # Cached frames are eventually synced to S3, so any frame in the
                # cache should be added to the S3 frames
                frames_s3 = get_frame_names(parent, p, is_cache=False)
                frames_cache = getattr(parent, f"frames_{suffix}", None)
                frames_s3 = sorted(merge_lists(frames_s3, frames_cache))
                setattr(parent, f"frames_{suffix}_s3", frames_s3)
                print_frame_range(parent, f"{suffix}_s3")


def verify(parent, save_frame_ranges=True):
    """Performs all validation on data. Warnings are displayed if an unexpected structure
    is encountered.
    """
    global verbose
    verbose = parent.verbose
    if not verbose:
        print(glog.green("\nVerifying data (may take a few seconds)..."))

    # Look for a rig
    rig_fns = get_rigs(parent)
    if not rig_fns and parent.is_aws:  # no local rigs, check S3
        rig_fns = get_rigs_s3(parent)
    glog.check(len(rig_fns) > 0, "Cannot launch UI without any rig")
    rig_ref = rig_fns[0]
    parent.cameras = get_cameras(parent, rig_ref)
    parent.rig_width = get_rig_width(parent, rig_ref)

    # We need full-size images if we want to (re-)calibrate
    have_full_color = have_color(parent)
    if not all(have_full_color.values()) and parent.is_aws:  # no local color, check S3
        if parent.s3_ignore_fullsize_color:
            log(glog.yellow(f"Ignoring full-size color image downloads from S3..."))
        else:
            have_full_color = get_data_s3(parent, have_full_color)

    # We have a rig, but we need color levels to run thresholds for depth
    # estimation
    have_level_color = have_color(parent, is_levels=True)

    if not all(have_level_color.values()) and parent.is_aws:  # no local color, check S3
        have_level_color = get_data_s3(parent, have_level_color, is_levels=True)

    # Check what color types have full-size but not level color
    map_level_full = dict(zip(have_level_color, have_full_color))
    for dir_level, has_level_color in have_level_color.items():
        if not has_level_color:
            log(glog.yellow(f"No level colors in {dir_level}"))
            dir_full = map_level_full[dir_level]
            if not have_full_color[dir_full]:
                log(
                    glog.yellow(
                        f"No full-size colors in {dir_full}. Cannot create levels"
                    )
                )
                continue
            else:
                resize_local_frame(parent, dir_full, dir_level, rig_ref)

    have_level_color = have_color(parent, is_levels=True)
    if not have_level_color[parent.path_bg_color_levels]:
        log(glog.yellow(f"No background frames found. Cannot render background"))

    if not have_level_color[parent.path_video_color_levels]:
        log(glog.yellow(f"No video frames found. Cannot render video"))

    if not any(have_level_color.values()) and not any(have_full_color.values()):
        glog.check(False, f"No colors. Cannot calibrate")

    # Download disparities from S3
    if parent.is_aws:
        download_s3_disparities(parent)

    # Get frames for color, color levels, disparity (background and video)
    if save_frame_ranges:
        update_frame_names(parent)


def make_path_dirs(parent):
    """Create directories expected on the specified tab.

    Args:
        parent: class instance
    """
    for attr in dir(parent):
        if attr.startswith("path_"):
            p = getattr(parent, attr)
            if p:
                ext = os.path.splitext(p)[-1].lower()
                if not ext:  # ignore paths that look like files
                    os.makedirs(p, exist_ok=True)


def set_default_top_level_paths(parent, mkdirs=False):
    """Defines class referenceable attributes for paths on the specified tab.

    Args:
        parent: class instance
        mkdirs (bool, optional): Whether or not to make the defined directories.
    """
    if "path_project" in dir(parent):
        project = parent.path_project
    else:
        project = parent.parent.path_project
    for d in [
        "aws",
        "background",
        "calibration",
        "flags",
        "ipc",
        "logs",
        "rigs",
        "video",
    ]:
        setattr(parent, f"path_{d}", posixpath.join(project, d))
    for d in ["color", "color_levels", "disparity", "disparity_levels"]:
        setattr(parent, f"path_bg_{d}", posixpath.join(parent.path_background, d))
        setattr(parent, f"path_video_{d}", posixpath.join(parent.path_video, d))
    parent.path_fg_masks = posixpath.join(parent.path_video, "foreground_masks")
    parent.path_fg_masks_levels = posixpath.join(
        parent.path_video, "foreground_masks_levels"
    )
    for d in ["bin", "export", "fused"]:
        setattr(parent, f"path_{d}", posixpath.join(parent.path_video, d))

    parent.path_aws_key_fn = posixpath.join(parent.path_aws, "key.pem")
    parent.path_aws_credentials = posixpath.join(parent.path_aws, "credentials.csv")
    parent.path_aws_ip_file = posixpath.join(parent.path_aws, "ec2_info.txt")

    # So we know where to look for frames
    parent.paths_color = [parent.path_bg_color, parent.path_video_color]
    parent.paths_color_levels = [
        parent.path_bg_color_levels,
        parent.path_video_color_levels,
    ]
    parent.paths_first = parent.paths_color_levels
    parent.paths_last = parent.paths_first
    parent.paths_disparity = [parent.path_bg_disparity, parent.path_video_disparity]
    parent.paths_disparity_levels = [
        parent.path_bg_disparity_levels,
        parent.path_video_disparity_levels,
    ]
    parent.path_frame_bg = parent.path_bg_color_levels
    parent.path_frame_fg = parent.path_video_color_levels
    parent.path_first = parent.path_frame_fg
    parent.path_last = parent.path_frame_fg

    parent.is_farm = project.startswith("s3://")

    if "flagfile_basename" in dir(parent) and parent.flagfile_basename:
        parent.flagfile_fn = posixpath.join(parent.path_flags, parent.flagfile_basename)

    parent.output_dirs = []  # should be populated by every tab
    parent.overwrite_output = False

    if mkdirs:
        make_path_dirs(parent)
