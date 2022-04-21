#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Create a kops cluster with the name <username>.facebook360.dep.k8s.local.

Takes paths that corresponds to credentials for the user's AWS account
and parameters desired for the kops cluster (e.g. instance type and number of workers)
and creates a corresponding kops cluster. If executed when a kops cluster is
already present, the script will terminate without any effect.

Example:
    To run create manually, simply execute:

        $ python create.py \
          --csv_path=/path/to/credentials.csv \
          --key_dir=/path/to/keys/ \
          --key_name=key \
          --ec2_file=/path/to/aws/ec2.txt \
          --cluster_size=2

    This will create a cluster with two worker nodes (with the default instance type
    of c4.xlarge) and a master node (with type c4.large).

Attributes:
    FLAGS (absl.flags._flagvalues.FlagValues): Globally defined flags for create.py.
"""

import os
import stat
import sys
import time
from base64 import b64decode
from pathlib import Path
from shutil import copyfile

import botocore
import docker
import patchwork.transfers
from absl import app, flags
from fabric import Connection

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "util"))

import scripts.render.config as config
from scripts.util.system_util import run_command
from util import AWSUtil

FLAGS = flags.FLAGS


def sync_files(key_fn, ip_staging):
    """Syncs all the local files aside from the input, output, and build roots to
    the root of the remote machine.

    Args:
        key_fn (str): Path to the .pem file for the staging instance.
        ip_staging (str): IP address of the staging instance.
    """
    # rsync through patchwork has issues with non-escaped key paths, but fabric wants non-escaped
    # Solution: create fabric connection without key and pass it escaped in rsync options
    with Connection(host=ip_staging, user="ubuntu") as c:
        ssh_opts = f'-i "{key_fn}"'
        patchwork.transfers.rsync(
            c,
            config.DOCKER_ROOT + "/",
            "/home/ubuntu/",
            ssh_opts=ssh_opts,
            exclude=(
                config.INPUT_ROOT_NAME,
                config.OUTPUT_ROOT_NAME,
                config.BUILD_ROOT_NAME,
            ),
            strict_host_keys=False,
        )


def run_ssh_command(key_fn, ip_staging, cmd, hide=False, ignore_env=False):
    """Executes a command over SSH on the remote machine.

    Args:
        key_fn (str): Path to the .pem file for the staging instance.
        ip_staging (str): IP address of the staging instance.
        cmd (str): Command to be executed on the instance.
        hide (bool, optional): Whether or not to show stdout.
        ignore_env (bool, optional): Whether or not to set up environment.

    Returns:
        str: Output from stdout of executing the command.
    """
    with Connection(
        host=ip_staging,
        user="ubuntu",
        connect_kwargs={"key_filename": [key_fn]},
        inline_ssh_env=True,
    ) as c:
        if not ignore_env:
            c.config.run.env = {"PATH": "/home/ubuntu/.local/bin:$PATH"}
        result = c.run(cmd, hide=hide)
    return result.stdout.strip()


def run_detached_ssh_command(key_fn, ip_staging, cmd, output_fn=None):
    redirection = f">> {output_fn}" if output_fn is not None else ""
    detached_cmd = (
        f"(nohup bash -c '{cmd} {redirection}' >& {output_fn} < /dev/null &) && sleep 1"
    )
    print(detached_cmd)
    run_ssh_command(key_fn, ip_staging, detached_cmd)


def configure_remote_shell(aws_util, key_fn, ip_staging):
    """Given a key and IP address of a machine and an AWSUtil configured
    to it, configures the shell with the access key ID, secret access key, and
    region if present in the AWSUtil instance.

    Args:
        aws_util (AWSUtil): AWSUtil configured with credentials for the staging instance.
        key_fn (str): Path to the .pem file for the staging instance.
        ip_staging (str): IP address of the staging instance.
    """
    run_ssh_command(
        key_fn,
        ip_staging,
        f"aws configure set aws_access_key_id {aws_util.aws_access_key_id}",
    )
    run_ssh_command(
        key_fn,
        ip_staging,
        f"aws configure set aws_secret_access_key {aws_util.aws_secret_access_key}",
    )
    if aws_util.region_name:
        run_ssh_command(
            key_fn,
            ip_staging,
            f"aws configure set default.region {aws_util.region_name}",
        )


def get_staging_info(aws_util, ec2_file, start_instance=True):
    instance_id = None
    ip_staging = None
    if os.path.exists(ec2_file):
        with open(ec2_file) as f:
            instance_id = f.readline().strip()
            if aws_util.ec2_instance_exists(instance_id):
                state = aws_util.get_instance_state(instance_id)
                if state != "terminated":
                    if state == "running":
                        ip_staging = aws_util.wait_for_ping(instance_id)
                    elif start_instance:
                        print(f"Starting instance {instance_id}...")
                        aws_util.ec2_instance_start(instance_id)
                        ip_staging = aws_util.wait_for_ping(instance_id)
    return instance_id, ip_staging


def create_instance(aws_util, key_fn, ec2_file=None):
    """Creates and sets up an instance for rendering. If an instance was previously
    created, that instance is started and set up is not re-run.

    Args:
        aws_util (AWSUtil): AWSUtil configured with credentials for the staging instance.
        key_fn (str): Path to the .pem file for the staging instance.

    Returns:
        tuple (str, str): Tuple with the instance ID name and corresponding IP.
    """
    if ec2_file is None:
        ec2_file = os.path.expanduser(FLAGS.ec2_file)
    if os.path.exists(ec2_file):
        instance_id, ip_staging = get_staging_info(aws_util, ec2_file)
        if instance_id and ip_staging:
            return instance_id, ip_staging

    print("Creating instance...")
    ec2 = aws_util.session.resource("ec2")

    # We open permissions for ingress and egress to ensure no communication issues w/ k8s
    try:
        security_group = ec2.create_security_group(
            GroupName=FLAGS.security_group, Description="Facebook360_dep security group"
        )
        security_group.authorize_ingress(
            IpProtocol="tcp", CidrIp="0.0.0.0/0", FromPort=0, ToPort=65535
        )
    except botocore.exceptions.ClientError:
        pass

    instances = ec2.create_instances(
        BlockDeviceMappings=[{"DeviceName": "/dev/sda1", "Ebs": {"VolumeSize": 128}}],
        ImageId=FLAGS.ami,
        InstanceType=FLAGS.instance_type_staging,
        MinCount=1,
        MaxCount=1,
        KeyName=FLAGS.key_name,
        SecurityGroups=[FLAGS.security_group],
        TagSpecifications=[
            {
                "ResourceType": "instance",
                "Tags": [{"Key": "Name", "Value": f"{aws_util.username}.{FLAGS.name}"}],
            }
        ],
    )

    print("Waiting for initialization...")
    instance = instances[0]
    instance.wait_until_running()
    aws_util.wait_for_ip(instance.id)
    print(f"Spawned instance {instance.id}! Waiting to be reachable...")

    ip_staging = aws_util.wait_for_ping(instance.id)

    print(f"Running setup on instance ({ip_staging})...")
    sync_files(key_fn, ip_staging)
    run_ssh_command(key_fn, ip_staging, "~/scripts/aws/setup.sh")

    return instance.id, ip_staging


def setup_instance(
    aws_util, key_fn, ip_staging, master_ip, repo_uri, wait_to_init=True
):
    """Creates a kops cluster and deploys kubernetes cluster to it. If a kops cluster already
    exists, this step is skipped over and the kubernetes cluster is deployed to the
    existing cluster. If the kubernetes cluster already exists, the cluster is updated.

    Args:
        aws_util (AWSUtil): AWSUtil configured with credentials for the staging instance.
        key_fn (str): Path to the .pem file for the staging instance.
        ip_staging (str): IP address of the staging instance.
        master_ip (str): IP address of the kubernetes master node instance.
        repo_uri (str): URI for remote Docker registry.
        wait_to_init (bool): Whether or not the function should wait until at least one
            container is running to complete.
    """
    # Send source code to EC2 instance
    sync_files(key_fn, ip_staging)
    if FLAGS.cluster_size > 0:
        config_fn = os.path.join(config.DOCKER_AWS_ROOT, "config.txt")
        cluster_config = f"""--cluster_size={str(FLAGS.cluster_size)}
        --instance_type={FLAGS.instance_type}
        """
        cached_config = None
        if os.path.exists(config_fn):
            with open(config_fn) as f:
                cached_config = "".join(f.readlines())
        delete_cluster = cached_config != cluster_config
        if delete_cluster:
            with open(config_fn, "w") as f:
                f.write(cluster_config)

        run_ssh_command(
            key_fn,
            ip_staging,
            f"""~/scripts/aws/create_cluster.sh \
                {aws_util.aws_access_key_id} \
                {aws_util.aws_secret_access_key} \
                {aws_util.region_name} \
                {str(FLAGS.cluster_size)} \
                {FLAGS.instance_type} \
                {aws_util.username} \
                {str(delete_cluster).lower()}""",
        )
        push_docker_to_aws(repo_uri, aws_util)
        run_ssh_command(
            key_fn,
            ip_staging,
            f"~/scripts/aws/deploy.sh {repo_uri} {master_ip} {str(FLAGS.cluster_size)}",
        )

        remote_logs_dir = "~/logs"
        try:
            run_ssh_command(key_fn, ip_staging, f"mkdir {remote_logs_dir}")
        except Exception:
            pass  # occurs if the directory already exists

        while wait_to_init:
            kubectl_pods = run_ssh_command(
                key_fn, ip_staging, "kubectl get pods", hide=True
            ).split("\n")[1:]

            # kubectl lines are of the format:
            # facebook360-dep-588bdc5ff5-7d572   1/1     Running   0          29m
            for kubectl_pod in kubectl_pods:
                kubectl_pod_status = kubectl_pod.split()[2].strip()
                if kubectl_pod_status == "Running":
                    wait_to_init = False

            if wait_to_init:
                print("Waiting for Kubernetes pods to initialize...")
                time.sleep(10)

        vpc_ips_to_id = aws_util.ec2_get_kube_workers()
        get_pods_result = run_ssh_command(
            key_fn, ip_staging, "kubectl get pods -o wide", hide=True
        )
        pod_properties = get_pods_result.split("\n")[1:]
        pod_names = []

        for pod_property in pod_properties:
            # kops pod names are of the form ip-172-20-40-140.us-west-2.compute.internal
            # where the ip-a-b-c-d correspond to the private IP a.b.c.d
            if "ip-" not in pod_property:
                continue
            pod_property_attrs = pod_property.split()
            node_name = pod_property_attrs[0]
            pod_name = [attr for attr in pod_property_attrs if "ip-" in attr][0]
            pod_names.append(pod_name)
            hyphenated_ip = pod_name.split(".")[0].split("-", 1)[1]
            private_ip = hyphenated_ip.replace("-", ".")
            instance_id = vpc_ips_to_id[private_ip]
            run_ssh_command(
                key_fn,
                ip_staging,
                f"nohup kubectl logs -f {node_name} >> {remote_logs_dir}/Worker-{instance_id}.txt &",
                hide=True,
                ignore_env=True,
            )
        with open(config.DOCKER_AWS_WORKERS, "w") as f:
            f.write("\n".join(pod_names))


def push_docker_to_aws(repo_uri, aws_util):
    """Pushes Docker image to the specified URI.

    Args:
        repo_uri (str): URI for remote Docker registry.
        aws_util (AWSUtil): AWSUtil configured with credentials for the staging instance.
    """
    local_img = f"localhost:5000/{config.DOCKER_IMAGE}"
    remote_img = f"{repo_uri}:{config.DOCKER_IMAGE}"

    ecr = aws_util.session.client("ecr")
    auth = ecr.get_authorization_token()
    token = auth["authorizationData"][0]["authorizationToken"]
    username, password = b64decode(token).split(b":")
    auth_config_payload = {
        "username": username.decode("utf8"),
        "password": password.decode("utf8"),
    }

    local_img = "localhost:5000/worker"
    remote_img = f"{repo_uri}:worker"

    client = docker.APIClient()
    client.tag(local_img, remote_img)
    for line in client.push(
        remote_img, stream=True, auth_config=auth_config_payload, decode=True
    ):
        if "status" in line:
            if "progress" in line:
                print(f"{line['status']}: {line['progress']}")
            else:
                print(line["status"])


def get_render_info(key_fn, ip_staging):
    render_jobs_raw = run_ssh_command(key_fn, ip_staging, "ps aux | grep render.py")
    return render_jobs_raw.split("\n")


def has_render_flag(key_fn, ip_staging, flag, value):
    render_jobs = get_render_info(key_fn, ip_staging)

    CMD_IDX = 10

    for render_job in render_jobs:
        # ps aux lines are of the format: ubuntu PID ... cmd
        render_job_info = render_job.split()
        cmd = " ".join(render_job_info[CMD_IDX:])
        if cmd.startswith("python3"):
            return any(f"{flag}={value}" in info for info in render_job_info)
    return None


def get_render_pid(key_fn, ip_staging):
    render_jobs = get_render_info(key_fn, ip_staging)

    PID_IDX = 1
    CMD_IDX = 10

    for render_job in render_jobs:
        # ps aux lines are of the format: ubuntu PID ... cmd
        render_job_info = render_job.split()
        cmd = " ".join(render_job_info[CMD_IDX:])
        print(f"COMMAND: {cmd}")
        if cmd.startswith("python3"):
            pid = render_job_info[PID_IDX]
            return pid
    return None


def run_render(key_fn, ip_staging, master_ip):
    """Runs render on the deployed kubernetes cluster.

    Args:
        key_fn (str): Path to the .pem file for the staging instance.
        ip_staging (str): IP address of the staging instance.
        master_ip (str): IP address of the kubernetes master node instance.
    """
    render_path = "~/scripts/render/render.py"
    render_flags = os.path.join(
        str(Path(os.path.abspath(__file__)).parents[2]),
        "project",
        "flags",
        f"render_{FLAGS.tag}.flags",
    )

    with open(render_flags) as f:
        render_flags = f.readlines()
    render_flags_combined = " ".join(render_flags).replace("\n", "")
    try:
        run_ssh_command(key_fn, ip_staging, "rm render.out")
    except Exception:
        pass  # There may be no previous runs on this staging machine or it was manually cleaned
    run_detached_ssh_command(
        key_fn,
        ip_staging,
        f"python3 -u {render_path} {render_flags_combined} --master={master_ip} --cloud=aws",
        output_fn="render.out",
    )


def create_key(aws_util):
    key_dir = os.path.expanduser(FLAGS.key_dir)
    key_file = f"{FLAGS.key_name}.pem"
    key_fn_mount = os.path.join(key_dir, key_file)
    aws_util.ec2_keypair_setup(key_fn_mount)

    # Copy key to local path. Windows doesn't allow us to change permissions in bound paths
    key_fn = f"/{key_file}"
    copyfile(key_fn_mount, key_fn)
    os.chmod(key_fn, stat.S_IREAD)
    return key_fn


def get_repo_uri(key_fn, ip_staging, ecr_registry_name):
    return run_ssh_command(
        key_fn,
        ip_staging,
        f"""aws ecr describe-repositories \
        --repository-names {ecr_registry_name} | jq -r '.repositories[0].repositoryUri'""",
        hide=True,
    )


def main(argv):
    """Creates a kops cluster, deploys a kubernetes cluster atop it, and runs render
    with the kubernetes nodes as workers. The cluster remain upon completion and must
    be externally terminated (re: clean.py).

    Args:
        argv (list[str]): List of arguments (used interally by abseil).
    """
    aws_util = AWSUtil(FLAGS.csv_path, region_name=FLAGS.region)
    key_fn = create_key(aws_util)

    instance_id, ip_staging = create_instance(aws_util, key_fn)
    ec2_file = os.path.expanduser(FLAGS.ec2_file)
    with open(ec2_file, "w") as f:
        f.write(instance_id)

    configure_remote_shell(aws_util, key_fn, ip_staging)

    master_ip = run_ssh_command(
        key_fn,
        ip_staging,
        """aws ec2 describe-instances \
                    --instance-ids $(ec2metadata --instance-id) \
                    --query 'Reservations[*].Instances[*].PublicIpAddress' \
                    --output text""",
        hide=True,
    )

    ecr_registry_name = f"fb360-{aws_util.username}"
    repo_uri = get_repo_uri(key_fn, ip_staging, ecr_registry_name)
    if not repo_uri.strip():
        run_ssh_command(
            key_fn,
            ip_staging,
            f"aws ecr create-repository --repository-name {ecr_registry_name}",
        )
        repo_uri = get_repo_uri(key_fn, ip_staging, ecr_registry_name)

    render_pid = get_render_pid(key_fn, ip_staging)
    if render_pid is None:
        setup_instance(aws_util, key_fn, ip_staging, master_ip, repo_uri)
        run_render(key_fn, ip_staging, master_ip)
        render_pid = get_render_pid(key_fn, ip_staging)

    sync_logs = f"""while true; do \
        rsync -avz -e 'ssh -i {key_fn}' \
        ubuntu@{ip_staging}:/home/ubuntu/logs/ {config.DOCKER_INPUT_ROOT}/logs/; \
        sleep 10; \
    done &"""
    run_command(sync_logs, run_async=True)

    display_render_progress = f"""tail --pid {render_pid} -n +1 -f render.out"""
    run_ssh_command(key_fn, ip_staging, display_render_progress)


if __name__ == "__main__":
    # Abseil entry point app.run() expects all flags to be already defined
    flags.DEFINE_string(
        "ami",
        "ami-005bdb005fb00e791",
        "ID of the AMI to use (defaults to Ubuntu 18.04)",
    )
    flags.DEFINE_integer(
        "cluster_size", 0, "size of Kubernetes cluster (0 = no cluster)"
    )
    flags.DEFINE_string("csv_path", None, "path to AWS credentials CSV")
    flags.DEFINE_string("ec2_file", "~/ec2_info.txt", "file to save EC2 info to")
    flags.DEFINE_string(
        "instance_type", "c4.xlarge", "AWS instance type for worker nodes"
    )
    flags.DEFINE_string(
        "instance_type_staging",
        "c4.xlarge",
        "AWS instance type for the staging machine",
    )
    flags.DEFINE_string(
        "key_dir", "~/aws_keys", "directory where AWS .pem files are stored"
    )
    flags.DEFINE_string("key_name", "ec2-keypair", "name of the .pem keypair")
    flags.DEFINE_string(
        "name", "facebook360-dep", "name of the instance to be loaded/created"
    )
    flags.DEFINE_string("region", "us-west-2", "region where instance will spawn")
    flags.DEFINE_string(
        "security_group", "facebook360-dep", "name of the security group"
    )
    flags.DEFINE_string(
        "tag", "", "tag of the type of render (either 'depth' or 'export')"
    )

    # Required FLAGS.
    flags.mark_flag_as_required("csv_path")
    app.run(main)
