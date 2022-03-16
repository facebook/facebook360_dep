#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Miscellaneous I/O utility functions for setting up the render pipeline.

Abstracts over the setup that has to be done for various configurations of the
farm, namely between over a single node, LAN farm, and AWS farm. It additionally sets up
the appropriate flags to execute render.py. setup.py cannot be run standalone.

Attributes:
    bin_to_flags (dict[str, list[dict[str, _]]]): Map from binary name to corrsponding flags.
    FLAGS (absl.flags._flagvalues.FlagValues): Globally defined flags for render.py. Note that,
        unlike all other apps, the FLAGS here do not directly relate to setup.py.
"""

import datetime
import json
import multiprocessing as mp
import os
import re
import signal
import sys
import traceback
from pathlib import Path
from shutil import which
from subprocess import Popen
from threading import Timer

from absl import flags, logging

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "util"))

import config
from network import Address, get_os_type, NetcatClient
from scripts.util.system_util import get_flags, image_type_paths, OSType, run_command


FLAGS = flags.FLAGS
flag_names = set()
child_pids = []

facebook360_dep_root = str(Path(os.path.abspath(__file__)).parents[2])
source_root = os.path.join(facebook360_dep_root, "source")
depth_est_src = os.path.join(source_root, "depth_estimation")
bin_to_flags = {
    "TemporalBilateralFilter": get_flags(
        os.path.join(depth_est_src, "TemporalBilateralFilter.cpp")
    ),
    "ConvertToBinary": get_flags(
        os.path.join(source_root, "mesh_stream", "ConvertToBinary.cpp")
    ),
    "DerpCLI": get_flags(os.path.join(depth_est_src, "DerpCLI.cpp")),
    "GenerateForegroundMasks": get_flags(
        os.path.join(source_root, "render", "GenerateForegroundMasks.cpp")
    ),
    "LayerDisparities": get_flags(os.path.join(depth_est_src, "LayerDisparities.cpp")),
    "SimpleMeshRenderer": get_flags(
        os.path.join(source_root, "render", "SimpleMeshRenderer.cpp")
    ),
    "UpsampleDisparity": get_flags(
        os.path.join(depth_est_src, "UpsampleDisparity.cpp")
    ),
}


class RepeatedTimer(object):

    """Executes a provided function at periodic intervals.

    Attributes:
        *args: Variable length argument list for the function to be repeatedly executed.
        function (func): Arbitrary function to be repeatedly run.
        interval (int): Number of seconds between consecutive runs of the function.
        is_running (bool): Whether or not the function is currently running.
        **kwargs: Arbitrary keyword arguments for the function to be repeatedly executed.
    """

    def __init__(self, interval, function, *args, **kwargs):
        """Sets up a function to be repeatedly run in the background at fixed intervals.

        Args:
            interval (int):  of seconds between consecutive runs of the function.
            function (func): Arbitrary function to be repeatedly run.
            *args: Variable length argument list.
            **kwargs: Arbitrary keyword arguments.
        """
        self._timer = None
        self.interval = interval
        self.function = function
        self.args = args
        self.kwargs = kwargs
        self.is_running = False
        self.start()

    def _run(self):
        """Runs the function asynchronously."""
        self.is_running = False
        self.start()
        self.function(*self.args, **self.kwargs)

    def start(self):
        """Starts the repeated execution asynchronously."""
        if not self.is_running:
            self._timer = Timer(self.interval, self._run)
            self._timer.start()
            self.is_running = True

    def stop(self):
        """Stops the repeated execution."""
        self._timer.cancel()
        self.is_running = False


def init_facebook360_dep(gflags):
    """Sets up the environment with expected values and handlers.

    Args:
        gflags (absl.flags._flagvalues.FlagValues): Globally defined flags.
    """
    setup_termination_handlers()
    set_glog_env(gflags)
    setup_logging_handler(gflags.log_dir)


# Glog wrapper doesn't see GLOG environment variables, so we need to set them manually
# GLOG environment variables override local flags
def set_glog_env(gflags):
    """Sets up GLOG environment variables.

    Args:
        gflags (absl.flags._flagvalues.FlagValues): Globally defined flags.
    """
    gflags.alsologtostderr = "1"
    gflags.stderrthreshold = "0"
    output_address = Address(FLAGS.output_root)
    if output_address.protocol != "s3":
        gflags.log_dir = os.path.join(FLAGS.output_root, "logs")


# Create logging directory and setup logging handler
def setup_logging_handler(log_dir):
    """Sets up logging.

    Args:
        log_dir (str): Path to directory where logs should be saved.
    """
    if log_dir:
        os.makedirs(log_dir, exist_ok=True)
        program_name = os.path.splitext(os.path.basename(sys.argv[0]))[0]
        logging.get_absl_handler().use_absl_log_file(program_name, log_dir)


def terminate_handler():
    """Cleans workers before terminating the program."""
    cleanup_workers()
    logging.error("".join(traceback.format_stack()))
    sys.exit(0)


def sigterm_handler(signal, frame):
    """Handler for any catchable signal that terminates the program.

    Args:
        signal (signal.signal): Type of signal.
        frame (frame): Stack frame.
    """
    logging.error(f"Signal handler called with signal {signal}")
    terminate_handler()


def setup_termination_handlers(sigterm_handler=sigterm_handler):
    """Sets up a handler for all termination signals.

    Args:
        sigterm_handler (func: (signal.signal, frame) -> void, optional): Function for handling
            termination signals.
    """
    [
        signal.signal(s, sigterm_handler)
        for s in [
            signal.SIGHUP,  # terminate process: terminal line hangup
            signal.SIGINT,  # terminate process: interrupt program
            signal.SIGQUIT,  # create core image: quit program
            signal.SIGILL,  # create core image: illegal instruction
            signal.SIGTRAP,  # create core image: trace trap
            signal.SIGFPE,  # create core image: floating-point exception
            signal.SIGBUS,  # create core image: bus error
            signal.SIGSEGV,  # create core image: segmentation violation
            signal.SIGSYS,  # create core image: non-existent system call invoked
            signal.SIGPIPE,  # terminate process: write on a pipe with no reader
            signal.SIGTERM,  # terminate process: software termination signal
        ]
    ]


def define_flags():
    """Defines abseil flags for render."""
    for bin in bin_to_flags:
        for flag in bin_to_flags[bin]:
            if flag["name"] in flag_names:
                continue
            cmd = f"flags.DEFINE_{flag['type']}('{flag['name']}', {flag['default']}, '{flag['descr']}')"
            exec(cmd)
            flag_names.add(flag["name"])

    flags.DEFINE_integer("chunk_size", 1, "chunk size of work distribution to workers")
    flags.DEFINE_string("cloud", "", "cloud compute service (currently supports: aws)")
    flags.DEFINE_string("color_type", "color", "type of color to render")
    flags.DEFINE_string("disparity_type", "disparity", "type of disparity to render")
    flags.DEFINE_boolean(
        "do_temporal_filter", True, "whether to run temporal filtering"
    )
    flags.DEFINE_boolean(
        "do_temporal_masking",
        False,
        "use foreground masks when doing temporal filtering",
    )
    flags.DEFINE_boolean(
        "force_recompute",
        False,
        "whether to recompute previously performed pipeline stages",
    )
    flags.DEFINE_string("master", config.LOCALHOST, "ip address of master")
    flags.DEFINE_string(
        "password", "", "password for NFS (only relevant for SMB mounts)"
    )
    flags.DEFINE_boolean("run_convert_to_binary", True, "run binary conversion")
    flags.DEFINE_boolean("run_depth_estimation", True, "run depth estimation")
    flags.DEFINE_boolean("run_fusion", True, "run fusion")
    flags.DEFINE_boolean(
        "run_generate_foreground_masks", True, "run foreground mask generation"
    )
    flags.DEFINE_boolean("run_precompute_resizes", True, "run resizing")
    flags.DEFINE_boolean(
        "run_precompute_resizes_foreground", True, "run foreground mask resizing"
    )
    flags.DEFINE_boolean("run_simple_mesh_renderer", True, "run simple mesh renderer")
    flags.DEFINE_boolean("skip_setup", False, "assume workers have already been set up")
    flags.DEFINE_string(
        "username", "", "username for NFS (only relevant for SMB mounts)"
    )
    flags.DEFINE_string("workers", config.LOCALHOST, "ip addresses of workers")

    flag_names.update(
        {
            "chunk_size",
            "cloud",
            "color_type",
            "disparity_type",
            "do_temporal_filter",
            "do_temporal_masking",
            "force_recompute",
            "master",
            "password",
            "run_generate_foreground_masks",
            "run_precompute_resizes",
            "run_precompute_resizes_foreground",
            "run_depth_estimation",
            "run_convert_to_binary",
            "run_fusion",
            "run_simple_mesh_renderer",
            "skip_setup",
            "username",
            "workers",
        }
    )


def log_flags():
    """Prints formatted list of flags and their values."""
    padding = max(len(flag_name) for flag_name in flag_names)
    sorted_flags = sorted(flag_names)
    for flag_name in sorted_flags:
        logging.info(f"{flag_name} = {FLAGS[flag_name].value}".ljust(padding))


def docker_mounts(input_root, host_to_docker_path, username, password):
    """Constructs a list of the relevant commands to mount the external paths.
    The mounts are performed as commands if on a LAN and are volume mounts if
    for a single node.

    Args:
        input_root (str): Path to the root of inputs.
        host_to_docker_path (dict[str, str]): Map of local paths to path inside container.
        username (str): Username for SMB drive. Can be blank if no username is used
            for the drive or if rendering locally.
        password (str): Password for SMB drive. Can be blank if no password is used
            for the drive or if rendering locally.

    Returns:
        list[str]: List of Docker mount commands
    """
    if Address(input_root).protocol == "smb":
        mount_creds = f"mount -t cifs -o username={username},password={password} "
        mounts = [
            f"{mount_creds} //{Address(external_path).ip_path} {docker_path}"
            for external_path, docker_path in host_to_docker_path.items()
        ]
    else:
        mounts = [
            f"--mount type=bind,source={external_path},target={docker_path} \\"
            for external_path, docker_path in host_to_docker_path.items()
        ]
    return mounts


def docker_run_cmd(ip, docker_img=config.DOCKER_IMAGE):
    """Constructs the command to run the Docker container. The container will map all
    the desired endpoints to the canonical structure internally.

    Args:
        ip (str): IP of the master.
        docker_img (str, optional): Name of the docker image.

    Returns:
        str: Command to run the configured Docker container.
    """
    master = config.DOCKER_LOCALHOST if ip == config.LOCALHOST else FLAGS.master
    host_to_docker_path = {
        FLAGS.input_root: config.DOCKER_INPUT_ROOT,
        FLAGS.color: os.path.join(config.DOCKER_INPUT_ROOT, image_type_paths["color"]),
        FLAGS.background_disp: os.path.join(
            config.DOCKER_INPUT_ROOT, image_type_paths["background_disp"]
        ),
        FLAGS.background_color: os.path.join(
            config.DOCKER_INPUT_ROOT, image_type_paths["background_color"]
        ),
        FLAGS.foreground_masks: os.path.join(
            config.DOCKER_INPUT_ROOT, image_type_paths["foreground_masks"]
        ),
        FLAGS.output_root: config.DOCKER_OUTPUT_ROOT,
    }

    mounts = docker_mounts(
        FLAGS.input_root, host_to_docker_path, FLAGS.username, FLAGS.password
    )
    if Address(FLAGS.input_root).protocol == "smb":
        return f"""docker run --privileged \
            -t -d {docker_img}:latest \
            /bin/bash -c "mkdir {config.DOCKER_INPUT_ROOT} && mkdir {config.DOCKER_OUTPUT_ROOT} && {" && ".join(
            mounts)} && python3 {config.DOCKER_SCRIPTS_ROOT}/render/worker.py --master {master}" """

    else:
        mount_cmds = "\n".join(mounts)
        return f"""docker run {mount_cmds} \
            -t -d {docker_img}:latest \
            python3 {config.DOCKER_SCRIPTS_ROOT}/render/worker.py --master {master}"""


