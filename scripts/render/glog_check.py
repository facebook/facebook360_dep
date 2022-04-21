#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Implementation of glog's CHECK functions.

Provides a clean syntax that parallels the interface of CHECKs in C++. This
file cannot be executed standalone and is intended to be used as a utility for other scripts.

Example:
    A standard use of the glog_check functionality is for verifying FLAGS validity:

        1   import glog_check as glog
        2   FLAGS = flags.FLAGS
        3   ...
        6   glog.check_ne(FLAGS.non_empty_flag, "", "non_empty_flag must be non-empty")
"""

import sys


class bcolors:
    END = "\033[0m"
    GREEN = "\033[1;32m"
    RED = "\033[1;31m"
    YELLOW = "\033[1;33m"


def green(msg):
    return f"{bcolors.GREEN}{msg}{bcolors.END}"


def yellow(msg):
    return f"{bcolors.YELLOW}{msg}{bcolors.END}"


def red(msg):
    return f"{bcolors.RED}{msg}{bcolors.END}"


def check(condition, message=None):
    """Produces a message if the condition does not hold.

    Args:
        condition (bool): The condition to be verified.
        message (str, optional): Text to be displayed if the condition fails.
    """
    if not condition:
        print(red(message))
        sys.exit(0)


def build_check_message(o1, o2, type, message=None):
    """Constructs failure message for validation.

    Args:
        o1 (comparable): Any comparable value.
        o1 (comparable): Any value comparable to o1.
        type (str): String representation of the comparison performed.
        message (str, optional): Message to display if comparison fails.

    Returns:
        str: Failure message.
    """
    msg = f"Check failed: {o1} {type} {o2}"
    if message:
        msg += f". {message}"
    return red(msg)


def check_eq(o1, o2, message=None):
    """Validates o1 == o2. Produces error message if not.

    Args:
        o1 (comparable): Any comparable value.
        o1 (comparable): Any value comparable to o1.
        message (str, optional): Message to display if comparison fails.
    """
    if o1 != o2:
        print(build_check_message(o1, o2, "!=", message))
        sys.exit(0)


def check_ne(o1, o2, message=None):
    """Validates o1 != o2. Produces error message if not.

    Args:
        o1 (comparable): Any comparable value.
        o1 (comparable): Any value comparable to o1.
        message (str, optional): Message to display if comparison fails.
    """
    if o1 == o2:
        print(build_check_message(o1, o2, "==", message))
        sys.exit(0)


def check_ge(o1, o2, message=None):
    """Validates o1 >= o2. Produces error message if not.

    Args:
        o1 (comparable): Any comparable value.
        o1 (comparable): Any value comparable to o1.
        message (str, optional): Message to display if comparison fails.
    """
    if o1 < o2:
        print(build_check_message(o1, o2, "<", message))
        sys.exit(0)


def check_gt(o1, o2, message=None):
    """Validates o1 > o2. Produces error message if not.

    Args:
        o1 (comparable): Any comparable value.
        o1 (comparable): Any value comparable to o1.
        message (str, optional): Message to display if comparison fails.
    """
    if o1 <= o2:
        print(build_check_message(o1, o2, "<=", message))
        sys.exit(0)


def check_le(o1, o2, message=None):
    """Validates o1 <= o2. Produces error message if not.

    Args:
        o1 (comparable): Any comparable value.
        o1 (comparable): Any value comparable to o1.
        message (str, optional): Message to display if comparison fails.
    """
    if o1 > o2:
        print(build_check_message(o1, o2, ">", message))
        sys.exit(0)


def check_lt(o1, o2, message=None):
    """Validates o1 < o2. Produces error message if not.

    Args:
        o1 (comparable): Any comparable value.
        o1 (comparable): Any value comparable to o1.
        message (str, optional): Message to display if comparison fails.
    """
    if o1 >= o2:
        print(build_check_message(o1, o2, ">=", message))
        sys.exit(0)
