#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Project class that allows S3 data verification from the host.

Example:
    project = Project(
        FLAGS.project_root,
        FLAGS.cache,
        FLAGS.csv_path,
        FLAGS.s3_sample_frame,
        FLAGS.s3_ignore_fullsize_color,
        FLAGS.verbose,
    )
    project.verify()
"""

import os
import pickle
import sys

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "render"))

import verify_data
from scripts.aws.util import AWSUtil
from scripts.render.network import Address


class Project:

    """Project class to interact with S3"""

    def __init__(
        self,
        project_root,
        cache,
        csv_path,
        s3_sample_frame,
        s3_ignore_fullsize_color,
        verbose,
    ):
        """Project class to interact with S3

        Args:
            project_root (str): Path of the project.
            cache (str): Path of the local cache.
            csv_path (str): Path to AWS credentials .csv file
            s3_sample_frame (str): Sample frame to donwload (empty = first found)
            s3_ignore_fullsize_color (bool): Boolean to ignore full-size colors
            verbose (bool): Verbose output

        """
        project_address = Address(project_root)
        self.is_aws = project_address.protocol == "s3"
        self.is_lan = project_address.protocol == "smb"
        self.project_root = project_root
        self.path_project = os.path.join(cache, project_address.path)
        self.s3_sample_frame = s3_sample_frame
        self.s3_ignore_fullsize_color = s3_ignore_fullsize_color
        self.verbose = verbose
        if self.is_aws:
            self.aws_util = AWSUtil(csv_path, s3_url=self.project_root)
            self.aws_util.configure_shell(run_silently=not verbose)

        verify_data.set_default_top_level_paths(self)

    def verify(self, pickle_frames=True):
        """Verify S3 data, including downloading and unpacking sample frames"""
        verify_data.verify(self)
        if pickle_frames:
            dict_all = self.__dict__
            dict_project = {k: v for k, v in dict_all.items() if k != "aws_util"}
            pickle_fn = os.path.join(self.path_project, "project.pickle")
            with open(pickle_fn, "wb") as f:
                pickle.dump(dict_project, f, pickle.HIGHEST_PROTOCOL)
