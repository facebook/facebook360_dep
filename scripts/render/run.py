#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Entrypoint for distributed render front-end.

The run script currently supports three modes of operation:
    - Single node (i.e. master/worker are the same machine)
    - LAN farms (i.e. master directly communicates to worker nodes in manually configured farm)
    - AWS farms (i.e. master sets up workers via kubernetes)

For users who wish to get results rather than modify the internals, we suggest simply running
this script. Running this script requires having access to Docker.

Example:
    Running run.py will produce a UI if properly executed to facilitate interacting with all
    parts of the pipeline. To run a local render job:

        $ python run.py \
          --project_root=/path/to/project

     To run a LAN farm render job, the master must be specified. If your SMB drive requires
     a username and password, those too must be specified:

        $ python run.py \
          --project_root=smb://192.168.1.100/path/to/project \
          --master=192.168.1.100 \
          --username=user \
          --password=pass123

    To run an AWS farm render job, the path to the credentials CSV and where files will be
    place locally must be specified:

       $ python run.py \
         --project_root=s3://bucket-name/path/to/project \
         --csv_path=/path/to/credentials.csv \
         --cache=/path/to/cache

    To run the viewer through the UI, a path to the directory where the viewer binary lives on the
    host must be specified (with --binary_dir).

Attributes:
    FLAGS (absl.flags._flagvalues.FlagValues): Globally defined flags for run.py.
