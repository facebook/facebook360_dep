#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Network utility for the render pipeline.

This utility provides a uniform abstraction for interacting across various interfaces. The
current functionality supports S3, SMB, and local Posix interfaces. For each of these,
utilities for downloading, uploading, moving, and locating files (per the standard file
structure) are made available. Classes defined here can be extended to support additional
endpoints if so desired.
"""

import glob
import json
import multiprocessing
import os
import random
import socket
import string
import subprocess
import sys
import tarfile
from difflib import SequenceMatcher
from shutil import copyfile, rmtree

import netifaces

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "util"))

import config
from scripts.util.system_util import get_os_type_local, image_type_paths, run_command


class Address:

    """Abstraction over various address types.

    Attributes:
        All the below attributes are premised on the example: s3://bucket/example/endpoint

        address (str): The full address: s3://bucket/example/endpoint.
        ip (str): The IP in the address: bucket.
        ip_path (str): The full path within the specified protocol: bucket/example/endpoint.
        path (str): The path within the specified IP: example/endpoint.
        protocol (str): The "type" of address passed in: s3.
    """

    def __init__(self, address):
        """Initializes an Address object given a full path. This path can be a local Posix
        path, SMB path, or S3. Other paths may work but remain untested.

        Args:
            address (str): Full network path.
        """
        protocol_delim = "://"

        if protocol_delim in address:
            self.address = address
            self.protocol, self.ip_path = address.split("://")
            self.ip, self.path = self.ip_path.split("/", 1)
        else:
            self.address = address
            self.ip_path = address
            self.protocol = None
            self.ip = None
            self.path = address


class NetcatClient:

    """A client for running commands synchronously over netcat.

    Attributes:
        hostname (str): IP of the host running the netcat server.
        port (int): Port on which the netcat server is running.
    """

    def __init__(self, hostname, port):
        """Establishes a client that can be used to execute commands over netcat.

        Args:
            hostname (str): IP of the host running the netcat server.
            port (int): Port on which the netcat server is running.
        """
        self.hostname = hostname
        self.port = port

    def run(self, cmds):
        """Synchronously runs a series of commands over netcat.

        Args:
            cmds (list[str]): Series of commands to run. All commands will run regardless
                of failures of any.

        Returns:
            str: stdout from command execution.
        """
        # Need some token to represent end of command execution
        end_token = "".join(random.choice(string.ascii_lowercase) for _ in range(10))

        cmds.append(f"echo {end_token}")
        cmd = "\n".join(cmds) + "\n"
        result = []

        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((self.hostname, self.port))
        s.send(cmd.encode("utf8"))
        while True:
            data = s.recv(1024).strip().decode("utf8")
            result.append(data.replace(end_token, ""))
            if end_token in data:
                break

        return "\n".join(result)

    def run_script(self, path):
        filename = os.path.basename(path)

        # File is "sent" by creating a file in destination and appending lines to it
        cmds = [f"touch {filename}"]
        with open(path) as f:
            for line in f:
                cmds.append(f'echo "{line.strip()}" >> {filename}')
        cmds.append(f"/bin/bash {filename}")
        self.run(cmds)

    def run_async(self, cmds):
        """Asynchronously runs a series of commands over netcat.

        Args:
            cmds (list[str]): Series of commands to run. All commands will run regardless
                of failures of any.
        """
        cmd = "\n".join(cmds) + "\n"
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((self.hostname, self.port))
        s.send(cmd.encode("utf8"))


class LAN:

    """Client for interacting with a local network.

    Attributes:
        lan_ips (list): List of IPs in the LAN.
        local_ip (TYPE): IP for local machine in the LAN.
        subnet (TYPE): Subnet on which LAN resides.
    """

    def __init__(self, subnet="192.168.1.255"):
        """Creates a client for interacting with machines on the LAN.

        Args:
            subnet (str, optional): Subnet on which LAN resides.
        """
        self.subnet = subnet
        self.lan_ips = None
        self.local_ip = None

    def _pinger(self, job_queue, results_queue):
        """Completes jobs for pinging across the subnet and feeds results to the queue.

        Args:
            job_queue (multiprocessing.Queue): Queue with jobs to complete.
            results_queue (multiprocessing.Queue): Queue for results.
        """
        while True:
            ip = job_queue.get()
            if ip is None:
                break

            try:
                with open(os.devnull, "w") as fp:
                    subprocess.check_call(["ping", "-c1", ip], stdout=fp)
                    results_queue.put(ip)
            except Exception:
                pass

    def scan(self):
        """Scans the subnet for ping-able machines and returns IPs.

        Returns:
            list[str]: IPs of reachable machines on the subnet.
        """
        if self.lan_ips is not None:
            return self.lan_ips

        # prepare the jobs queue
        jobs = multiprocessing.Queue()
        results = multiprocessing.Queue()

        pool = [
            multiprocessing.Process(target=self._pinger, args=(jobs, results))
            for _ in range(255)
        ]

        ip_begin, _ = self.subnet.rsplit(".", 1)

        for p in pool:
            p.start()

        for i in range(1, 255):
            jobs.put(f"{ip_begin}.{i}")

        for _ in pool:
            jobs.put(None)

        for p in pool:
            p.join()

        self.lan_ips = []
        while not results.empty():
            ip = results.get()
            self.lan_ips.append(ip)
        return self.lan_ips

    def get_local_ip(self):
        """Finds IP address of the local machine on the LAN.

        Returns:
            str: IP address of the local machine.
        """
        if self.local_ip is not None:
            return self.local_ip

        interfaces = netifaces.interfaces()
        master_candidates = []
        for interface in interfaces:
            addresses = netifaces.ifaddresses(interface)
            for address in addresses:
                addr_broadcast_info = addresses[address]
                for addr_broadcast in addr_broadcast_info:
                    if (
                        "broadcast" in addr_broadcast
                        and addr_broadcast["broadcast"] == self.subnet
                    ):
                        master_candidates.append(addr_broadcast["addr"])
        if len(master_candidates) > 0:
            self.local_ip = master_candidates[0]
        return self.local_ip


def get_os_type(ip):
    """Determines the operating system of the machine assuming it can be reached.

    Args:
        ip (str): IP of the machine in interest.

    Returns:
        OSType: Type of operating system.

    Raises:
        Exception: If the operating system cannot be properly determined as being
            Linux, Mac OS X, or Windows.
    """
    if ip == config.LOCALHOST:
        platform = sys.platform
    else:
        nc = NetcatClient(ip, config.NETCAT_PORT)
        platform = nc.run(["echo $OSTYPE"]).lower()
    return get_os_type_local(platform)


def _get_image_root_type(image_type):
    """Determines which root an image type is located in.

    Args:
        image_type (str): Type of image.

    Returns:
        str: Either "input" or "output" for the corresponding root.
    """
    inputs = {
        "background_color",
        "background_color_levels",
        "background_disp",
        "background_disp_levels",
        "background_disp_upsample",
        "color",
        "color_levels",
        "foreground_masks",
        "foreground_masks_levels",
    }
    outputs = {
        "bin",
        "confidence",
        "cost",
        "disparity",
        "disparity_upsample",
        "disparity_levels",
        "disparity_time_filtered",
        "disparity_time_filtered_levels",
        "fused",
        "mismatches",
        "exports",
        "exports_cubecolor",
        "exports_cubedisp",
        "exports_eqrcolor",
        "exports_eqrdisp",
        "exports_lr180",
        "exports_tb3dof",
        "exports_tbstereo",
    }
    root_map = dict.fromkeys(inputs, "input")
    root_map.update(dict.fromkeys(outputs, "output"))
    return root_map[image_type]


def get_frame_name(frame):
    """Gets the frame name for a frame number.

    Args:
        frame (int): Frame number.

    Returns:
        str: 0-padded frame name (with length 6).
    """
    return str(frame).rjust(6, "0")


def get_frame_range(first, last):
    """Gets list of frame names within the range specified.

    Args:
        first (str): Name of the first frame.
        last (str): Name of the last frame.

    Returns:
        list[str]: Names of the frames between the specified frames (includes first, excludes last).
    """
    return [get_frame_name(frame) for frame in range(int(first), int(last) + 1)]


def get_frame_fns(msg, frames, uncompressed=False, src=None):
    if uncompressed:
        if msg["app"].startswith("SimpleMeshRenderer"):
            img_ext = msg["file_type"]
            frame_fns = [f"{frame}.{img_ext}" for frame in frames]
        else:
            cam_dirs = next(os.walk(src))[1]
            if msg["app"].startswith("ConvertToBinary") or msg["app"].startswith(
                "DerpCLI"
            ):
                img_exts = [f".{ft}" for ft in msg["output_formats"].split(",")]
            else:
                sample_cam_src = os.path.join(src, next(iter(cam_dirs)))
                sample_file = get_sample_file(sample_cam_src)
                _, img_ext = os.path.splitext(sample_file)
                img_exts = [img_ext]
            frame_fns = []
            for img_ext in img_exts:
                frame_fns.extend(
                    os.path.join(cam_dir, f"{frame}{img_ext}")
                    for frame in frames
                    for cam_dir in cam_dirs
                )
    else:
        frame_fns = [f"{frame}.tar" for frame in frames]
    return list(set(frame_fns))


def _netop_helper(netop, src, dst, frame_fns):
    """Generic helper function for performing operations over a network. Currently supports S3.

    Args:
        netop (func: str -> bool): Network function that operates on a URL and returns success.
        src (str): Source path (remote).
        dst (str): Destination path (local).
        frame_fns (list[str]): Filenames to be downloaded or uploaded.

    Returns:
        bool: Success of network operation completion.
    """
    # Both list and string (resp. for multi and single frame) calls are supported
    completed = True
    for frame_fn in frame_fns:
        completed &= netop(os.path.join(src, frame_fn), os.path.join(dst, frame_fn))
    return completed


def download_rig(msg):
    """Downloads rig to the appropriate local path.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.

    Returns:
        bool: Success of download.
    """
    return download(
        src=msg["rig"],
        dst=msg["rig"].replace(msg["input_root"], config.DOCKER_INPUT_ROOT),
        filters=["*.json"],
    )


def get_cameras(msg, dst_field="cameras"):
    """Gets the cameras pertinent to the render.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
        dst_field (str, optional): Field mapping to cameras in msg.

    Returns:
        list[str]: List of camera names.
    """
    if msg[dst_field] == "":
        download_rig(msg)
        with open(local_rig_path(msg), "r") as f:
            rig = json.load(f)
        return [camera["id"] for camera in rig["cameras"]]
    return msg[dst_field].split(",")


def download_image_type(msg, image_type, frames, level=None):
    """Downloads frames of an image type to the appropriate local path.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
        image_type (str): Name of an image type (re: source/util/ImageTypes.h).
        frames (list[str]): List of frame names to download.
        level (None, optional): Level to download. If None is passed in, the full-size
            image is downloaded.

    Returns:
        bool: Success of download.
    """
    src = remote_image_type_path(msg, image_type, level)
    remote = Address(src)
    if remote.protocol == "s3":
        src = remote_image_type_path(msg, image_type, level)
        dst = local_image_type_path(msg, image_type, level)

        frame_tar_fns = get_frame_fns(msg, frames)
        downloaded_frames = _netop_helper(download, src, dst, frame_tar_fns)

        if downloaded_frames:
            for frame_tar_fn in frame_tar_fns:
                dst_fn = os.path.join(dst, frame_tar_fn)
                tar_ref = tarfile.open(dst_fn)
                tar_ref.extractall(dst)
                tar_ref.close()
    return False


def download_image_types(msg, image_type_to_level, frames=None):
    """Downloads frames of image types to the appropriate local paths.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
        image_type_to_level (dict[str, int]): Map of image type to level to download.
        frames (list[str], optional): List of frame names to download. If None is passed in,
            the range from [first, last) are downloaded (extracted from msg).

    Returns:
        bool: Success of download.
    """
    ran_download = False
    if frames is None:
        frames = get_frame_range(msg["first"], msg["last"])
    for image_type, level in image_type_to_level:
        ran_download |= download_image_type(msg, image_type, frames, level)
    return ran_download


def tar_frame(src, frame, tar_h):
    """Tars a directory.
    Args:
        path (str): Path to the directory to be packed.
        frame (str): Name of the frame (6 digit, zero padded)
        tar_h (file handle): File handle to the tar file to be written.
    """
    for root, _, files in os.walk(src):
        for file in files:
            if frame in file:
                arcname = os.path.join(os.path.basename(root), file)
                tar_h.add(os.path.join(root, file), arcname=arcname)


def tar_frames(src, frames):
    """Tars a frame.
    Args:
        src (str): Path to the directory with frames to be packed.
        frames (list[str]): List of frame names (6 digit, zero padded)
    """
    for frame in frames:
        frame_tar_fn = os.path.join(src, f"{frame}.tar")
        tar_file = tarfile.open(frame_tar_fn, "w")
        tar_frame(src, frame, tar_file)
        tar_file.close()


def upload_image_type(msg, image_type, frames=None, level=None):
    """Uploads frames of an image type to the appropriate local path.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
        image_type (str): Name of an image type (re: source/util/ImageTypes.h).
        frames (list[str], optional): List of frame names to upload. If None is passed in,
            the range from [first, last) are uploaded (extracted from msg).
        level (int, optional): Level to upload. If None is passed in, the full-size
            image is uploaded.

    Returns:
        bool: Success of upload.
    """
    src = local_image_type_path(msg, image_type, level)
    dst = remote_image_type_path(msg, image_type, level)

    if frames is None:
        frames = get_frame_range(msg["first"], msg["last"])

    remote = Address(dst)
    if remote.protocol == "s3":
        uploaded = True
        uncompressed = "exports" in image_type
        if not uncompressed:
            tar_frames(src, frames)
        frame_fns = get_frame_fns(msg, frames, uncompressed, src)
        uploaded &= _netop_helper(upload, src, dst, frame_fns)
        return uploaded
    return False


def copy_image_level(
    msg,
    src_image_type,
    dst_image_type,
    cameras,
    frames,
    src_level=None,
    dst_level=None,
    uncompressed=False,
):
    """Copies frames and cameras of an image type to a new type.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
        src_image_type (str): Image type of the source.
        dst_image_type (str): Image type of the destination.
        cameras (list[str]): List of cameras to be copied.
        frames (list[str]): List of frame names to be copied.
        src_level (int, optional): Image level of the source. If None is passed in, the
            location of the full-size images is used.
        dst_level (int, optional): Image level of the destination. If None is passed in, the
            location of the full-size images is used.
        uncompressed (bool, optional): Whether or not the src should be uncompresseded before
            copying to the destination. Only relevant for archived intermediates.
    """
    src = remote_image_type_path(msg, src_image_type, src_level)
    dst = remote_image_type_path(msg, dst_image_type, dst_level)
    for frame in frames:
        copy_frame(src, dst, frame, cameras, uncompressed=uncompressed)


def local_rig_path(msg):
    """Gets local path of the rig.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.

    Returns:
        str: Path to local rig.
    """
    return msg["rig"].replace(msg["input_root"], config.DOCKER_INPUT_ROOT)


def local_image_type_path(msg, image_type, level=None):
    """Gets local path to the directory for an image type.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
        image_type (str): Name of an image type (re: source/util/ImageTypes.h).
        level (int, optional): Image level to be returned. If None is passed in, the
            location of the full-size images is returned.

    Returns:
        str: Path to local image type directory.
    """
    if level is not None:
        image_type = config.type_to_levels_type[image_type]

    image_root_type = _get_image_root_type(image_type)
    image_root = (
        config.DOCKER_INPUT_ROOT
        if image_root_type == "input"
        else config.DOCKER_OUTPUT_ROOT
    )
    image_type_dir = os.path.join(image_root, image_type_paths[image_type])
    if level is None:
        return image_type_dir
    return os.path.join(image_type_dir, f"level_{level}")


def remote_image_type_path(msg, image_type, level=None):
    """Gets remote path to the directory for an image type.

    Args:
        msg (dict[str, str]): Message received from RabbitMQ publisher.
        image_type (str): Name of an image type (re: source/util/ImageTypes.h).
        level (int, optional): Image level to be returned. If None is passed in, the
            location of the full-size images is returned.

    Returns:
        str: Path to remote image type directory.
    """
    if level is not None:
        image_type = config.type_to_levels_type[image_type]

    image_root_type = _get_image_root_type(image_type)
    image_root = msg["input_root"] if image_root_type == "input" else msg["output_root"]
    image_type_dir = os.path.join(image_root, image_type_paths[image_type])

    if level is None:
        return image_type_dir
    return os.path.join(image_type_dir, f"level_{level}")


def download(src, dst, filters=None, run_silently=False):
    """Recursively downloads the objects from source that adhere to the filters. No
    operation is performed for local paths.

    Args:
        src (str): Path to the source directory.
        dst (str): Path to the destination directory where downloaded files will be saved.
        filters (list[str], optional): List of filters to choose files to download.
        run_silently (bool, optional): Whether or not to display results to stdout.

    Returns:
        bool: Success of download.
    """
    filters = filters if filters is not None else []
    remote = Address(src)
    if remote.protocol == "s3":
        print("Downloading from S3...")
        # Slight difference between downloading a single file and a folder
        if "." in os.path.basename(remote.path):
            download_cmd = f"aws s3 cp {remote.address} {dst}"
        else:
            filter_cmd = "--exclude '*' "
            filter_cmd += " ".join([f"--include '{filter}'" for filter in filters])
            download_cmd = f"aws s3 cp {remote.address} {dst} --recursive {filter_cmd}"
        run_command(download_cmd, run_silently)
        return True
    return False


def upload(src, dst, filters=None, run_silently=False):
    """Recursively uploads the objects from source that adhere to the filters. No
    operation is performed for local paths.

    Args:
        src (str): Path to the source directory.
        dst (str): Path to the destination directory where uploaded files will be saved.
        filters (list[str], optional): List of filters to choose files to upload.
        run_silently (bool, optional): Whether or not to display results to stdout.

    Returns:
        bool: Success of upload.
    """
    filters = filters if filters is not None else []
    remote = Address(dst)

    if remote.protocol == "s3":
        print("Uploading to S3...")

        # Slight difference between downloading a single file and a folder
        if "." in os.path.basename(remote.path):
            upload_cmd = f"aws s3 cp {src} {dst}"
        else:
            filter_cmd = "--exclude '*' "
            filter_cmd += " ".join([f"--include '{filter}'" for filter in filters])
            upload_cmd = f"aws s3 sync {src} {dst} {filter_cmd}"
        run_command(upload_cmd, run_silently)
        return True
    return False


def copy_frame(src, dst, frame, cameras, run_silently=False, uncompressed=False):
    """Copies a single frame to a new directory. A standard cp operation is
    performed for local paths.

    Args:
        src (str): Path to the source directory.
        dst (str): Path to the destination directory.
        frames (list[str]): Frame names being copied.
        run_silently (bool, optional): Whether or not to display results to stdout.
        uncompressed (bool, optional): Whether or not the src should be uncompressed before
            copying to the destination. Only relevant for archived intermediates.
    """
    remote = Address(src)
    if remote.protocol == "s3":
        frame_tar_fn = f"{frame}.tar"
        src_frame = os.path.join(src, frame_tar_fn)
        if uncompressed:
            tmp_extract_dir = "tmp"
            run_command(f"aws s3 cp {src_frame} .", run_silently)

            os.makedirs(tmp_extract_dir, exist_ok=True)
            tar_ref = tarfile.open(frame_tar_fn)
            tar_ref.extractall(tmp_extract_dir)
            tar_ref.close()

            run_command(f"aws s3 cp {tmp_extract_dir} {dst} --recursive", run_silently)
            rmtree(tmp_extract_dir)
            os.remove(frame_tar_fn)
        else:
            dst_frame = os.path.join(dst, frame_tar_fn)
            run_command(f"aws s3 cp {src_frame} {dst_frame}", run_silently)
    else:
        for camera in cameras:
            src_cam = os.path.join(src, camera)
            dst_cam = os.path.join(dst, camera)
            os.makedirs(dst_cam, exist_ok=True)

            # Copy all extensions for current frame
            for f in glob.iglob(f"{src_cam}/{frame}.*", recursive=False):
                f = os.path.basename(f)
                copyfile(os.path.join(src_cam, f), os.path.join(dst_cam, f))


def listdir(src, run_silently=False, recursive=True):
    """Lists objects in a source directory. A standard ls is performed for local paths.

    Args:
        src (str): Path to the source directory.
        run_silently (bool, optional): Whether or not to display results to stdout.

    Returns:
        set[str]: Set of contained filenames.
    """
    remote = Address(src)

    if remote.protocol == "s3":
        if not src.endswith("/"):
            src = src + "/"
        try:
            cmd = f"aws s3 ls {src}"
            if recursive:
                cmd = cmd + "--recursive"
            raw_output = run_command(cmd, run_silently)
        except Exception:
            return set()  # An exception is raised if no such src directory is found
        raw_lines = raw_output.split("\n")
        files = {raw_line.split(" ")[-1] for raw_line in raw_lines}
        return set(filter(None, files))  # Remove empty results
    else:
        result = set()
        if recursive:
            for root, _, files in os.walk(src):
                for file in files:
                    if not file.startswith("."):
                        result.add(os.path.join(os.path.relpath(root, src), file))
        else:
            result = set(os.listdir(src))
        return result


def get_sample_file(src, run_silently=False):
    """Gets the name of a single objects in the source directory. Local paths are
    also supported.

    Args:
        src (str): Path to the source directory.
        run_silently (bool, optional): Whether or not to display results to stdout.

    Returns:
        str: Single filename from the directory. If no such file or directory exists,
            None is returned instead.
    """
    remote = Address(src)

    if remote.protocol == "s3":
        if not src.endswith("/"):
            src = src + "/"
        raw_output = run_command(f"aws s3 ls {src}", run_silently)
        raw_lines = raw_output.split("\n")
        if len(raw_lines) > 0:
            return raw_lines[0].split(" ")[-1]
        return None
    else:
        for root, _, files in os.walk(src):
            for file in files:
                if not file.startswith("."):
                    return os.path.join(root, file)
        return None
