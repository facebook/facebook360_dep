#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Resize image to fixed pyramid level sizes.

Depth estimation assumes particular sizes for the levels, which are fixed ahead of time in
config.py. This resizes a full size image to those expected fixed sizes. Resizing can
be run as an independent script or as part of a pipeline.

Example:
    To independently run resizing:

        $ python resize.py \
          --src_dir=/path/to/resized \
          --dst_dir=/path/to/data \
          --first=001700 \
          --last=001700 \
          --rig=/path/to/data/rig.json

Attributes:
    FLAGS (absl.flags._flagvalues.FlagValues): Globally defined flags for resize.py.
"""


import glob
import json
import multiprocessing as mp
import os
import sys

import cv2
import imageio
from absl import app, flags

import config
from network import get_frame_name, get_sample_file

FLAGS = flags.FLAGS
imageio.plugins.freeimage.download()  # allows PFM file I/O


def get_frame_path(src_dir, camera, frame):
    sample_file = get_sample_file(os.path.join(src_dir, camera))
    _, img_ext = os.path.splitext(sample_file)
    frame_fn = f"{frame}{img_ext}"
    return os.path.join(src_dir, camera, frame_fn)


def resize_camera(src_dir, dst_dir, camera, rig_resolution, frame, threshold):
    """Resizes a frame for a given camera to the appropriate pyramid level sizes.
    Files are saved in level_0/[camera], ..., level_9/[camera] in the destination directory.

    Args:
        src_dir (str): Path to the source directory.
        dst_dir (str): Path to the destination directory.
        camera (str): Name of the camera to be resized.
        rig_resolution (int): Width of the resize. Aspect ratio is maintained.
        frame (str): Name of the frame to render.
        threshold (int): Threshold to be used for binary thresholding. No thresholding
            is performed if None is passed in.
    """
    original_file = get_frame_path(src_dir, camera, frame)
    frame_fn = os.path.basename(original_file)
    _, ext = os.path.splitext(frame_fn)
    if ext == ".pfm":
        img = imageio.imread(original_file)
    else:
        img = cv2.imread(original_file, cv2.IMREAD_UNCHANGED)
    ratio = rig_resolution[1] / rig_resolution[0]
    for level, width in enumerate(config.WIDTHS):
        height = round(ratio * width)
        height += height % 2

        level_name = f"level_{level}"
        new_file = os.path.join(dst_dir, level_name, camera, frame_fn)
        os.makedirs(os.path.dirname(new_file), exist_ok=True)
        scaled = cv2.resize(img, (width, height), interpolation=cv2.INTER_AREA)
        if threshold is not None:
            _, scaled = cv2.threshold(scaled, threshold, 255, cv2.THRESH_BINARY)
        if ext == ".pfm":
            imageio.imwrite(new_file, scaled)
        else:
            cv2.imwrite(new_file, scaled)


def verify_frame(src_dir, camera, frame):
    original_file = get_frame_path(src_dir, camera, frame)
    if not os.path.isfile(original_file):
        raise Exception(f"Non-existent file for resize: {original_file}")


def resize_frames(src_dir, dst_dir, rig, first, last, threshold=None):
    """Resizes a frame to the appropriate pyramid level sizes. Files are saved in
    level_0/[camera], ..., level_9/[camera] in the destination directory.

    Args:
        src_dir (str): Path to the source directory.
        dst_dir (str): Path to the destination directory.
        rig (dict[str, _]): Rig descriptor object.
        first (str): Name of the first frame to render.
        last (str): Name of the last frame to render.
        threshold (int): Threshold to be used for binary thresholding. No thresholding
            is performed if None is passed in.
    """
    num_workers = mp.cpu_count()

    try:
        mp.set_start_method(
            "spawn", force=True
        )  # see: https://github.com/opencv/opencv/issues/5150
    except RuntimeError:
        pass

    pool = mp.Pool(num_workers)
    for frame in range(int(first), int(last) + 1):
        for camera in rig["cameras"]:
            verify_frame(src_dir, camera["id"], get_frame_name(frame))
            pool.apply_async(
                resize_camera,
                args=(
                    src_dir,
                    dst_dir,
                    camera["id"],
                    camera["resolution"],
                    get_frame_name(frame),
                    threshold,
                ),
            )

    pool.close()
    pool.join()


def main(argv):
    """Validates flags and resizes the frame pointed at by them if determined to be valid.

    Args:
        argv (list[str]): List of arguments (used interally by abseil).
    """
    with open(FLAGS.rig, "r") as f:
        rig = json.load(f)

    cameras_rig = sorted([camera["id"] for camera in rig["cameras"]])
    cameras_dir = sorted(
        os.path.basename(d)
        for d in glob.iglob(os.path.join(FLAGS.src_dir, "*"))
        if os.path.isdir(d)
    )
    if len(cameras_dir) == 0:
        print(f"No cameras found in {FLAGS.src_dir}")
        sys.exit()
    if cameras_rig != cameras_dir:
        print(
            f"Cameras from rig differ from cameras in source directory: {cameras_rig} vs {cameras_dir}"
        )
        sys.exit()

    # Get list of frames from the first camera
    frames = []
    dirCamRef = os.path.join(FLAGS.src_dir, cameras_dir[0])
    img_ext = ""
    for f in glob.iglob(os.path.join(dirCamRef, "*")):
        if os.path.isfile(f):
            frame, img_ext = os.path.splitext(os.path.basename(f))
            frames.append(frame)
    frames.sort()

    if len(frames) == 0:
        print(f"No frames found in {dirCamRef}")
        sys.exit()

    if img_ext == "":
        print(f"Could not find image extension from frames in {dirCamRef}")
        sys.exit()

    if not FLAGS.first:
        FLAGS.first = frames[0]

    if not FLAGS.last:
        FLAGS.last = frames[-1]

    if FLAGS.first not in frames:
        print(f"Could not find frame {FLAGS.first} in {dirCamRef}")
        sys.exit()

    if FLAGS.last not in frames:
        print(f"Could not find frame {FLAGS.last} in {dirCamRef}")
        sys.exit()

    resize_frames(
        FLAGS.src_dir,
        FLAGS.dst_dir,
        rig,
        frames.index(FLAGS.first),
        frames.index(FLAGS.last),
    )


if __name__ == "__main__":
    flags.DEFINE_string("dst_dir", None, "Destination directory")
    flags.DEFINE_string("first", "", "First frame to extract")
    flags.DEFINE_string("last", "", "Last frame to extract")
    flags.DEFINE_string("rig", None, "Camera rig json (to get list of cameras)")
    flags.DEFINE_string("src_dir", None, "Directory containing camera images")

    # Required FLAGS.
    flags.mark_flag_as_required("rig")
    flags.mark_flag_as_required("src_dir")
    flags.mark_flag_as_required("dst_dir")
    app.run(main)
