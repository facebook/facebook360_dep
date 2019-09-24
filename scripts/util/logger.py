#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

import logging


class Logger:
    def __init__(self, name=None):
        self.logger = logging.getLogger(name)
        self.logger.setLevel(logging.DEBUG)

        handler = logging.StreamHandler()
        handler.setLevel(logging.DEBUG)

        format = logging.Formatter("%(asctime)s - %(name)s/%(levelname)s: %(message)s")
        handler.setFormatter(format)

        self.logger.addHandler(handler)

    def check(self, condition, message=""):
        if not condition:
            self.logger.error("Failed condition: %s", message)

    def check_lt(self, objL, objR, message=""):
        self.check(objL < objR, "{} < {}. {}".format(objL, objR, message))

    def check_le(self, objL, objR, message=""):
        self.check(objL <= objR, "{} <= {}. {}".format(objL, objR, message))

    def check_gt(self, objL, objR, message=""):
        self.check(objL > objR, "{} > {}. {}".format(objL, objR, message))

    def check_ge(self, objL, objR, message=""):
        self.check(objL >= objR, "{} >= {}. {}".format(objL, objR, message))

    def check_eq(self, objL, objR, message=""):
        self.check(objL == objR, "{} == {}. {}".format(objL, objR, message))

    def check_ne(self, objL, objR, message=""):
        self.check(objL != objR, "{} != {}. {}".format(objL, objR, message))

    def debug(self, message):
        self.logger.debug(message)

    def info(self, message):
        self.logger.info(message)

    def warn(self, message):
        self.logger.warning(message)

    def error(self, message):
        self.logger.error(message)

    def fatal(self, message):
        self.logger.critical(message)