def configure_worker_daemon(ip):
    """Configures the Docker daemon to accept HTTP connections for using the local registry.

    Args:
        ip (str): IP of the worker.
    """
    os_type = get_os_type(ip)

    os_paths = {
        OSType.MAC: "~/.docker/",
        OSType.WINDOWS: "$env:userprofile\.docker",
        OSType.LINUX: "/etc/docker/",
    }

    os_restarts = {
        OSType.MAC: [
            """osascript -e 'quit app "Docker"'""",
            "open -a Docker",
            "until docker ps; sleep 2; done",
        ],
        OSType.WINDOWS: [
            "net stop docker",
            "net stop com.docker.service",
            'taskkill /IM "dockerd.exe" /F',
            'taskkill /IM "Docker for Windows.exe" /F',
            "net start docker",
            "net start com.docker.service",
            '& "c:\\Program Files\\Docker\\Docker\\Docker for Windows.exe"',
            "while (!(docker ps)) { sleep 2 };",
        ],
        OSType.LINUX: ["systemctl restart docker"],
    }

    registry = f"{FLAGS.master}:{config.DOCKER_REGISTRY_PORT}"
    daemon_json = os.path.join(os_paths[os_type], config.DOCKER_DAEMON_JSON)

    nc = NetcatClient(ip, config.NETCAT_PORT)
    results = nc.run([f"cat {daemon_json}"])
    try:
        relevant_part = r"\{[^\}]*\}"  # extracts section inside braces
        m = re.search(relevant_part, results)
        daemon_config = json.loads(m.group(0))
    except Exception:
        daemon_config = {}
    if "insecure-registries" in daemon_config:
        if registry in daemon_config["insecure-registries"]:
            return
    else:
        daemon_config["insecure-registries"] = []

    daemon_config["insecure-registries"].append(registry)
    new_daemon_config = json.dumps(daemon_config)
    configure_cmds = [f"echo '{new_daemon_config}' > {daemon_json}"]
    configure_cmds += os_restarts[os_type]
    nc.run(configure_cmds)


