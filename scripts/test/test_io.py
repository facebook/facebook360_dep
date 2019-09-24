#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Miscellaneous download utilities used by the test suite.

The utility exposes an endpoint map, which provides functionality for downloading datasets from
the corresponding endpoint. This functionality can be extended to arbitrary endpoints if similar
download functions are added for additional endpoints. The current functionality only covers S3.

Example:
    To extend the Loader to support additional endpoints for datasets, either subclass this
    or define a new download function and add it to the endpoint map:

        1 class ExampleLoader(Loader):
        2    def __init__(self):
        3        super().__init__()
        4        self.endpoint_map["example"] = self._download_example
        5
        6    def _download_gaia(self, dataset, tag="vr_camera"):
        7        # Download dataset from example:// websites
        8        ...
        20       return local_path

"""

import os
import sys

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "util"))

from scripts.util.system_util import extract_tar


class Loader:

    """Loader for downloading and preparing datasets available at public endpoints.

    Attributes:
        endpoint_map (dict[str, func : str -> str): Map of endpoint type to a function
            that downloads URLs of said type. Functions should return the local path.
    """

    def __init__(self):
        self.endpoint_map = {"s3": self._download_s3}

    def download(self, dataset_root, dataset, local):
        """Downloads the desired dataset to the local path specified. The dataset must
        be located in dataset_root. The dataset is assumed to be tar compressed.

        Args:
            dataset_root (str): Root directory in which datasets are all saved (e.g. s3://test/).
            dataset (str): Name of the dataset file (e.g. example.tar).
            local (str): Directory where tars are to be saved and extracted.
        """
        protocol, dataset_path = dataset_root.split("://", 1)
        dataset_tar = f"{dataset}.tar"
        self.endpoint_map[protocol](os.path.join(dataset_path, dataset_tar))
        extract_tar(dataset_tar, dst=local)

    def _download_s3(self, url):
        """Downloads from the specified S3 URL to the current directory.

        Args:
            url (str): S3 endpoint without s3:// prefix (e.g. bucket/testing)

        Returns:
            str: Local path to where the object pointed to by the URL was downloaded.
        """
        if os.path.exists(os.path.basename(url)):
            return None

        import wget

        bucket_name, remote_path = url.split("/", 1)
        public_s3_url = f"http://{bucket_name}.s3.amazonaws.com/{remote_path}"
        local_path = wget.download(public_s3_url)
        return local_path
