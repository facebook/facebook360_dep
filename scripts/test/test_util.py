#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Utility functions across the unit tests.

Running the script independently runs min_max_frame_from_data_dir on the specified directory.

Example:
    To test min_max_frame_from_data_dir on [directory_name], run the script via:

        $ python test_util.py [directory_name]
"""

import argparse
import os


def min_max_frame_from_data_dir(data_dir):
    """Finds the frame names of min and max integer value from the specified directory.

    Args:
        data_dir (str): Directory in which frames are saved on disk.

    Returns:
        tuple[str]: Two element pair (first, last) of min/max frames present in
            the specified directory.
    """
    cams = [f for f in os.listdir(data_dir) if not f.startswith(".")]
    frames = [
        f for f in os.listdir(os.path.join(data_dir, cams[0])) if not f.startswith(".")
    ]
    frames.sort()
    frame_first = os.path.splitext(frames[0])[0]
    frame_last = os.path.splitext(frames[-1])[0]
    return (frame_first, frame_last)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("data_dir", type=str)
    args = parser.parse_args()

    print(min_max_frame_from_data_dir(args.data_dir))