def spawn_worker(ip, num_containers, run_async):
    """Creates worker container(s) on the desired IP.

    Args:
        ip (str): IP of the machine to run the worker container.
        num_containers (int): Number of containers to be run.
        run_async (bool): Whether the spawning should happen synchronously or not.
    """
    print(f"Spawning worker on: {ip}...")

    remote_image = f"{FLAGS.master}:{config.DOCKER_REGISTRY_PORT}/{config.DOCKER_IMAGE}"
    configure_worker_daemon(ip)
    cmds = ["docker stop $(docker ps -a -q)", f"docker pull {remote_image}"]
    cmds += [docker_run_cmd(ip, remote_image)] * num_containers

    nc = NetcatClient(ip, config.NETCAT_PORT)

    os_type = get_os_type(ip)
    if os_type == OSType.LINUX:
        nc.run_script("setup_gpu.sh")

    if run_async:
        nc.run_async(cmds)
    else:
        nc.run(cmds)
    print(f"Completed setup of {ip}!")


def spawn_worker_local(replica):
    """Starts a worker locally.

    Args:
        replica (int): Replica ID of the worker being spawned.
    """
    # We use Popen instead of run_command, since worker process is backgrounded
    timestamp = datetime.datetime.now().strftime("%Y%m%d%H%M%S.%f")
    worker_logfile = os.path.join(
        config.DOCKER_INPUT_ROOT, "logs", f"Worker-{timestamp}-{replica}"
    )
    os.makedirs(os.path.dirname(worker_logfile), exist_ok=True)

    with open(worker_logfile, "w") as fp:
        proc = Popen(
            [
                "python3",
                f"{config.DOCKER_SCRIPTS_ROOT}/render/worker.py",
                f"--master={FLAGS.master}",
            ],
            stdout=fp,
            stderr=fp,
        )
        global child_pids
        child_pids.append(proc.pid)


