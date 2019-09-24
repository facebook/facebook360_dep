#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Entrypoint for distributed render back-end.

The render script currently supports three modes of operation:
    - Single node (i.e. master/worker are the same machine)
    - LAN farms (i.e. master directly communicates to worker nodes in manually configured farm)
    - AWS farms (i.e. master sets up workers via kubernetes)

For users who wish to get results rather than modify the internals, we suggest using the
front-end to this render pipeline by using run.py.

Example:
    To run a single node render:

        $ python render.py \
          --input_root=/path/to/data \
          --output_root=/path/to/data/output \
          --rig=/path/to/data/rigs/rig_calibrated.json \
          --first=001700 \
          --last=001700

    To run on LAN:

        $ python render.py \
          --input_root=smb://192.168.1.100/example \
          --output_root=smb://192.168.1.100/example/output \
          --rig=smb://192.168.1.100/example/rigs/rig_calibrated.json \
          --first=001700 \
          --last=001700 \
          --master=192.168.1.100 \
          --workers=192.168.1.100,192.168.1.101

    To run on AWS:

        $ python render.py \
          --input_root=s3://example/data \
          --output_root=s3://example/data/output \
          --rig=s3://example/data/rigs/rig_calibrated.json \
          --first=001700 \
          --last=001700

Attributes:
    FLAGS (absl.flags._flagvalues.FlagValues): Globally defined flags for render.py.
"""

import logging
import os
import sys

from absl import app, flags

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "util"))

import config
import glog_check as glog
import setup
from network import Address, get_frame_name
from pipeline import Pipeline
from scripts.util.system_util import image_type_paths

requests_logger = logging.getLogger("pika")
requests_logger.propagate = False  # prevents pika logger from being displayed

FLAGS = flags.FLAGS


def verify_inputs():
    """Verifies that all the command line flags are valid."""
    glog.check_ne(FLAGS.input_root, "", "Input_root cannot be empty")
    glog.check_ne(FLAGS.output_root, "", "Output_root cannot be empty")

    if not FLAGS.rig:
        FLAGS.rig = os.path.join(FLAGS.input_root, "rig.json")

    if not FLAGS.color:
        FLAGS.color = os.path.join(FLAGS.input_root, image_type_paths["color"])

    if not FLAGS.background_disp:
        FLAGS.background_disp = os.path.join(
            FLAGS.input_root, image_type_paths["background_disp"]
        )

    if not FLAGS.background_color:
        FLAGS.background_color = os.path.join(
            FLAGS.input_root, image_type_paths["background_color"]
        )

    if not FLAGS.foreground_masks:
        FLAGS.foreground_masks = os.path.join(
            FLAGS.input_root, image_type_paths["foreground_masks"]
        )

    FLAGS.workers = config.LOCALHOST if not FLAGS.workers else FLAGS.workers
    FLAGS.master = config.LOCALHOST if not FLAGS.master else FLAGS.master

    # Check flag values
    glog.check_ge(FLAGS.random_proposals, 0, "Random_proposals must be > 1")
    glog.check_le(FLAGS.first, FLAGS.last, "First must be <= last")

    if FLAGS.run_depth_estimation and FLAGS.do_temporal_filter:
        glog.check_gt(FLAGS.time_radius, 0, "Temporal filter radius must be > 0")
        num_frames = int(FLAGS.last) - int(FLAGS.first) + 1

        # Ignore temporal filter if we do not have enough frames
        if num_frames < 2 * int(FLAGS.time_radius) - 1:
            FLAGS.do_temporal_filter = False
            FLAGS.time_radius = 0

        glog.check_gt(
            num_frames,
            FLAGS.time_radius,
            f"Number of frames ({num_frames}) must be greater than temporal range ({FLAGS.time_radius})",
        )


def set_input_param(base_params, image_type):
    """Updates paths to the flags to reflect those constructed inside Docker.

    Args:
        base_params (dict[str, _]): Map of all the FLAGS defined in render.py.
        image_type (str): Name of an image type (re: source/util/ImageTypes.h).
    """
    base_params[image_type] = os.path.join(
        config.DOCKER_INPUT_ROOT, image_type_paths[image_type]
    )


def main():
    """Runs the main render pipeline with the parameters passed in through command line args."""
    base_params = {
        flag: value
        for flag, value in FLAGS.flag_values_dict().items()
        if flag in setup.flag_names
    }

    if not FLAGS.skip_setup and FLAGS.cloud == "":
        setup.setup_workers(base_params)

    # If the address is a Samba or local endpoint, we have the addresses mapped in Docker
    # Access to the externally visible endpoint is only necessary in remote cases
    input_protocol = Address(FLAGS.input_root).protocol

    base_params["num_levels"] = len(config.WIDTHS)
    if input_protocol is None or input_protocol == "smb":
        base_params["input_root"] = config.DOCKER_INPUT_ROOT
        base_params["output_root"] = config.DOCKER_OUTPUT_ROOT
        base_params["rig"] = FLAGS.rig.replace(
            FLAGS.input_root, base_params["input_root"]
        )
        input_image_types = {
            "background_color",
            "background_disp",
            "color",
            "foreground_masks",
        }
        for image_type in input_image_types:
            set_input_param(base_params, image_type)

    # frame_chunks use the Python range standard where first is included but last excluded
    frame_chunks = [
        {
            "first": get_frame_name(frame),
            "last": get_frame_name(min(int(FLAGS.last), frame + FLAGS.chunk_size - 1)),
        }
        for frame in range(int(FLAGS.first), int(FLAGS.last) + 1, FLAGS.chunk_size)
    ]
    if FLAGS.background_frame == "":
        background_frame = None
    else:
        background_frame = [
            {
                "first": get_frame_name(int(FLAGS.background_frame)),
                "last": get_frame_name(int(FLAGS.background_frame)),
            }
        ]

    pipeline = Pipeline(
        FLAGS.master, base_params, frame_chunks, background_frame, FLAGS.force_recompute
    )

    # We need resized colors to compute foreground masks
    pipeline_stages = [
        (pipeline.precompute_resizes, FLAGS.run_precompute_resizes),
        (pipeline.generate_foreground_masks, FLAGS.run_generate_foreground_masks),
    ]

    if FLAGS.use_foreground_masks:
        # Resize foreground masks
        pipeline_stages.append(
            (
                pipeline.precompute_resizes_foreground,
                FLAGS.run_precompute_resizes_foreground,
            )
        )

    pipeline_stages.append((pipeline.depth_estimation, FLAGS.run_depth_estimation))

    if FLAGS.format == "6dof":
        pipeline_stages += [
            (pipeline.convert_to_binary, FLAGS.run_convert_to_binary),
            (pipeline.fusion, FLAGS.run_fusion),
        ]
    else:
        pipeline_stages.append(
            (pipeline.simple_mesh_renderer, FLAGS.run_simple_mesh_renderer)
        )

    pipeline.run(pipeline_stages)
    setup.cleanup_workers()


def main_loop(argv):
    """Validates and logs flags and renders with them if determined to be valid.

    Args:
        argv (list[str]): List of arguments (used interally by abseil).
    """
    setup.init_facebook360_dep(FLAGS)
    setup.log_flags()
    verify_inputs()
    main()


if __name__ == "__main__":
    # Abseil entry point app.run() expects all flags to be already defined
    setup.define_flags()
    app.run(main_loop)
