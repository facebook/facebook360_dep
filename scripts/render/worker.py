#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Consumption side of render message queue.

Provides the interface for performing computations based on messages received
from the queue. Running a worker node only has semantic meaning in the context of
subscribing to a master running a queue, but once it establishes connection, it will
continue to poll for messages until explicitly terminated or the connection is closed.

Example:
    To run a single worker node subscribed to 192.168.1.100:

        $ python worker.py \
          --master=192.168.1.100

Attributes:
    FLAGS (absl.flags._flagvalues.FlagValues): Globally defined flags for worker.py.
"""

import functools
import json
import os
import shutil
import sys
import threading
from copy import copy

import pika
from absl import app, flags

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "util"))

import config
from network import (
    copy_image_level,
    download,
    download_image_type,
    download_image_types,
    download_rig,
    get_cameras,
    get_frame_fns,
    get_frame_name,
    get_frame_range,
    local_image_type_path,
    local_rig_path,
    remote_image_type_path,
    upload,
    upload_image_type,
)
from resize import resize_frames
from scripts.render.network import Address
from scripts.util.system_util import run_command
from setup import bin_to_flags

FLAGS = flags.FLAGS


def _run_bin(msg):
    """Runs the binary associated with the message. The execution assumes the worker is
    running in a configured Docker container.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
    """
    msg_cp = copy(msg)

    # The binary flag convention includes the "last" frame
    msg_cp["last"] = get_frame_name(int(msg["last"]))
    app_name = msg_cp["app"].split(":")[0]
    relevant_flags = [flag["name"] for flag in bin_to_flags[app_name]]
    cmd_flags = " ".join(
        [
            f"--{flag}={msg_cp[flag]}"
            for flag in relevant_flags
            if flag in msg_cp and msg_cp[flag] != ""
        ]
    )

    # Order is determined to prevent substrings from being accidentally replaced
    input_root = msg_cp["input_root"].rstrip("/")
    output_root = msg_cp["output_root"].rstrip("/")

    root_order = (
        [output_root, input_root]
        if input_root in output_root
        else [input_root, output_root]
    )
    root_to_docker = {
        input_root: config.DOCKER_INPUT_ROOT,
        output_root: config.DOCKER_OUTPUT_ROOT,
    }

    for root in root_order:
        if not os.path.exists(root):
            cmd_flags = cmd_flags.replace(root, root_to_docker[root])

    bin_path = os.path.join(config.DOCKER_BUILD_ROOT, "bin", app_name)
    cmd = f"GLOG_alsologtostderr=1 GLOG_stderrthreshold=0 {bin_path} {cmd_flags}"
    run_command(cmd)


def _clean_worker(ran_download, ran_upload):
    """Deletes any files that were downloaded or uploaded.

    Args:
        ran_download (bool): Whether or not a download was performed.
        ran_upload (bool): Whether or not an upload was performed.
    """
    if ran_download and os.path.exists(config.DOCKER_INPUT_ROOT):
        shutil.rmtree(config.DOCKER_INPUT_ROOT)
    if ran_upload and os.path.exists(config.DOCKER_INPUT_ROOT):
        shutil.rmtree(config.DOCKER_OUTPUT_ROOT)


def generate_foreground_masks_callback(msg):
    """Runs foreground mask generation according to parameters read from the message.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
    """
    print("Running foreground mask generation...")

    image_types_to_level = [("color", msg["level"])]
    ran_download = download_rig(msg)
    ran_download |= download_image_types(msg, image_types_to_level)
    ran_download |= download_image_type(
        msg, "background_color", [msg["background_frame"]], msg["level"]
    )

    msg_cp = copy(msg)
    msg_cp["color"] = local_image_type_path(msg, "color", msg["level"])
    msg_cp["background_color"] = local_image_type_path(
        msg, "background_color", msg["level"]
    )
    msg_cp["foreground_masks"] = local_image_type_path(
        msg, "foreground_masks", msg["dst_level"]
    )

    _run_bin(msg_cp)
    ran_upload = upload_image_type(msg, "foreground_masks", level=msg["dst_level"])
    _clean_worker(ran_download, ran_upload)


def resize_images_callback(msg):
    """Runs image resizing according to parameters read from the message.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
    """
    print("Running image resizing...")

    image_types_to_level = [(msg["image_type"], None)]
    ran_download = download_rig(msg)
    ran_download |= download_image_types(msg, image_types_to_level)

    with open(local_rig_path(msg), "r") as f:
        rig = json.load(f)
    local_src_dir = local_image_type_path(msg, msg["image_type"])
    local_dst_dir = local_image_type_path(
        msg, config.type_to_levels_type[msg["image_type"]]
    )
    resize_frames(
        local_src_dir, local_dst_dir, rig, msg["first"], msg["last"], msg["threshold"]
    )

    # Clean up workspace to prevent using too much disk space on workers
    for level in msg["dst_level"]:
        ran_upload = upload_image_type(msg, msg["image_type"], level=level)
    _clean_worker(ran_download, ran_upload)


def depth_estimation_callback(msg):
    """Runs depth estimation according to parameters read from the message.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
    """
    print("Running depth estimation...")

    ran_download = False
    msg_cp = copy(msg)
    if msg["image_type"] == "disparity":
        image_types_to_level = [("color", msg["level_start"])]
        if msg["use_foreground_masks"]:
            ran_download |= download_image_type(
                msg, "background_disp", [msg["background_frame"]], msg["level_start"]
            )
            image_types_to_level.append(("foreground_masks", msg["level_start"]))

        if msg["level_start"] < msg["num_levels"] - 1:
            image_types_to_level.append(("disparity", msg["level_start"] + 1))
            if msg["use_foreground_masks"]:
                image_types_to_level.append(
                    ("foreground_masks", msg["level_start"] + 1)
                )

    else:
        image_types_to_level = [("background_color", msg["level_start"])]
        if msg["level_start"] < msg["num_levels"] - 1:
            image_types_to_level.append(("background_disp", msg["level_start"] + 1))

        msg_cp["color"] = local_image_type_path(msg, "background_color_levels")
        msg_cp["output_root"] = os.path.join(msg["input_root"], "background")

    ran_download |= download_rig(msg)
    ran_download |= download_image_types(msg, image_types_to_level)

    _run_bin(msg_cp)
    ran_upload = upload_image_type(msg, msg["image_type"], level=msg["level_end"])
    _clean_worker(ran_download, ran_upload)


def temporal_filter_callback(msg):
    """Runs temporal filtering according to parameters read from the message.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
    """
    print("Running temporal filtering...")

    # If such frames do not exist, S3 simply does not download them
    msg_cp = copy(msg)
    frames = get_frame_range(msg["filter_first"], msg["filter_last"])
    image_types_to_level = [("color", msg["level"]), ("disparity", msg["level"])]
    if msg["use_foreground_masks"]:
        image_types_to_level.append(("foreground_masks", msg["level"]))

    ran_download = download_rig(msg)
    ran_download |= download_image_types(msg, image_types_to_level, frames)

    msg_cp["disparity"] = ""  # disparity_level is automatically populated by app
    _run_bin(msg_cp)
    processed_frames = get_frame_range(msg["first"], msg["last"])
    ran_upload = upload_image_type(
        msg, "disparity_time_filtered", processed_frames, level=msg["level"]
    )
    _clean_worker(ran_download, ran_upload)


def transfer_callback(msg):
    """Runs transfer according to parameters read from the message.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
    """
    print("Running rearranging...")

    rig_cameras = get_cameras(msg, "cameras")
    frames = get_frame_range(msg["first"], msg["last"])
    copy_image_level(
        msg,
        msg["src_image_type"],
        msg["dst_image_type"],
        rig_cameras,
        frames,
        msg["src_level"],
        msg["dst_level"],
    )


def _run_upsample(msg, run_upload=True):
    """Runs disparity upsampling according to parameters read from the message.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
        run_upload (bool, optional): Whether or not an upload was performed.

    Returns:
        tuple(bool, bool): Respectively whether or not a download and upload were performed.
    """
    image_types_to_level = [(msg["image_type"], msg["level"])]

    msg_cp = copy(msg)
    if msg["image_type"] == "disparity":
        color_image_type = "color"
        image_types_to_level += [
            ("foreground_masks", msg["level"]),
            ("foreground_masks", msg["dst_level"]),
        ]
        msg_cp["foreground_masks_in"] = local_image_type_path(
            msg, "foreground_masks", msg["level"]
        )
        msg_cp["foreground_masks_out"] = local_image_type_path(
            msg, "foreground_masks", msg["dst_level"]
        )
        msg_cp["background_disp"] = local_image_type_path(
            msg, "background_disp", msg["dst_level"]
        )

        download_image_type(
            msg, "background_disp", [msg["background_frame"]], msg["dst_level"]
        )
        msg_cp["background_disp"] = local_image_type_path(
            msg, "background_disp", msg["dst_level"]
        )

    elif msg["image_type"] == "background_disp":
        color_image_type = "background_color"
        msg_cp["foreground_masks_in"] = ""  # Background upsampling doesn't use masks
        msg_cp["foreground_masks_out"] = ""

    image_types_to_level.append((color_image_type, msg["dst_level"]))

    ran_download = download_image_types(msg, image_types_to_level)
    ran_download |= download_rig(msg)

    msg_cp["disparity"] = local_image_type_path(msg, msg["image_type"], msg["level"])
    msg_cp["output"] = local_image_type_path(
        msg, config.type_to_upsample_type[msg["image_type"]]
    )
    msg_cp["color"] = local_image_type_path(msg, color_image_type, msg["dst_level"])

    _run_bin(msg_cp)
    if run_upload:
        ran_upload = upload_image_type(
            msg, config.type_to_upsample_type[msg["image_type"]]
        )
    return ran_download, ran_upload


def upsample_disparity_callback(msg):
    """Runs disparity upsampling according to parameters read from the message.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
    """
    print("Running disparity upsampling...")
    ran_download, ran_upload = _run_upsample(msg)
    _clean_worker(ran_download, ran_upload)


def upsample_layer_disparity_callback(msg):
    """Runs disparity upsampling and layering according to parameters read from the message.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
    """
    print("Running disparity upsampling and layering...")

    msg_cp = copy(msg)
    msg_cp["app"] = "UpsampleDisparity"
    ran_download, _ = _run_upsample(msg, run_upload=False)
    ran_download |= download_image_type(
        msg, config.type_to_upsample_type["background_disp"], [msg["background_frame"]]
    )

    msg_cp["app"] = "LayerDisparities"
    msg_cp["background_disp"] = local_image_type_path(
        msg, config.type_to_upsample_type["background_disp"]
    )
    msg_cp["foreground_disp"] = local_image_type_path(
        msg, config.type_to_upsample_type["disparity"]
    )
    msg_cp["output"] = config.DOCKER_OUTPUT_ROOT

    _run_bin(msg_cp)
    ran_upload = upload_image_type(msg, "disparity")
    _clean_worker(ran_download, ran_upload)


def convert_to_binary_callback(msg):
    """Runs binary conversion according to parameters read from the message.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
    """
    print("Converting to binary...")
    msg_cp = copy(msg)
    ran_download = download_rig(msg)

    rig_json = os.path.basename(msg["rig"])
    ext_index = rig_json.index(".")
    fused_json = f"{rig_json[:ext_index]}_fused{rig_json[ext_index:]}"

    if msg["run_conversion"]:
        image_types_to_level = [
            (msg["color_type"], None),
            (msg["disparity_type"], msg["level"]),
        ]
        msg_cp["disparity"] = local_image_type_path(
            msg, msg["disparity_type"], msg["level"]
        )
        msg_cp["color"] = local_image_type_path(msg, msg["color_type"])
        msg_cp["fused"] = ""  # fusion is done independently from conversion
        ran_download |= download_image_types(msg, image_types_to_level)

        # If we only have color levels uploaded to S3, we fall back to level_0
        if len(os.listdir(msg_cp["color"])) == 0:
            ran_download = download_image_types(msg, [(msg["color_type"], 0)])
            msg_cp["color"] = local_image_type_path(msg, msg["color_type"], 0)
    else:
        image_types_to_level = [("bin", None)]
        local_fused_dir = local_image_type_path(msg, "fused")

        # Paths are explicitly emptied to avoid path verifications
        msg_cp["color"] = ""
        msg_cp["disparity"] = ""
        msg_cp["foreground_masks"] = ""
        msg_cp["fused"] = local_fused_dir

        ran_download |= download_image_types(msg, image_types_to_level)
        ran_download |= download(
            src=os.path.join(remote_image_type_path(msg, "bin"), fused_json),
            dst=os.path.join(local_image_type_path(msg, "bin"), fused_json),
        )

    msg_cp["bin"] = local_image_type_path(msg, "bin")

    os.makedirs(msg["bin"], exist_ok=True)

    _run_bin(msg_cp)
    if msg["run_conversion"]:
        ran_upload = upload_image_type(msg, "bin")
        ran_upload |= upload(
            src=os.path.join(local_image_type_path(msg, "bin"), fused_json),
            dst=os.path.join(remote_image_type_path(msg, "bin"), fused_json),
        )
    else:
        # We use a raw upload since upload_image_type only handles frames but we want to
        # also upload the fused json here
        ran_upload = upload(
            src=local_image_type_path(msg, "fused"),
            dst=remote_image_type_path(msg, "fused"),
            filters=["*"],
        )
    _clean_worker(ran_download, ran_upload)


def simple_mesh_renderer_callback(msg):
    print("Generating exports...")

    msg_cp = copy(msg)
    frames = get_frame_range(msg_cp["first"], msg_cp["last"])
    ran_download = download_rig(msg)

    ran_download = download_image_type(msg, msg_cp["color_type"], frames)
    ran_download |= download_image_type(msg, msg_cp["disparity_type"], frames)
    msg_cp["color"] = local_image_type_path(msg, msg_cp["color_type"])
    msg_cp["disparity"] = local_image_type_path(msg, msg_cp["disparity_type"])
    msg_cp["output"] = local_image_type_path(msg, msg_cp["dst_image_type"])
    msg_cp["position"] = '"0.0 0.0 0.0"'
    msg_cp["forward"] = '"-1.0 0.0 0.0"'
    msg_cp["up"] = '"0.0 0.0 1.0"'

    _run_bin(msg_cp)
    ran_upload = upload_image_type(msg, msg_cp["dst_image_type"], frames)
    _clean_worker(ran_download, ran_upload)


def success(channel, delivery_tag):
    if channel.is_open:
        channel.basic_ack(delivery_tag)
        channel.queue_declare(config.RESPONSE_QUEUE_NAME)
        channel.basic_publish(
            exchange="", routing_key=config.RESPONSE_QUEUE_NAME, body="Completed!"
        )
    else:
        pass


def failure(channel, delivery_tag, msg):
    if channel.is_open:
        channel.basic_reject(delivery_tag)
        channel.queue_declare(config.QUEUE_NAME)
        channel.basic_publish(
            exchange="",
            routing_key=config.QUEUE_NAME,
            body=json.dumps(msg),
            properties=pika.BasicProperties(delivery_mode=2),  # make message persistent
        )
    else:
        pass


def handle_message(connection, channel, delivery_tag, body):
    msg = json.loads(body.decode("utf-8"))
    try:
        print(f"Received {msg}")

        app_name_to_callback = {
            "GenerateForegroundMasks": generate_foreground_masks_callback,
            "DerpCLI": depth_estimation_callback,
            "TemporalBilateralFilter": temporal_filter_callback,
            "Transfer": transfer_callback,
            "UpsampleDisparity": upsample_disparity_callback,
            "UpsampleLayer": upsample_layer_disparity_callback,
            "ConvertToBinary": convert_to_binary_callback,
            "SimpleMeshRenderer": simple_mesh_renderer_callback,
            "Resize": resize_images_callback,
        }

        for app_name in app_name_to_callback:
            if msg["app"].startswith(app_name):
                app_name_to_callback[app_name](msg)
                break

        # Sends response of job completion
        success_callback = functools.partial(success, channel, delivery_tag)
        connection.add_callback_threadsafe(success_callback)
    except Exception:
        failure_callback = functools.partial(failure, channel, delivery_tag, msg)
        connection.add_callback_threadsafe(failure_callback)


def callback(ch, method, properties, body, connection):
    """Dispatches to different callbacks based on the contents of the message.

    Args:
        Only the body argument is necessary. The others are imposed by Pika.

        ch (pika.Channel): N/a
        method (pika.spec.Basic): N/a
        properties (pika.spec.BasicProperties): N/a
        body (bytes): utf-8 encoded message published to the message queue.
    """
    handle = threading.Thread(
        target=handle_message, args=(connection, ch, method.delivery_tag, body)
    )
    handle.start()


def main_loop(argv):
    """Sets up the callback loop for the worker.

    Args:
        argv (list[str]): List of arguments (used interally by abseil).
    """
    while True:
        try:
            connection = pika.BlockingConnection(
                pika.ConnectionParameters(FLAGS.master)
            )
            on_message_callback = functools.partial(callback, connection=(connection))
            channel = connection.channel()
            channel.queue_declare(queue=config.QUEUE_NAME)
            channel.basic_qos(prefetch_count=1)
            channel.basic_consume(
                queue=config.QUEUE_NAME,
                auto_ack=False,
                on_message_callback=on_message_callback,
            )
            channel.start_consuming()
        # Don't recover if connection was closed by broker
        except pika.exceptions.ConnectionClosedByBroker:
            break
        # Don't recover on channel errors
        except pika.exceptions.AMQPChannelError:
            break
        # Recover on all other connection errors
        except pika.exceptions.AMQPConnectionError:
            continue


if __name__ == "__main__":
    # Abseil entry point app.run() expects all flags to be already defined
    flags.DEFINE_string("master", None, "master IP")

    # Required FLAGS.
    flags.mark_flag_as_required("master")
    app.run(main_loop)
