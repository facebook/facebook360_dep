#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Defines independent building blocks (stages) of the render pipeline.

Defines a Pipeline class with various stages corresponding to different stages of the
render process. The class can easily be extended to allow for additional stages. The stages
are designed to be independent of one another to allow the pipeline to be modifiable and
create many entrypoints.

Example:
    The standard pipeline is defined in render.py, but a modified version that uses the
    yet undefined awesome_special_effects pipeline stage can be seen as:

        >>> pipeline_stages = [
            (pipeline.depth_estimation, FLAGS.run_depth_estimation),
            (pipeline.convert_to_binary, FLAGS.run_convert_to_binary),
            (pipeline.fusion, FLAGS.run_fusion),
            (pipeline.awesome_special_effects, FLAGS.run_awesome_special_effects),
        ]
        >>> pipeline.run(pipeline_stages)
"""

import json
import os
import sys
import time
from copy import copy

import pika
import progressbar

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "util"))

import config
import setup
from network import (
    Address,
    download,
    get_frame_fns,
    get_frame_name,
    get_frame_range,
    listdir,
    remote_image_type_path,
)
from scripts.util.system_util import image_type_paths


class Pipeline:
    """Pipeline class for rendering stages. Pipeline stages process sequentially.

    Attributes:
        background_frame (dict[str, str]): Map of the background frame chunk with keys
            "first" and "last" corresponding to the appropriate frame names for the chunk.
        base_params (dict[str, _]): Map of all the FLAGS defined in render.py.
        force_recompute (bool): Whether or not to overwrite previous computations.
        frame_chunks (list[dict[str, str]]): List of frame chunk with keys
            "first" and "last" corresponding to the appropriate frame names for the chunk.
        master_ip (str): IP of the master host.
    """

    def __init__(
        self,
        master_ip,
        base_params,
        frame_chunks,
        background_frame,
        force_recompute=False,
    ):
        """Constructs empty pipeline.

        Args:
            master_ip (str): IP of the master host.
            base_params (dict[str, _]): Map of all the FLAGS defined in render.py.
            frame_chunks (list[dict[str, str]]): List of frame chunk with keys
                "first" and "last" corresponding to the appropriate frame names for the chunk.
            background_frame (dict[str, str]): Map of the background frame chunk with keys
                "first" and "last" corresponding to the appropriate frame names for the chunk.
            force_recompute (bool, optional): Whether or not to overwrite previous computations.
        """
        self.base_params = base_params
        self.frame_chunks = frame_chunks
        self.background_frame = background_frame
        self.master_ip = master_ip
        self.force_recompute = force_recompute

        # We only need to spawn the master if there is no RabbitMQ node already available
        try:
            pika.BlockingConnection(pika.ConnectionParameters(master_ip))
        except Exception:
            setup.setup_master(base_params)

        # Since the master is spawned asynchronously, we have to await the queue to be available
        while True:
            try:
                self.purge_queue(config.QUEUE_NAME)
                break
            except Exception:
                time.sleep(1)
                continue

    def purge_queue(self, queue_name):
        """Clears contents of a queue.

        Args:
            queue_name (str): Name of the queue to clear.
        """
        connection = pika.BlockingConnection(pika.ConnectionParameters(self.master_ip))
        channel = connection.channel()
        channel.queue_declare(queue=queue_name)
        channel.queue_purge(queue=queue_name)  # clears queue from previous runs

    def _get_missing_chunks_level(self, params, level, frame_chunks):
        dst_dir = remote_image_type_path(params, params["dst_image_type"], level)
        dst_frames = get_frame_range(frame_chunks[0]["first"], frame_chunks[-1]["last"])
        remote = Address(dst_dir)

        uncompressed = remote.protocol != "s3"
        try:
            expected_frame_fns = set(
                get_frame_fns(params, dst_frames, uncompressed, dst_dir)
            )
            actual_frame_fns = listdir(dst_dir, run_silently=True, recursive=True)
        except Exception as e:
            print(e)
            return None

        missing_frames_fns = expected_frame_fns - actual_frame_fns
        missing_frames = [
            os.path.splitext(os.path.basename(frames_fn))[0]
            for frames_fn in missing_frames_fns
        ]
        return missing_frames

    def _get_missing_chunks(self, params, frame_chunks):
        if params["force_recompute"]:
            return frame_chunks

        print(f"Checking cache for {params['app']}...")
        if isinstance(params["dst_level"], list):
            missing_frames = set()
            for dst_level in params["dst_level"]:
                new_missing_chunks = self._get_missing_chunks_level(
                    params, dst_level, frame_chunks
                )
                if new_missing_chunks is None:
                    return frame_chunks
                missing_frames = missing_frames.union(new_missing_chunks)
        else:
            missing_frames = self._get_missing_chunks_level(
                params, params["dst_level"], frame_chunks
            )
            if missing_frames is None:
                return frame_chunks
        if len(missing_frames) == 0:
            return []

        missing_frame_chunks = []
        for frame_chunk in frame_chunks:
            for frame in get_frame_range(frame_chunk["first"], frame_chunk["last"]):
                if frame in missing_frames:
                    missing_frame_chunks.append(frame_chunk)
                    break
        return missing_frame_chunks

    def run_halted_queue(self, params, frame_chunks):
        """Runs a queue with params for each of the frame chunks. The program halts while
        awaiting the completion of tasks in the queue and shows a progress bar meanwhile. Any
        frame chunks that have been previously completed will be marked as complete unless
        running with force_recompute.

        Args:
            params (dict[str, _]): Message to be published to RabbitMQ.
            frame_chunks (list[dict[str, str]]): List of frame chunk with keys
                "first" and "last" corresponding to the appropriate frame names for the chunk.
        """
        connection = pika.BlockingConnection(
            pika.ConnectionParameters(self.master_ip, heartbeat=0)
        )
        channel = connection.channel()
        channel.queue_declare(queue=config.QUEUE_NAME)
        channel.queue_declare(queue=config.RESPONSE_QUEUE_NAME)

        self.purge_queue(config.QUEUE_NAME)
        self.purge_queue(config.RESPONSE_QUEUE_NAME)

        # force_recompute can be specified over the entire pipeline or particular stages
        frame_chunks = self._get_missing_chunks(params, frame_chunks)
        if len(frame_chunks) == 0:
            return

        for frame_chunk in frame_chunks:
            params.update(frame_chunk)
            msg = json.dumps(params)
            channel.basic_publish(
                exchange="",
                routing_key=config.QUEUE_NAME,
                body=msg,
                properties=pika.BasicProperties(
                    delivery_mode=2
                ),  # make message persistent
            )

        # Waits until the queue is empty before returning for next step
        queue_state = channel.queue_declare(config.RESPONSE_QUEUE_NAME)
        queue_size = queue_state.method.message_count

        progress = "█"
        widgets = [
            f"{progress} ",
            f"{params['app']}:",
            progressbar.Bar(progress, "|", "|"),
            progressbar.Percentage(),
            " (Workers: ",
            progressbar.FormatLabel("0"),
            ") (",
            progressbar.FormatLabel("%(elapsed)s"),
            ")",
        ]
        bar = progressbar.ProgressBar(maxval=len(frame_chunks), widgets=widgets)
        bar.start()
        no_worker_period = None
        while queue_size != len(frame_chunks):
            time.sleep(1.0)
            queue_size = channel.queue_declare(
                config.RESPONSE_QUEUE_NAME
            ).method.message_count
            num_workers = channel.queue_declare(config.QUEUE_NAME).method.consumer_count
            widgets[5] = str(num_workers)

            if num_workers != 0:
                no_worker_period = None
            if num_workers == 0:
                if no_worker_period is None:
                    no_worker_period = time.time()
                if time.time() - no_worker_period > config.NO_WORKER_TIMEOUT:
                    raise Exception(
                        "No workers for extended time! Check worker logs for errors..."
                    )
            bar.update(queue_size)
        bar.finish()

    def generate_foreground_masks(self):
        """Runs distributed foreground mask generation."""
        depth_params = copy(self.base_params)
        depth_params.update(
            {
                "app": "GenerateForegroundMasks",
                "level": 0,
                "dst_level": None,
                "dst_image_type": "foreground_masks",
            }
        )
        self.run_halted_queue(depth_params, self.frame_chunks)

    def _resize_job(self, resize_params, image_type, frame_chunks, threshold=None):
        """Runs distributed arbitrary image type resizing.

        Args:
            params (dict[str, _]): Message to be published to RabbitMQ.
            image_type (str): Name of an image type (re: source/util/ImageTypes.h).
            frame_chunks (list[dict[str, str]]): List of frame chunk with keys
                "first" and "last" corresponding to the appropriate frame names for the chunk.
            threshold (int, optional): Binary threshold to be applied to resizing operation. If
                None is passed in, no thresholding is performed.
        """
        resize_params["app"] = f"Resize: {image_type.capitalize()}"
        resize_params["image_type"] = image_type
        resize_params["threshold"] = threshold
        resize_params["dst_image_type"] = image_type
        resize_params["dst_level"] = [level for level, _ in enumerate(config.WIDTHS)]
        self.run_halted_queue(resize_params, frame_chunks)

    def precompute_resizes_foreground(self):
        """Runs distributed foreground mask resizing."""
        resize_params = copy(self.base_params)
        self._resize_job(
            resize_params, "foreground_masks", self.frame_chunks, threshold=127
        )

    def precompute_resizes(self):
        """Runs distributed color, background color, and background disparity resizing."""
        resize_params = copy(self.base_params)
        if resize_params["disparity_type"] == "background_disp":
            self._resize_job(resize_params, "background_color", self.background_frame)
        elif resize_params["disparity_type"] == "disparity":
            self._resize_job(resize_params, "color", self.frame_chunks)
            if self.background_frame is not None:
                self._resize_job(
                    resize_params, "background_color", self.background_frame
                )
                self._resize_job(
                    resize_params, "background_disp", self.background_frame
                )
        else:
            raise Exception(
                f"Invalid disparity type: {resize_params['disparity_type']}"
            )

    def depth_estimation(self):
        """Runs distributed depth estimation with temporal filtering."""
        post_resize_params = copy(self.base_params)

        if self.base_params["disparity_type"] == "disparity":
            post_resize_params["color"] = os.path.join(
                self.base_params["input_root"],
                image_type_paths[config.type_to_levels_type["color"]],
            )
            post_resize_params["foreground_masks"] = os.path.join(
                self.base_params["input_root"],
                image_type_paths[config.type_to_levels_type["foreground_masks"]],
            )
            post_resize_params["background_disp"] = os.path.join(
                self.base_params["input_root"],
                image_type_paths[config.type_to_levels_type["background_disp"]],
            )
        elif self.base_params["disparity_type"] == "background_disp":
            post_resize_params["color"] = os.path.join(
                self.base_params["input_root"],
                image_type_paths[config.type_to_levels_type["color"]],
            )

        start_level = (
            post_resize_params["level_start"]
            if post_resize_params["level_start"] != -1
            else len(config.WIDTHS) - 1
        )
        if post_resize_params["level_end"] != -1:
            end_level = post_resize_params["level_end"]
        else:
            for level, width in enumerate(config.WIDTHS):
                if post_resize_params["resolution"] >= width:
                    end_level = level
                    break

        # Ranges for temporal filtering (only used if performing temporal filtering)
        filter_ranges = [
            {
                "first": frame_chunk["first"],
                "last": frame_chunk["last"],
                "filter_first": get_frame_name(
                    max(
                        int(post_resize_params["first"]),
                        int(frame_chunk["first"]) - post_resize_params["time_radius"],
                    )
                ),
                "filter_last": get_frame_name(
                    min(
                        int(post_resize_params["last"]),
                        int(frame_chunk["last"]) + post_resize_params["time_radius"],
                    )
                ),
            }
            for frame_chunk in self.frame_chunks
        ]

        # Optionally batch coarsest N levels in one job to reduce S3 traffic
        coarsest_batch = int(self.base_params.get("coarsest_batch_levels", 0))
        if coarsest_batch > 0:
            # Coarsest is start_level; define batched range [batch_start .. batch_end]
            batch_start = start_level
            batch_end = max(end_level, start_level - coarsest_batch + 1)
            if batch_end <= batch_start:
                batched_params = copy(post_resize_params)
                batched_params["output_formats"] = "pfm"
                batched_params.update(
                    {
                        "app": f"DerpCLI: Levels {batch_start}-{batch_end}",
                        "level_start": batch_start,
                        "level_end": batch_end,
                        "image_type": batched_params["disparity_type"],
                        "dst_level": list(range(batch_end, batch_start + 1)),
                        "dst_image_type": batched_params["disparity_type"],
                    }
                )
                # Run one message per frame chunk covering the whole level range
                self.run_halted_queue(batched_params, self.frame_chunks)
                # Skip temporal filtering for batched levels as per feature spec
                # After batching, continue remaining finer levels individually below
                start_level = batch_end - 1

        # Continue remaining levels normally (if any left after batching)
        for level in range(start_level, end_level - 1, -1):
            depth_params = copy(post_resize_params)
            if level != end_level:
                depth_params["output_formats"] = "pfm"
            depth_params.update(
                {
                    "app": f"DerpCLI: Level {level}",
                    "level_start": level,
                    "level_end": level,
                    "image_type": depth_params["disparity_type"],
                    "dst_level": level,
                    "dst_image_type": depth_params["disparity_type"],
                }
            )
            self.run_halted_queue(depth_params, self.frame_chunks)

            if post_resize_params["do_temporal_filter"]:
                filter_params = copy(post_resize_params)
                filter_params.update(
                    {
                        "app": "TemporalBilateralFilter",
                        "level": level,
                        "use_foreground_masks": post_resize_params[
                            "do_temporal_masking"
                        ],
                        "dst_level": level,
                        "dst_image_type": "disparity_time_filtered",
                    }
                )
                self.run_halted_queue(filter_params, filter_ranges)

                transfer_params = copy(post_resize_params)
                transfer_params.update(
                    {
                        "app": "Transfer",
                        "src_level": level,
                        "src_image_type": "disparity_time_filtered",
                        "dst_level": level,
                        "dst_image_type": "disparity",
                        "force_recompute": True,
                    }
                )
                self.run_halted_queue(transfer_params, self.frame_chunks)

        # Optionally batch coarsest N levels in one job to reduce S3 traffic
        coarsest_batch = int(self.base_params.get("coarsest_batch_levels", 0))
        if coarsest_batch > 0:
            # Coarsest is start_level; define batched range [batch_start .. batch_end]
            batch_start = start_level
            batch_end = max(end_level, start_level - coarsest_batch + 1)
            if batch_end <= batch_start:
                batched_params = copy(post_resize_params)
                batched_params["output_formats"] = "pfm"
                batched_params.update(
                    {
                        "app": f"DerpCLI: Levels {batch_start}-{batch_end}",
                        "level_start": batch_start,
                        "level_end": batch_end,
                        "image_type": batched_params["disparity_type"],
                        "dst_level": list(range(batch_end, batch_start + 1)),
                        "dst_image_type": batched_params["disparity_type"],
                    }
                )
                # Run one message per frame chunk covering the whole level range
                self.run_halted_queue(batched_params, self.frame_chunks)
                # Skip temporal filtering for batched levels as per feature spec
                # After batching, continue remaining finer levels individually below
                start_level = batch_end - 1

        # Continue remaining levels normally (if any left after batching)
        for level in range(start_level, end_level - 1, -1):
            depth_params = copy(post_resize_params)
            if level != end_level:
                depth_params["output_formats"] = "pfm"
            depth_params.update(
                {
                    "app": f"DerpCLI: Level {level}",
                    "level_start": level,
                    "level_end": level,
                    "image_type": depth_params["disparity_type"],
                    "dst_level": level,
                    "dst_image_type": depth_params["disparity_type"],
                }
            )
            self.run_halted_queue(depth_params, self.frame_chunks)

            if post_resize_params["do_temporal_filter"]:
                filter_params = copy(post_resize_params)
                filter_params.update(
                    {
                        "app": "TemporalBilateralFilter",
                        "level": level,
                        "use_foreground_masks": post_resize_params[
                            "do_temporal_masking"
                        ],
                        "dst_level": level,
                        "dst_image_type": "disparity_time_filtered",
                    }
                )
                self.run_halted_queue(filter_params, filter_ranges)

                transfer_params = copy(post_resize_params)
                transfer_params.update(
                    {
                        "app": "Transfer",
                        "src_level": level,
                        "src_image_type": "disparity_time_filtered",
                        "dst_level": level,
                        "dst_image_type": "disparity",
                        "force_recompute": True,
                    }
                )
                self.run_halted_queue(transfer_params, self.frame_chunks)
        if post_resize_params["resolution"] > config.WIDTHS[end_level]:
            # The upsampling color level is the smallest one larger than our last level
            dst_level = end_level - 1 if end_level > 0 else None
            upsample_params = copy(self.base_params)
            upsample_params.update(
                {
                    "app": "UpsampleDisparity",
                    "level": end_level,
                    "image_type": post_resize_params["disparity_type"],
                    "dst_level": dst_level,
                    "dst_image_type": config.type_to_upsample_type["disparity"],
                }
            )

            if post_resize_params["disparity_type"] == "background_disp":
                frame_chunks = self.background_frame
            elif post_resize_params["disparity_type"] == "disparity":
                frame_chunks = self.frame_chunks
            else:
                raise Exception(
                    f"Invalid disparity type {post_resize_params['disparity_type']}"
                )
            self.run_halted_queue(upsample_params, frame_chunks)

            transfer_params = copy(post_resize_params)
            transfer_params.update(
                {
                    "app": "Transfer",
                    "src_level": None,
                    "src_image_type": config.type_to_upsample_type["disparity"],
                    "dst_level": None,
                    "dst_image_type": post_resize_params["disparity_type"],
                }
            )
            self.run_halted_queue(transfer_params, frame_chunks)

        else:
            transfer_params = copy(post_resize_params)
            transfer_params.update(
                {
                    "app": "Transfer",
                    "src_level": end_level,
                    "src_image_type": post_resize_params["disparity_type"],
                    "dst_level": None,
                    "dst_image_type": post_resize_params["disparity_type"],
                }
            )
            self.run_halted_queue(transfer_params, self.frame_chunks)

    def convert_to_binary(self):
        """Runs distributed binary conversion."""
        convert_to_binary_params = copy(self.base_params)
        convert_to_binary_params.update(
            {
                "app": "ConvertToBinary: Meshing",
                "level": None,
                "foreground_masks": "",
                "run_conversion": True,
                "dst_level": None,
                "dst_image_type": "bin",
            }
        )
        self.run_halted_queue(convert_to_binary_params, self.frame_chunks)

    def fusion(self):
        """Runs distributed binary striping."""
        fusion_params = copy(self.base_params)
        fusion_params.update(
            {
                "app": "ConvertToBinary: Striping",
                "run_conversion": False,
                "dst_level": None,
                "dst_image_type": "fused",
            }
        )
        self.run_halted_queue(
            fusion_params,
            [
                {
                    "first": self.frame_chunks[0]["first"],
                    "last": self.frame_chunks[-1]["last"],
                }
            ],
        )

    def simple_mesh_renderer(self):
        simple_mesh_renderer_params = copy(self.base_params)
        simple_mesh_renderer_params.update(
            {
                "app": "SimpleMeshRenderer",
                "level": None,
                "dst_level": None,
                "dst_image_type": f"exports_{simple_mesh_renderer_params['format']}",
            }
        )
        self.run_halted_queue(simple_mesh_renderer_params, self.frame_chunks)

    def run(self, stages):
        """Runs the pipeline stages.

        Args:
            stages (list[tuple(func : void -> void), bool]): List of functions and whether
                or not they are to be executed.
        """
        for stage, run_stage in stages:
            if run_stage:
                stage()
