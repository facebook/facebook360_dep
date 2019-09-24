#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Message queue based distributed render module.

This module serves as the main entrypoint for the rendering pipeline. The system uses a simple
message queue architecture for supported arbitrary host rendering. Functionality is best couple
when run across Docker containers, although any system that can run the binaries are supported.

The current rendering pipeline supports execution in:
    - Single node (i.e. master/worker are the same machine)
    - LAN farms (i.e. master directly communicates to worker nodes in manually configured farm)
    - AWS farms (i.e. master sets up workers via kubernetes)

To render, the render.py script serves as the main entrypoint to the back-end and
run.py to the front-end.
"""
