#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Defines AWSUtil class for miscellaneous interaction functionality with EC2 and S3.

This utility class provides a simple means of interacting with EC2 and S3, similar
to the functionality allowed by the EC2 management dashboard.
"""

import os
import socket
import stat
import subprocess
import sys
import time
from datetime import datetime, timedelta

import boto3

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "render"))
sys.path.append(os.path.join(dir_scripts, "util"))

import scripts.render.glog_check as glog
from scripts.util.system_util import run_command


class AWSUtil:
    """Utility class for interacting with EC2 and S3.

    Attributes:
        aws_access_key_id (str): AWS access key ID
        aws_secret_access_key (str): AWS secret access key
        region_name (str): AWS region name
        session (boto3.Session): Configured session used to front interactions with underlying AWS objects (i.e. EC2/S3).
    """

    def __init__(self, csv_path, s3_url=None, region_name=None):
        """Initializes a session with credentials and determined region and assigns
        instance attributes.

        Args:
            csv_path (str): Path to CSV with credentials (e.g. AWS access key ID and
                secret access key)
            s3_url (str, optional): Path to the S3 bucket to be interacted with. The region
                name is deduced from this path if passed in and the session configured accordingly.
            region_name (str, optional): Region name the session should be configured with.
        """
        self.parse_credentials(csv_path)

        self.region_name = region_name
        if s3_url is not None:
            # session is used temporarily to determine region_name
            self.session = boto3.Session(
                aws_access_key_id=self.aws_access_key_id,
                aws_secret_access_key=self.aws_secret_access_key,
            )
            self.region_name = self.s3_bucket_get_region(s3_url)
        self.session = boto3.Session(
            aws_access_key_id=self.aws_access_key_id,
            aws_secret_access_key=self.aws_secret_access_key,
            region_name=self.region_name,
        )

        iam = self.session.client("iam")
        self.username = iam.get_access_key_last_used(
            AccessKeyId=self.aws_access_key_id
        )["UserName"]

    def parse_credentials(self, csv_path):
        glog.check(csv_path, "Must provide a CSV file with AWS credentials")
        glog.check(os.path.isfile(csv_path), f"File does not exist: {csv_path}")
        with open(csv_path) as f:
            lines = f.readlines()

        err = f"""Invalid AWS credentials CSV format: {lines}
                Expecting fields Access key ID, Secret access key"""
        expected_columns = ["Access key ID", "Secret access key"]
        glog.check_eq(len(lines), len(expected_columns), err)
        csv_names = lines[0].strip().split(",")
        glog.check_eq(len(csv_names), len(expected_columns), err)
        glog.check_eq(csv_names, expected_columns, err)
        csv_vals = lines[1].strip().split(",")
        glog.check_eq(len(csv_vals), len(expected_columns), err)
        self.aws_access_key_id, self.aws_secret_access_key = csv_vals

    def ec2_get_running_instances(self):
        """Gets the EC2 instances that are currently running.

        Returns:
            tuple(str, str): Tuples of instance ID and instance type.
        """
        ec2_instances = self.session.resource("ec2").instances.filter(
            Filters=[{"Name": "instance-state-name", "Values": ["running"]}]
        )
        running_tuples = [
            (instance.id, instance.instance_type) for instance in ec2_instances
        ]
        return running_tuples

    def ec2_get_stats(self, id):
        """Gets CPU utilization stats for a given EC2 instance.

        Args:
            id (str): EC2 instance ID.

        Returns:
            float: Average CPU utilization over the past 30 minutes.
        """
        cw = self.session.client("cloudwatch")
        now = datetime.utcnow()
        past = now - timedelta(minutes=30)
        future = now + timedelta(minutes=10)
        results = cw.get_metric_statistics(
            Namespace="AWS/EC2",
            MetricName="CPUUtilization",
            Dimensions=[{"Name": "InstanceId", "Value": id}],
            StartTime=past,
            EndTime=future,
            Period=300,
            Statistics=["Average"],
        )
        if len(results["Datapoints"]) == 0:
            return None
        return results["Datapoints"][-1]["Average"]

    def ec2_get_kube_worker_instances(self):
        """Gets EC2 instances of workers in the kops cluster.

        Returns:
            ec2.instancesCollection: EC2 instance objects corresponding to kubernetes cluste.
        """
        return self.session.resource("ec2").instances.filter(
            Filters=[
                {
                    "Name": "tag:Name",
                    "Values": [f"nodes.{self.username}.facebook360.dep.k8s.local"],
                }
            ]
        )

    def ec2_get_kube_workers(self):
        """Gets instance IDs of EC2 instances that correspond to workers in the kops cluster.

        Returns:
            dict[str, str]: Map of VPC IPs to public instance IDs.
        """
        client = self.session.client("ec2")
        response = client.describe_instances()

        vpc_ip_to_id = {}
        for reservation in response["Reservations"]:
            for instance in reservation["Instances"]:
                if "Tags" in instance and "PrivateIpAddress" in instance:
                    for tag in instance["Tags"]:
                        if tag["Value"] == f"{self.username}.facebook360.dep.k8s.local":
                            vpc_ip_to_id[instance["PrivateIpAddress"]] = instance[
                                "InstanceId"
                            ]
        return vpc_ip_to_id

    def ec2_get_kube_stats(self):
        """Gets average CPU utilization of workers across the kubernetes cluster.

        Returns:
            float: Average CPU utilization over the past 30 minutes across the cluster.
                Returns None if no workers are present.
        """
        worker_instances = self.ec2_get_kube_worker_instances()
        worker_cpus = []
        for worker in worker_instances:
            worker_cpu_usage = self.ec2_get_stats(worker.id)
            if worker_cpu_usage is not None:
                worker_cpus.append(float(worker_cpu_usage))
        if len(worker_cpus) == 0:
            return None
        return sum(worker_cpus) / len(worker_cpus)

    def ec2_instance_exists(self, id):
        """Checks for the presence of an instance with the specified ID.

        Args:
            id (str): EC2 instance ID.

        Returns:
            bool: Whether or not an instance with the given ID exists.
        """
        ec2_instances = self.session.resource("ec2").instances.all()
        for i in ec2_instances:
            if i.id == id:
                if i.state["Name"] == "terminated":
                    print(f"Note: Instance {id} is terminated and cannot restart")
                    return False
                else:
                    return True
        return False

    def ec2_instance_start(self, id):
        """Starts an EC2 instance with the provided ID.

        Args:
            id (str): EC2 instance ID.
        """
        self.session.client("ec2").start_instances(InstanceIds=[id])
        self.session.resource("ec2").Instance(id).wait_until_running()
        self.wait_for_ip(id)
        print(f"Spawned instance {id}! Waiting to be reachable...")
        ip_address = self.wait_for_ping(id)
        print(f"{ip_address} is reachable!")

    def ec2_instance_stop(self, id):
        """Stops an EC2 instance with the provided ID.

        Args:
            id (str): EC2 instance ID.
        """
        self.session.client("ec2").stop_instances(InstanceIds=[id])

    def ec2_keypair_setup(self, key_fn):
        """Sets up an EC2 keypair and saves it to the specified location. If the
        key already exists locally, the permissions are modified accordingly. If
        the key exists remotely but not locally, the key is deleted and a new one
        created with its name.

        Args:
            key_fn (str): Path to where the key is to be saved.
        """
        if not os.path.exists(key_fn):
            ec2 = self.session.resource("ec2")
            key_name = os.path.splitext(os.path.basename(key_fn))[0]
            if key_name in [k.name for k in ec2.key_pairs.all()]:
                ec2.KeyPair(key_name).delete()
            key_pair = ec2.create_key_pair(KeyName=key_name)
            os.makedirs(os.path.dirname(key_fn), exist_ok=True)
            with open(key_fn, "w") as f:
                f.write(str(key_pair.key_material))
        os.chmod(key_fn, stat.S_IREAD)

    def s3_bucket_is_valid(self, s3_url):
        """Checks whether or not the given URL corresponds to a valid bucket.

        Args:
            s3_url (str): S3 endpoint.

        Returns:
            bool: Whether or not the path corresponds to a properly formed S3 bucket.
        """
        s3 = self.session.resource("s3")
        if "://" not in s3_url:
            return False

        _, s3_path = s3_url.split("://")
        bucket_name, bucket_path = s3_path.split("/", 1)
        bucket = s3.Bucket(bucket_name)
        objs = bucket.objects.filter(Prefix=bucket_path)
        for _ in objs:
            return True
        return False

    def s3_bucket_get_region(self, s3_url):
        """Gets the region associated with a given S3 path.

        Args:
            s3_url (str): S3 endpoint.

        Returns:
            str: Name of the region where the bucket is saved.
        """
        _, s3_path = s3_url.split("://")
        bucket_name, bucket_path = s3_path.split("/", 1)
        s3 = self.session.client("s3")
        return s3.head_bucket(Bucket=bucket_name)["ResponseMetadata"]["HTTPHeaders"][
            "x-amz-bucket-region"
        ]

    def s3_ls(self, s3_url, run_silently=False):
        """Lists the contents of the given S3 path

        Args:
            s3_url (str): S3 URL
        """
        if not s3_url.endswith("/"):
            s3_url = f"{s3_url}/"
        try:
            raw_output = run_command(f"aws s3 ls {s3_url}", run_silently)
        except subprocess.CalledProcessError:
            if not run_silently:
                print(f"Failed to list: {s3_url}!")
            return []

        raw_lines = raw_output.split("\n")
        return [raw_line.split(" ")[-1].strip().rstrip("/") for raw_line in raw_lines]

    def s3_cp(
        self, src, dst, exclude=None, include=None, recursive=True, run_silently=False
    ):
        """Copies files in src to dst according to given exclude and include filters

        Args:
            src (str): source path
            dst (str): destination path
            exclude (str): exclude filter
            include (str): include filter
        """
        try:
            filters = ""
            if exclude:
                filters += f" --exclude '{exclude}'"
            if include:
                filters += f" --include '{include}'"
            if recursive:
                filters += f" --recursive"
            run_command(f"aws s3 cp {src} {dst} {filters}", run_silently)
        except subprocess.CalledProcessError:
            raise Exception(f"Failed to cp {src} to {dst}!")

    def s3_sync(self, src, dst, exclude=None, include=None, run_silently=False):
        """Syncs files in src to dst according to given exclude and include filters

        Args:
            src (str): source path
            dst (str): destination path
            exclude (str): exclude filter
            include (list[str]): include filters
        """
        try:
            filters = ""
            if exclude:
                filters += f" --exclude '{exclude}'"
            if include:
                for incl in include:
                    filters += f" --include '{incl}'"
            run_command(f"aws s3 sync {src} {dst} {filters}", run_silently)
        except subprocess.CalledProcessError:
            raise Exception(f"Failed to sync {src} to {dst}!")

    def setup_aws_region(self, region_name):
        """Sets up AWS region and updates session instance variable.

        Args:
            region_name (str): Region name.
        """
        self.region_name = region_name
        self.session = boto3.Session(
            aws_access_key_id=self.aws_access_key_id,
            aws_secret_access_key=self.aws_secret_access_key,
            region_name=self.region_name,
        )

    def configure_shell(self, run_silently=False):
        """Configures local shell with the AWS credentials and region."""
        cmd = "aws configure set"
        run_command(
            f"{cmd} aws_access_key_id {self.aws_access_key_id}",
            run_silently=run_silently,
        )
        run_command(
            f"{cmd} aws_secret_access_key {self.aws_secret_access_key}",
            run_silently=run_silently,
        )
        if self.region_name:
            run_command(
                f"{cmd} default.region {self.region_name}", run_silently=run_silently
            )

    def wait_for_ip(self, instance_id):
        """Halts execution until an IP is obtained for the specified EC2 instance.

        Args:
            instance_id (str): EC2 instance ID.

        Returns:
            str: IP address of the EC2 instance.
        """
        instance = self.session.resource("ec2").Instance(id=instance_id)
        while not instance.public_ip_address:
            time.sleep(5)
            instance.reload()
        return instance.public_ip_address

    def wait_for_ping(self, instance_id, retries=10):
        """Halts execution until the specified EC2 instance can be pinged.

        Args:
            instance_id (str): EC2 instance ID.

        Returns:
            str: IP address of the EC2 instance.
        """
        instance = self.session.resource("ec2").Instance(id=instance_id)
        ip_address = instance.public_ip_address
        retry = 1
        while retry <= retries:
            print(f"Trying to reach {ip_address}. Retry {retry}/{retries}...")
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            if sock.connect_ex((ip_address, 22)) == 0:
                break
            else:
                time.sleep(10)
                retry = retry + 1
        return ip_address

    def get_instance_state(self, instance_id):
        """Returns the state of the specified EC2 instance.

        Args:
            instance_id (str): EC2 instance ID.

        Returns:
            str: Instance state.
        """
        instance = self.session.resource("ec2").Instance(id=instance_id)
        return instance.state["Name"]
