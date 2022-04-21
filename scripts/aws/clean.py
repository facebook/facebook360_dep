#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Terminates any kops cluster present with the name facebook360.dep.k8s.local.

Takes paths that corresponds to credentials for the user's AWS account
and terminates the kops cluster. If executed when no kops cluster is
present, the script will terminate without any effect.

Example:
    To run cleanup manually, simply execute:

        $ python clean.py \
          --csv_path=/path/to/credentials.csv \
          --key_dir=/path/to/keys/ \
          --key_name=key \
          --ec2_file=/path/to/aws/ec2.txt

    Note that the ec2_file flag points to a file that has the instance ID corresponding
    to the instance used to stage the kops cluster. This file is generated automatically
    after running through the create.py but can be created manually if so desired.

Attributes:
    FLAGS (absl.flags._flagvalues.FlagValues): Globally defined flags for clean.py.
"""

import os
import sys

from absl import app, flags
from fabric import Connection

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "render"))

import scripts.render.config as config
from create import run_ssh_command
from util import AWSUtil

FLAGS = flags.FLAGS


def main(argv):
    """Terminates any kubernetes cluster associated with the account that is running
    with the name <username>.facebook360.dep.k8s.local.

    Args:
        argv (list[str]): List of arguments (used interally by abseil).
    """
    aws_util = AWSUtil(FLAGS.csv_path)
    ec2_file = os.path.expanduser(FLAGS.ec2_file)
    with open(ec2_file) as f:
        instance_id = f.readline().strip()
        ip_staging = aws_util.wait_for_ping(instance_id)

    key_dir = os.path.expanduser(FLAGS.key_dir)
    key_fn = os.path.join(key_dir, f"{FLAGS.key_name}.pem")

    # Last command is responsible for clearing old Docker images
    cmds = [
        "echo 'Deleting old cluster. This can take a few minutes...'",
        f"""kops delete cluster \
            --state=s3://{aws_util.username}-facebook360-dep-kops-state-store \
            --name={aws_util.username}.facebook360.dep.k8s.local --yes""",
        """aws ecr describe-repositories --output text | \
        awk '{print $5}' | \
        while read line; do  \
            aws ecr list-images --repository-name $line --filter tagStatus=UNTAGGED --query 'imageIds[*]' --output text | \
            while read imageId; do \
                aws ecr batch-delete-image --repository-name $line --image-ids imageDigest=$imageId;
            done;
        done""",
    ]

    with open(config.DOCKER_AWS_WORKERS, "w") as f:
        pass  # clears the file

    for cmd in cmds:
        run_ssh_command(key_fn, ip_staging, cmd)
    aws_util.ec2_instance_stop(instance_id)


if __name__ == "__main__":
    # Abseil entry point app.run() expects all flags to be already defined
    flags.DEFINE_string("csv_path", None, "path to AWS credentials CSV")
    flags.DEFINE_string("ec2_file", "~/ec2_info.txt", "file to read EC2 info from")
    flags.DEFINE_string(
        "key_dir", "~/aws_keys", "directory where AWS .pem files are stored"
    )
    flags.DEFINE_string("key_name", "ec2-keypair", "name of the .pem keypair")
    flags.DEFINE_string("region", "us-west-2", "region cluster exists")

    # Required FLAGS.
    flags.mark_flag_as_required("csv_path")
    app.run(main)