"""

import os
import posixpath
import sys
import time
from shutil import which

import colorama
import docker
import pyvidia
import requests
from absl import app, flags
from watchdog.events import DirModifiedEvent, FileSystemEventHandler
from watchdog.observers import Observer

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "util"))
sys.path.append(os.path.join(dir_scripts, "ui"))

import config
import glog_check as glog
from network import Address, get_os_type
from scripts.aws.util import AWSUtil
from scripts.ui.project import Project
from scripts.util.system_util import (
    get_flags_from_flagfile,
    image_type_paths,
    OSType,
    run_command,
)
from setup import bin_to_flags, docker_mounts, RepeatedTimer

FLAGS = flags.FLAGS
container_name = None
colorama.init(autoreset=True)


class ViewerHandler(FileSystemEventHandler):

    """Handles events triggered for displaying the viewer if called from within the UI.

    Attributes:
        local_project_root (str): Path of the project on the host.
    """

    def __init__(self, local_project_root):
        """Initializes an event handler for viewers.

        Args:
            local_project_root (str): Path of the project on the host.
        """
        self.local_project_root = local_project_root

    def get_fused_json(self, fused_dir):
        for file in os.listdir(fused_dir):
            if file.endswith("_fused.json"):
                return file
        return None

    def get_render_flags(self, type):
        flags_dir = posixpath.join(self.local_project_root, "flags")
        flagfile_fn = posixpath.join(flags_dir, f"render_{type}.flags")
        flags_render = get_flags_from_flagfile(flagfile_fn)
        return flags_render

    def on_modified(self, event):
        """When a viewer file is modified from the UI, the appropriate viewer runs on the host.

        Args:
            event (watchdog.FileSystemEvent): Watchdog event for when viewer file has been modified.
        """
        if isinstance(event, DirModifiedEvent):
            return

        ipc_name = os.path.basename(event.src_path)
        host_os = get_os_type(config.LOCALHOST)
        if ipc_name == config.DOCKER_RIFT_VIEWER_IPC and host_os != OSType.WINDOWS:
            print(glog.yellow("RiftViewer is only supported on Windows!"))
            return

        app_name = config.get_app_name(ipc_name)
        if not app_name:
            print(glog.red(f"Invalid IPC name: {ipc_name}"))
            return

        try:
            output_dir = posixpath.join(
                self.local_project_root, config.OUTPUT_ROOT_NAME
            )
            if ipc_name == config.DOCKER_RIFT_VIEWER_IPC:
                fused_dir = posixpath.join(output_dir, image_type_paths["fused"])
                fused_json = self.get_fused_json(fused_dir)
                if not fused_json:
                    print(glog.red(f"Cannot find fused rig json in {fused_dir}"))
                    return
                cmd_flags = f"""--rig={posixpath.join(fused_dir, fused_json)} \
                    --catalog={posixpath.join(fused_dir, "fused.json")} \
                    --strip_files={posixpath.join(fused_dir, "fused_0.bin")}"""
            elif ipc_name in [config.DOCKER_SMR_IPC, config.DOCKER_SMR_ONSCREEN_IPC]:
                flags_render = self.get_render_flags("export")

                if ipc_name == config.DOCKER_SMR_IPC:
                    flags_render["output"] = posixpath.join(
                        output_dir, image_type_paths["exports"]
                    )
                flags_smr = [flag["name"] for flag in bin_to_flags[app_name]]

                cmd_flags = ""
                ignore_onscreen = ["format", "output"]
                for flag in flags_render:
                    if flag in flags_smr:
                        if (
                            flag in ignore_onscreen
                            and ipc_name == config.DOCKER_SMR_ONSCREEN_IPC
                        ):
                            continue
                        cmd_flags += f" --{flag}={flags_render[flag]}"
                cmd_flags = cmd_flags.replace(
                    config.DOCKER_INPUT_ROOT, self.local_project_root
                )

            cmd = f"{posixpath.join(FLAGS.local_bin, app_name)} {cmd_flags}"
            if os.name != "nt":  # GLOG initiatives don't work in Powershell/cmd
                cmd = f"GLOG_alsologtostderr=1 GLOG_stderrthreshold=0 {cmd}"
            run_command(cmd)
        except Exception as e:
            print(glog.red(e))


def start_registry(client):
    """Starts a local Docker registry. If a registry already exists, nothing happens.

    Args:
        client (DockerClient): Docker client configured to the host environment.
    """
    try:
        client.containers.run(
            "registry:2",
            detach=True,
            name="registry",
            ports={config.DOCKER_REGISTRY_PORT: config.DOCKER_REGISTRY_PORT},
            restart_policy={"name": "always"},
        )
    except docker.errors.APIError:
        pass


def build(client, docker_img):
    """Builds the Docker image.

    Args:
        client (DockerClient): Docker client configured to the host environment.
        docker_img (str): Name of the Docker image.

    Raises:
        Exception: If Docker encounters an issue during the build.
    """
    try:
        print(glog.green("Preparing context"), end="")
        loading_context = RepeatedTimer(1, lambda: print(glog.green("."), end=""))
        building_image = False
        for line in client.api.build(
            path=os.path.dirname(os.path.abspath(FLAGS.dockerfile)),
            dockerfile=FLAGS.dockerfile,
            tag=docker_img,
            decode=True,
        ):
            if "stream" in line:
                if FLAGS.verbose:
                    loading_context.stop()
                    print(line["stream"].strip())
                elif not building_image:
                    print(glog.green("\nBuilding Docker image"), end="")
                    building_image = True
            if "error" in line:
                raise Exception(line["error"])
    except requests.exceptions.ConnectionError:
        print(glog.red("\nError: Docker is not running!"))
        loading_context.stop()
        exit(1)
    if not FLAGS.verbose:
        loading_context.stop()
        print("")  # force newline


def push(client, docker_img):
    """Pushes the Docker image to the local registry.

    Args:
        client (DockerClient): Docker client configured to the host environment.
        docker_img (str): Name of the Docker image.

    Raises:
        Exception: If Docker encounters an issue during the push.
    """
    for line in client.api.push(docker_img, stream=True, decode=True):
        if "status" in line:
            if "progress" in line:
                print(f"{line['status']}: {line['progress']}")
            else:
                print(line["status"])
        if "error" in line:
            raise Exception(line["error"])


def create_viewer_watchdog(client, ipc_dir, local_project_root):
    """Sets up the Watchdog to monitor the files used to remotely call the viewers.

    Args:
        client (DockerClient): Docker client configured to the host environment.
        ipc_dir (str): Directory where the files used to signal to the Watchdog reside.
        local_project_root (str): Path of the project on the host.
    """
    for ipc in config.DOCKER_IPCS:
        ipc_callsite = os.path.join(local_project_root, config.IPC_ROOT_NAME, ipc)
        with open(ipc_callsite, "w"):
            os.utime(ipc_callsite, None)

    event_handler = ViewerHandler(local_project_root)
    observer = Observer()
    observer.schedule(event_handler, path=ipc_dir, recursive=False)
    observer.start()

    global container_name
    try:
        while True:
            time.sleep(1)
            container = client.containers.get(container_name)
            if container.status == "exited":
                observer.stop()
                break
            for line in container.logs(stream=True):
                print(line.decode("utf8").strip())
    except KeyboardInterrupt:
        container = client.containers.get(container_name)
        container.stop()
        observer.stop()
    observer.join()


def run_ui(client, docker_img):
    """Starts the UI.

    Args:
        client (DockerClient): Docker client configured to the host environment.
        docker_img (str): Name of the Docker image.
    """
    if not FLAGS.verbose:
        print(glog.green("Initializing container"), end="")
        loading_context = RepeatedTimer(1, lambda: print(glog.green("."), end=""))

    host_os = get_os_type(config.LOCALHOST)

    # Setup steps for X11 forwarding vary slightly per the host operating system
    volumes = {"/var/run/docker.sock": {"bind": "/var/run/docker.sock", "mode": "ro"}}
    if host_os == OSType.MAC or host_os == OSType.LINUX:
        volumes.update({"/tmp/.X11-unix": {"bind": "/tmp/.X11-unix", "mode": "ro"}})

    if host_os == OSType.MAC or host_os == OSType.LINUX:
        run_command(f"xhost + {config.LOCALHOST}", run_silently=not FLAGS.verbose)
    if host_os == OSType.LINUX:
        run_command(
            f"xhost + {config.DOCKER_LOCAL_HOSTNAME}", run_silently=not FLAGS.verbose
        )

    host_to_docker_path = {FLAGS.project_root: config.DOCKER_INPUT_ROOT}

    project = Project(
        FLAGS.project_root,
        FLAGS.cache,
        FLAGS.csv_path,
        FLAGS.s3_sample_frame,
        FLAGS.s3_ignore_fullsize_color,
        FLAGS.verbose,
    )
    project.verify()

    cmds = [
        "cd scripts/ui",
        f"""python3 -u dep.py \
        --host_os={get_os_type(config.LOCALHOST)} \
        --local_bin={FLAGS.local_bin} \
        --master={FLAGS.master} \
        --password={FLAGS.password} \
        --project_root={FLAGS.project_root} \
        --s3_ignore_fullsize_color={FLAGS.s3_ignore_fullsize_color} \
        --s3_sample_frame={FLAGS.s3_sample_frame} \
        --username={FLAGS.username} \
        --verbose={FLAGS.verbose}""",
    ]

    docker_networks = client.networks.list()
    network_names = [docker_network.name for docker_network in docker_networks]
    if config.DOCKER_NETWORK not in network_names:
        client.networks.create(config.DOCKER_NETWORK, driver="bridge")

    project_address = Address(FLAGS.project_root)
    project_protocol = project_address.protocol
    if project_protocol == "smb":
        mounts = docker_mounts(
            FLAGS.project_root, host_to_docker_path, FLAGS.username, FLAGS.password
        )
        cmds = [f"mkdir {config.DOCKER_INPUT_ROOT}"] + mounts + cmds

        local_project_root = None
    elif project_protocol == "s3":
        glog.check_ne(
            FLAGS.csv_path, "", "csv_path cannot be empty if rendering on AWS"
        )
        aws_util = AWSUtil(FLAGS.csv_path, s3_url=FLAGS.project_root)
        glog.check(
            aws_util.s3_bucket_is_valid(FLAGS.project_root),
            f"Invalid S3 project path: {FLAGS.project_root}",
        )

        volumes.update(
            {FLAGS.csv_path: {"bind": config.DOCKER_AWS_CREDENTIALS, "mode": "rw"}}
        )

        project_name = project_address.path
        cache_path = os.path.join(os.path.expanduser(FLAGS.cache), project_name)
        os.makedirs(cache_path, exist_ok=True)
        volumes.update({cache_path: {"bind": config.DOCKER_INPUT_ROOT, "mode": "rw"}})

        local_project_root = cache_path
    else:
        glog.check(
            os.path.isdir(FLAGS.project_root),
            f"Invalid project path: {FLAGS.project_root}",
        )
        volumes.update(
            {
                host_path: {"bind": docker_path, "mode": "rw"}
                for host_path, docker_path in host_to_docker_path.items()
            }
        )
        local_project_root = FLAGS.project_root

    ipc_dir = os.path.join(local_project_root, "ipc")
    os.makedirs(ipc_dir, exist_ok=True)
    volumes.update({ipc_dir: {"bind": config.DOCKER_IPC_ROOT, "mode": "rw"}})

    cmd = f'/bin/bash -c "{" && ".join(cmds)}"'
    global container_name
    display = ":0" if host_os == OSType.LINUX else "host.docker.internal:0"
    runtime = "nvidia" if which("nvidia-docker") else ""
    if host_os != OSType.LINUX:
        display = "host.docker.internal:0"

    if not FLAGS.verbose:
        loading_context.stop()
        print("")

    try:
        container = client.containers.run(
            docker_img,
            command=cmd,
            detach=True,
            environment={"DISPLAY": display},
            runtime=runtime,
            network=config.DOCKER_NETWORK,
            ports={
                config.RABBITMQ_PORT: config.RABBITMQ_PORT,
                config.RABBITMQ_MANAGE_PORT: config.RABBITMQ_MANAGE_PORT,
            },
            privileged=True,
            volumes=volumes,
            stderr=True,
        )
    except docker.errors.APIError as e:
        if "port is already allocated" in str(e):
            raise Exception(
                "Failed to launch UI! Ensure: \n"
                "(1) No other instance of UI is running (check: docker ps) and\n"
                "(2) RabbitMQ is not running on your machine (check: ps aux | grep 'rabbitmq')"
            ) from None
        raise e
    container_name = container.name
    create_viewer_watchdog(client, ipc_dir, local_project_root)


def setup_local_gpu():
    # Check if we are using Linux and we have an NVIDIA card, and we are not rendering in AWS
    if not FLAGS.project_root.startswith("s3://"):
        host_os = get_os_type(config.LOCALHOST)
        if host_os == OSType.LINUX and pyvidia.get_nvidia_device() is not None:
            gpu_script = os.path.join(dir_scripts, "render", "setup_gpu.sh")
            print(glog.green("Setting up GPU environment..."))
            run_command(f"/bin/bash {gpu_script}", run_silently=not FLAGS.verbose)
        else:
            print(
                glog.yellow(
                    "We can only access an Nvidia GPU from a Linux host. Skipping Docker GPU setup"
                )
            )


def main(argv):
    """Builds the Docker container corresponding to the project and starts the UI.
    Upon exiting the UI, the script will terminate. Similarly, exiting the script
    will terminate the UI.

    Args:
        argv (list[str]): List of arguments (used interally by abseil).
    """

    FLAGS.cache = os.path.expanduser(FLAGS.cache)
    FLAGS.csv_path = os.path.expanduser(FLAGS.csv_path)
    FLAGS.dockerfile = os.path.expanduser(FLAGS.dockerfile)
    FLAGS.local_bin = os.path.expanduser(FLAGS.local_bin)
    FLAGS.project_root = os.path.expanduser(FLAGS.project_root)

    setup_local_gpu()
    client = docker.from_env()
    docker_img = f"localhost:{config.DOCKER_REGISTRY_PORT}/{config.DOCKER_IMAGE}"
    if not FLAGS.skip_setup:
        build(client, docker_img)
        if FLAGS.master != config.LOCALHOST:
            start_registry(client)
            push(client, docker_img)
    run_ui(client, docker_img)


if __name__ == "__main__":
    flags.DEFINE_string(
        "cache", "~/cache", "local directory where sample files are cached"
    )
    flags.DEFINE_string("csv_path", "", "path to AWS credentials CSV")
    flags.DEFINE_string("dockerfile", "Dockerfile", "Path to the Dockerfile")
    flags.DEFINE_string(
        "local_bin", "", "Path local binaries (needed to run GPU-based viewers)"
    )
    flags.DEFINE_string(
        "master", config.LOCALHOST, "IP of the master (only local farm)"
    )
    flags.DEFINE_string(
        "password", "", "Username for mounted network drive (only local farm)"
    )
    flags.DEFINE_string("project_root", None, "Input root of the project (required)")
    flags.DEFINE_boolean(
        "s3_ignore_fullsize_color", False, "Do not download full-size color from S3"
    )
    flags.DEFINE_string("s3_sample_frame", "", "Sample frame to download from S3")
    flags.DEFINE_boolean("skip_setup", False, "Skip docker build step")
    flags.DEFINE_string(
        "username", "", "Password for mounted network drive (only local farm)"
    )
    flags.DEFINE_boolean("verbose", False, "Verbose mode")

    # Required FLAGS.
    flags.mark_flag_as_required("project_root")
    app.run(main)
