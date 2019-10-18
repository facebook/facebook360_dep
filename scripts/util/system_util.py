#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""General systems utility used across all scripts.

Defines functions for abstracting away command line interfaces and for defining a globally
accessible map of image paths. Any script that wishes to reference the path of an image
type (e.g. color or disparity) should do so through this utility rather than hard-coding it.

Example:
    To reference the image type path (as do many of the render scripts):

        >>> from system_util import image_type_paths
        >>> input_root = "/path/to/data"
        >>> color_path = os.path.join(input_root, image_type_paths["color"])

Attributes:
    image_type_paths (map[str, str]): Map from image type to its location in the standard
        structure. The paths are relative to the respective root an image type is defined
        for (e.g. "color" with respect to the input root and "disparity" to the output). For
        a full list of the input types, see: source/util/ImageTypes.h.
"""

import os
import re
import signal
import subprocess
import sys
import tarfile

from enum import Enum
from itertools import chain
from functools import reduce
from pathlib import Path

facebook360_dep_root = str(Path(os.path.abspath(__file__)).parents[2])


class OSType(Enum):
    """Enum for referencing operating systems.

    Attributes:
        LINUX (int): Any Linux distro.
        MAC (int): Any version of Mac OS X.
        WINDOWS (int): Any version of Windows.
    """

    WINDOWS = 1
    MAC = 2
    LINUX = 3


def get_os_type_local(platform=None):
    if not platform:
        platform = sys.platform
    if platform.startswith("win"):
        platform = "windows"

    if "darwin" in platform:
        os_type = OSType.MAC
    elif "windows" in platform:
        os_type = OSType.WINDOWS
    elif "linux" in platform:
        os_type = OSType.LINUX
    else:
        raise Exception("Unsupported OS!")
    return os_type


def _set_image_type_paths():
    """Creates up the image type paths map.

    Returns:
        (map[str, str]): Map from image type to its location in the standard structure.
            The paths are relative to the respective root an image type is defined
            for (e.g. "color" with respect to the input root and "disparity" to the output). For
            a full list of the input types, see: source/util/ImageTypes.h.
    """
    image_type_paths = {}
    image_type_paths["background_color"] = "background/color"
    image_type_paths["background_color_levels"] = "background/color_levels"
    image_type_paths["background_disp"] = "background/disparity"
    image_type_paths["background_disp_levels"] = "background/disparity_levels"
    image_type_paths["background_disp_upsample"] = "background/disparity_upsample"
    image_type_paths["bin"] = "bin"
    image_type_paths["color"] = "video/color"
    image_type_paths["color_levels"] = "video/color_levels"
    image_type_paths["confidence"] = "confidence"
    image_type_paths["cost"] = "cost"
    image_type_paths["disparity"] = "disparity"
    image_type_paths["disparity_upsample"] = "disparity_upsample"
    image_type_paths["disparity_levels"] = "disparity_levels"
    image_type_paths["disparity_time_filtered"] = "disparity_time_filtered"
    image_type_paths[
        "disparity_time_filtered_levels"
    ] = "disparity_time_filtered_levels"
    image_type_paths["exports"] = "exports"
    image_type_paths["exports_cubecolor"] = "exports/cubecolor"
    image_type_paths["exports_cubedisp"] = "exports/cubedisp"
    image_type_paths["exports_eqrcolor"] = "exports/eqrcolor"
    image_type_paths["exports_eqrdisp"] = "exports/eqrdisp"
    image_type_paths["exports_lr180"] = "exports/lr180"
    image_type_paths["exports_tb3dof"] = "exports/tb3dof"
    image_type_paths["exports_tbstereo"] = "exports/tbstereo"
    image_type_paths["foreground_masks"] = "video/foreground_masks"
    image_type_paths["foreground_masks_levels"] = "video/foreground_masks_levels"
    image_type_paths["fused"] = "fused"
    image_type_paths["mismatches"] = "mismatches"
    image_type_paths["video_bin"] = "video/bin"
    image_type_paths["video_disp"] = "video/disparity"
    image_type_paths["video_disp_levels"] = "video/disparity_levels"
    image_type_paths["video_fused"] = "video/fused"
    return image_type_paths


image_type_paths = _set_image_type_paths()


def get_flags(source):
    """Gets flags from a source file.

    Args:
        source (str): Path to the source file (could be any extension).

    Returns:
        list[dict[str, _]]: List of maps with keys "type", "name", "default", and "descr" for the
            respective fields corresponding to the flag.
    """
    flags = []
    _, ext = os.path.splitext(source)
    if ext == ".py":
        delim = "flags.DEFINE_"
    else:
        delim = "DEFINE_"

    split_comma_regex = r",(?=(?:[^\"]*\"[^\"]*\")*[^\"]*$)"
    cppflag_to_pygflag = {"int32": "integer", "double": "float", "bool": "boolean"}
    with open(source) as f:
        lines = f.readlines()

    flag_lines = "".join(lines).replace("\n", "").split(delim)[1:]
    for flag_line in flag_lines:
        flag_type, flag_def = flag_line.split("(", 1)
        flag_contents = re.compile(split_comma_regex).split(flag_def)

        NUM_FLAG_FIELDS = 3
        if len(flag_contents) < NUM_FLAG_FIELDS:
            continue

        flag = {}
        flag["type"] = flag_type
        flag["name"] = flag_contents[0].strip().replace('"', "")
        flag["default"] = flag_contents[1].strip()
        flag["descr"] = flag_contents[2].rsplit(")", 1)[0].strip()
        if flag["type"] in cppflag_to_pygflag:
            flag["type"] = cppflag_to_pygflag[flag["type"]]

        if flag["type"] == "boolean":
            flag["default"] = False if (flag["default"] == "false") else True
        elif flag["type"] == "integer":
            try:
                flag["default"] = int(flag["default"])
            except Exception:
                pass
        elif flag["type"] == "float":
            try:
                flag["default"] = float(flag["default"])
            except Exception:
                pass
        flags.append(flag)

    return flags


def get_flags_from_flagfile(flagfile_fn):
    flags = {}
    for line in open(flagfile_fn, "r"):
        m = re.findall("--(.*)=(.*)", line)
        if len(m) == 1 and len(m[0]) == 2:
            flags[m[0][0]] = m[0][1]
    return flags


def gen_args_from_flags(flags):
    """Constructs CLI arguments from flags, assuming the format in res/test/.

    Returns:
        str: Space-separated string of CLI arguments (e.g.
            "--example1 <X> --example2 <Y>")
    """
    flags_string = []
    for flag_name, flag_value in flags.items():
        flags_string.append(f"--{flag_name}={flag_value}")
    args_string = " ".join(flags_string)
    return args_string


def list_only_visible_files(src_dir):
    """Gets the visible files in a directory.

    Args:
        src_dir (str): Path to the directory.

    Returns:
        list[str]: Names of visible files (not full paths).
    """
    return [
        f
        for f in os.listdir(src_dir)
        if not f.startswith(".") and os.path.isfile(src_dir + "/" + f)
    ]


def list_only_visible_dirs(src_dir):
    """Gets the visible directories in a directory.

    Args:
        src_dir (str): Path to the directory.

    Returns:
        list[str]: Names of visible directories (not full paths).
    """
    return [
        f
        for f in os.listdir(src_dir)
        if not f.startswith(".") and os.path.isdir(src_dir + "/" + f)
    ]


def start_subprocess(name, cmd):
    """Synchronously runs a named command.

    Args:
        name (str): Process name.
        cmd (str): Command to execute.

    Returns:
        int: Return code of execution.
    """
    global current_process
    current_process = subprocess.Popen(cmd, shell=True)
    current_process.name = name
    current_process.communicate()
    return current_process.returncode


def list_only_visible_files_recursive(src_dir):
    """Recursively gets the visible files in a directory.

    Args:
        src_dir (str): Path to the directory.

    Returns:
        list[str]: Names of visible files (not full paths).
    """
    return reduce(
        lambda x, y: x + y,
        [
            list(
                map(
                    lambda x: root + "/" + x,
                    (f for f in files if not f.startswith(".")),
                )
            )
            for root, dirs, files in os.walk(src_dir)
        ],
    )


def intersect_lists(*args):
    return set(args[0]).intersection(*args)


def merge_lists(*args):
    return list(set(chain(*args)))


def extract_tar(filename, dst=None):
    """Extracts a tar file.

    Args:
        filename (str): Path to the tar file.
        dst (str, optional): Directory where extraction should produce results.

    Returns:
        str: Path to the extracted directory.
    """
    if dst is None:
        dst = os.getcwd()
    dir_name = os.path.splitext(filename)[0]
    extract_directory = os.path.join(dst, dir_name)
    tar_filename_path = os.path.join(os.getcwd(), filename)
    t = tarfile.open(tar_filename_path)
    t.extractall(dst)
    return extract_directory


def run_command(
    shell_string, run_silently=False, stream=True, run_async=False, file_fn=None
):
    """Run a shell command.

    Args:
        shell_string (str): Command to be run.
        run_silently (bool, optional): Whether or not to show stdout.
        stream (bool, optional): Whether or not to stream stdout. If run_silently is
            set to True, if the file_fn is not empty, or if running on a Windows host,
            this argument cannot be overrided (will be False).
        run_async (bool, optional): Whether or not to run command asynchronously. No output
            is returned if running async.
        file_fn (str, optional): Filename of where output should be saved.

    Returns:
        str: stdout from executing command.
    """
    if run_async:
        with open(os.devnull, "w") as FNULL:
            subprocess.Popen(
                shell_string, shell=True, stdout=FNULL, stderr=subprocess.STDOUT
            )
        return ""
    if run_silently or file_fn is not None or os.name == "nt":
        stream = False
    try:
        sh_fn = sh_stream if stream else sh_buffered
        if run_silently:
            with open(os.devnull, "w") as f:
                output = sh_fn(shell_string, file=f)
        else:
            if file_fn is not None:
                with open(file_fn, "w") as f:
                    output = sh_fn(shell_string, file=f)
            else:
                output = sh_fn(shell_string)
        return output
    except Exception:
        if not run_silently:
            print(f"Failed to run program: {shell_string}")
        raise


def sh_buffered(arg, file=sys.stdout, use_shell=True):
    """Run a shell command with buffered output.

    Args:
        arg (str): Command to run.
        file (file, optional): File handler to write stdout to.
        use_shell (bool, optional): Whether or not to execute in a shell.

    Returns:
        str: stdout from executing command.
    """
    if file:
        print(f"$ {arg}", file=file)
    try:
        output = subprocess.check_output(arg, shell=use_shell, stderr=file)
    except subprocess.CalledProcessError as e:
        error = e.output.decode("utf-8")
        if error:
            print(error, file=sys.stderr)
        raise
    output = output.decode("utf-8")
    if file:
        print(output, file=file)
    return output.rstrip()


def sh_stream(arg, file=sys.stdout, use_shell=None):
    """Run a shell command with streaming output.

    Args:
        arg (str): Command to run.
        file (file, optional): File handler to write stdout to.
        use_shell (bool, optional): Whether or not to execute in a shell.

    Returns:
        str: stdout from executing command.
    """
    if file:
        print(f"$ {arg}", file=file)
    process = subprocess.Popen(
        arg, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    outputs = []
    for c in iter(lambda: process.stdout.read(1), b""):
        decoded_c = c.decode("utf-8")
        sys.stdout.write(decoded_c)
        outputs.append(decoded_c)
    exit_code = process.poll()
    if exit_code != 0:
        raise subprocess.CalledProcessError(exit_code, arg)
    return "".join(outputs)


def get_catchable_signals():
    """Defines list of signals that can be caught (OS dependent).

    Returns:
        list[signal.signal]: Signals that can be caught.
    """
    catchable_sigs = [
        signal.SIGINT,
        signal.SIGILL,
        signal.SIGFPE,
        signal.SIGSEGV,
        signal.SIGTERM,
    ]
    if get_os_type_local() == OSType.WINDOWS:
        catchable_sigs.extend(
            [
                signal.SIGHUP,
                signal.SIGQUIT,
                signal.SIGTRAP,
                signal.SIGKILL,
                signal.SIGBUS,
                signal.SIGSYS,
                signal.SIGPIPE,
            ]
        )
    return catchable_sigs
