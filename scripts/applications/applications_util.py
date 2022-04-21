#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Utility class for running system commands of C++ binaries"""

import os
import sys
import time

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
dir_res = os.path.join(dir_root, "res")
binary_dir = os.path.join(dir_root, "build/bin")
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "util"))

from scripts.util.system_util import gen_args_from_flags, run_command


class AppUtil:
    def __init__(self, binary_name, flags):
        self.binary_name = binary_name
        self.flags = flags

    def run_app(self):
        """Runs desired binary and returns the time elapsed during execution."""
        cmd = self.gen_command()
        start_time = time.time()
        run_command(cmd)
        end_time = time.time()
        return end_time - start_time

    def gen_command(self):
        """Constructs the command to run the binary.

        Returns:
            str: Command to run.
        """
        check_binary_availability(self.binary_name)

        args_string = gen_args_from_flags(self.flags)

        return f"{os.path.join(binary_dir, self.binary_name)} {args_string}"


def check_binary_availability(binary_name):
    """Checks whether or not the binary exists. Exits with error code 1 if
    the binary does not exist.
    """
    binary_path = os.path.join(binary_dir, binary_name)
    if not os.path.isfile(binary_path):
        print(f"No application {binary_name} found at {binary_path}")
        exit(1)
