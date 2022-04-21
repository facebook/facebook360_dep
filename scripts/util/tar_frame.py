#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Archives a frame as tar

Example:
    $ python3 tar_frame.py --src=/path/to/video/disparity --frame=000000

Attributes:
    FLAGS (absl.flags._flagvalues.FlagValues): Globally defined flags for tar_frame.py.
"""

import os
import tarfile

from absl import app, flags

FLAGS = flags.FLAGS


def main(argv):
    """Tars a frame.

    Args:
        argv (list[str]): List of arguments (used interally by abseil).
    """
    frame_tar_fn = os.path.join(FLAGS.src, f"{FLAGS.frame}.tar")
    tar_file = tarfile.open(frame_tar_fn, "w")
    print(f"Creating {frame_tar_fn}...")
    for root, _, files in os.walk(FLAGS.src):
        for file in files:
            if FLAGS.frame in file:
                arcname = os.path.join(os.path.basename(root), file)
                tar_file.add(os.path.join(root, file), arcname=arcname)
    tar_file.close()


if __name__ == "__main__":
    flags.DEFINE_string("src", None, "Path to the directory to be packed")
    flags.DEFINE_string("frame", None, "Name of the frame (6 digit, zero padded)")

    # Required FLAGS.
    flags.mark_flag_as_required("src")
    flags.mark_flag_as_required("frame")
    app.run(main)
