#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Constants used across all scripts.

Attributes:
    AUTO_TERMINATE_CPU (float): Threshold for auto-termination of kubernetes cluster (if sustained
        for a prolonged period).
    DOCKER_AWS_CREDENTIALS (str): Path in Docker container where AWS credentials saved.
    DOCKER_BUILD_ROOT (str): Path in Docker container where binaries are built.
    DOCKER_CONTAINER (str): Name of the Docker container.
    DOCKER_DAEMON_JSON (str): Default name of the Docker daemon config json.
    DOCKER_FLAGS_ROOT (str): Path in Docker container where flags are saved.
    DOCKER_IMAGE (str): Name of the Docker image.
    DOCKER_INPUT_ROOT (str): Path in Docker container for the root of input files.
    DOCKER_IPC_ROOT (str): Path in Docker container where host communication files are saved.
    DOCKER_LOCAL_HOSTNAME (str): Default hostname used for xhost.
    DOCKER_LOCALHOST (str): Default name used for referencing the host from within Docker.
    DOCKER_NETWORK (str): Name of the network the created container attaches to.
    DOCKER_OUTPUT_ROOT (str): Path in Docker container for the root of output files.
    DOCKER_REGISTRY_PORT (int): Port on host on which the local registry runs.
    DOCKER_RIFT_VIEWER_IPC (str): File used to call RiftViewer on the host.
    DOCKER_ROOT (str): Path in Docker container corresponding to home.
    DOCKER_SCRIPTS_ROOT (str): Path in Docker container for the root of script files.
    DOCKER_SMR_IPC (str): File used to call SimpleMeshRenderer on the host.
    DOCKER_SMR_ONSCREEN_IPC (str): File used to call onscreen SimpleMeshRenderer on the host.
    LOCALHOST (str): Default IP for localhost.
    NETCAT_PORT (int): Port on host on which netcat runs.
    POLLING_INTERVAL (int): Time bound before auto-terminating kubernetes cluster.
    QUEUE_NAME (str): Name of the RabbitMQ queue used for work to be completed.
    RABBITMQ_MANAGE_PORT (int): Port on host on which RabbitMQ management runs.
    RABBITMQ_PORT (int): Port on host on which RabbitMQ runs.
    RESPONSE_QUEUE_NAME (str): Name of the RabbitMQ queue used for completed work.
    type_to_levels_type (dict[str, str]): Map from image type to level image type.
    type_to_upsample_type (dict[str, str]): Map from image type to upsampled image type.
    WIDTHS (list[int]): Fixed list of widths used in depth estimation.
"""

import hashlib

# Predetermined widths of levels
WIDTHS = [2048, 1024, 512, 256, 200, 128, 100, 80, 60, 50]

LOCALHOST = "127.0.0.1"
DOCKER_LOCAL_HOSTNAME = "local:docker"
DOCKER_LOCALHOST = "host.docker.internal"
RESPONSE_QUEUE_NAME = "facebook360_dep_response"
QUEUE_NAME = "facebook360_dep"

AWS_ROOT_NAME = "project/aws"
BUILD_ROOT_NAME = "build"
FLAGS_ROOT_NAME = "res/flags"
INPUT_ROOT_NAME = "project"
IPC_ROOT_NAME = "ipc"
SCRIPTS_ROOT_NAME = "scripts"

OUTPUT_ROOT_NAME = "video"  # lies in INPUT_ROOT
RIGS_ROOT_NAME = "rigs"  # lies in INPUT_ROOT

DOCKER_NETWORK = "facebook360_dep"
DOCKER_ROOT = "/app/facebook360_dep"
DOCKER_AWS_ROOT = f"{DOCKER_ROOT}/{AWS_ROOT_NAME}"
DOCKER_AWS_CREDENTIALS = f"{DOCKER_AWS_ROOT}/credentials.csv"
DOCKER_AWS_WORKERS = f"{DOCKER_AWS_ROOT}/workers.txt"
DOCKER_BUILD_ROOT = f"{DOCKER_ROOT}/{BUILD_ROOT_NAME}"
DOCKER_FLAGS_ROOT = f"{DOCKER_ROOT}/{FLAGS_ROOT_NAME}"
DOCKER_INPUT_ROOT = f"{DOCKER_ROOT}/{INPUT_ROOT_NAME}"
DOCKER_IPC_ROOT = f"{DOCKER_ROOT}/{IPC_ROOT_NAME}"
DOCKER_SCRIPTS_ROOT = f"{DOCKER_ROOT}/{SCRIPTS_ROOT_NAME}"
DOCKER_OUTPUT_ROOT = f"{DOCKER_INPUT_ROOT}/{OUTPUT_ROOT_NAME}"

# Need extremely unlikely filename for the IPC file the watchdog monitors
DOCKER_RIFT_VIEWER_IPC = hashlib.md5("rift_viewer.ipc".encode("utf-8")).hexdigest()
DOCKER_SMR_IPC = hashlib.md5("smr.ipc".encode("utf-8")).hexdigest()
DOCKER_SMR_ONSCREEN_IPC = hashlib.md5("smr_onscreen.ipc".encode("utf-8")).hexdigest()
DOCKER_IPCS = [DOCKER_RIFT_VIEWER_IPC, DOCKER_SMR_IPC, DOCKER_SMR_ONSCREEN_IPC]

DOCKER_CONTAINER = "worker"
DOCKER_IMAGE = "worker"
DOCKER_DAEMON_JSON = "daemon.json"

POLLING_INTERVAL = 1200  # Check for auto-terminate condition every 20 minutes
AUTO_TERMINATE_CPU = 0.05
DOCKER_REGISTRY_PORT = 5000
NETCAT_PORT = 9000
RABBITMQ_PORT = 5672
RABBITMQ_MANAGE_PORT = 15672
NO_WORKER_TIMEOUT = 180  # Checks to make sure no 3 minute stretch has 0 workers

EC2_UNSUPPORTED_TYPES = (
    "m1",
    "m3",
    "t1",
    "c1",
    "c3",
    "c4.l",
    "c4.x",
    "c4.2x",
    "c5.l",
    "c5.x",
    "c5d.l",
    "c5d.x",
    "c5n.l",
    "cc",
    "cg",
    "cr1",
    "m2",
    "r3",
    "d2",
    "hs1",
    "i2",
    "g2",
)

type_to_levels_type = {
    "background_disp": "background_disp_levels",
    "background_color": "background_color_levels",
    "color": "color_levels",
    "disparity": "disparity_levels",
    "disparity_time_filtered": "disparity_time_filtered_levels",
    "foreground_masks": "foreground_masks_levels",
}

type_to_upsample_type = {
    "disparity": "disparity_upsample",
    "background_disp": "background_disp_upsample",
}


def get_app_name(ipc_name):
    if ipc_name == DOCKER_RIFT_VIEWER_IPC:
        return "RiftViewer"
    elif ipc_name in [DOCKER_SMR_IPC, DOCKER_SMR_ONSCREEN_IPC]:
        return "SimpleMeshRenderer"
    else:
        return None