def setup_master(base_params):
    """Sets up the master node for rendering.

    Args:
        base_params (dict[str, _]): Map of all the FLAGS defined in render.py.
    """
    protocol = Address(base_params["input_root"]).protocol
    try:
        if protocol == "s3":
            run_command("sudo service rabbitmq-server start")
        else:
            run_command("service rabbitmq-server start")
    except Exception:
        runtime = "nvidia" if which("nvidia-docker") else ""
        cmd = f"""docker run --runtime={runtime} -p 5672:5672 -p 15672:15672 \
            -d {config.DOCKER_IMAGE}:latest rabbitmq-server start"""
        run_command(cmd)


def setup_workers(base_params):
    """Sets up the worker nodes for rendering.

    Args:
        base_params (dict[str, _]): Map of all the FLAGS defined in render.py.
    """
    processes = []
    for worker in FLAGS.workers.split(","):
        if ":" in worker:
            ip, num_replicas = worker.split(":")
            num_replicas = int(num_replicas)
        else:
            ip = worker
            num_replicas = 1

        if ip == config.LOCALHOST:
            for replica in range(num_replicas):
                spawn_worker_local(replica)
        else:
            processes.append(
                mp.Process(target=spawn_worker, args=(ip, num_replicas, False))
            )

    for process in processes:
        process.start()

    for process in processes:
        process.join()


def cleanup_workers():
    """Destroys the worker process if running locally."""
    for child_pid in child_pids:
        os.kill(child_pid, signal.SIGTERM)
