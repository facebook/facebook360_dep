#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Defines parent class and common functionality used amongst test files.

The DepTest parent class is defined, which all app tests should inherit from. This
class defines functionality for checking results of the tests and also provides default
paths for where certain image types are located. Individual tests should also make use
of the generic_main for execution, which performs the requisite setup and then runs
a unittest test suite across the desired classes.
"""

import argparse
import filecmp
import glob
import json
import os
import re
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "util"))

from . import test_config as config
from . import test_io
from scripts.util.system_util import image_type_paths, run_command
from .test_util import min_max_frame_from_data_dir

parser = argparse.ArgumentParser()
parser.add_argument("--binary_dir", help="Path to binary files on disk", required=True)
parser.add_argument(
    "--dataset_root",
    help="Endpoint to root directory where *.tar data files are hosted",
    required=True,
)


def listdir_nohidden(path):
    """Gets all the non-hidden files at specified path.

    Args:
        path (str): Path on disk.

    Returns:
        list[str]: All visible files existing within the specified path.
    """
    return glob.glob(os.path.join(path, "*"))


def get_sorted_json_string(file):
    with open(file, "r") as f:
        json_data = json.load(f)
    return json.dumps(json_data, sort_keys="True")


def json_cmp(file1, file2):
    return get_sorted_json_string(file1) == get_sorted_json_string(file2)


def test_file_cmp(file1, file2):
    if os.path.splitext(file1)[1] == ".json":
        return json_cmp(file1, file2)
    else:
        return filecmp.cmp(file1, file2)


def dir_trees_equal(dir1, dir2):
    """Determines whether or not two subtrees rooted at dir1 and dir2 are identical
    when only comparing visible files.

    Args:
        dir1 (str): Name of a directory on disk.
        dir2 (str): Name of another directory on disk.

    Returns:
        bool: Whether or not the two directories are identical.
    """
    dirs_cmp = filecmp.dircmp(dir1, dir2)
    if len(listdir_nohidden(dir1)) != len(listdir_nohidden(dir2)):
        return False

    (_, mismatch, errors) = filecmp.cmpfiles(
        dir1, dir2, dirs_cmp.common_files, shallow=False
    )
    mismatches = [file for file in mismatch if not file.startswith(".")]

    if len(errors) > 0:
        return False

    if len(mismatches) > 0:
        for mismatch in mismatches:
            if os.path.splitext(mismatch)[1] != ".json":
                return False
            if not json_cmp(os.path.join(dir1, mismatch), os.path.join(dir2, mismatch)):
                return False

    for common_dir in dirs_cmp.common_dirs:
        new_dir1 = os.path.join(dir1, common_dir)
        new_dir2 = os.path.join(dir2, common_dir)
        if not dir_trees_equal(new_dir1, new_dir2):
            return False
    return True


def camel_to_snake(str):
    s1 = re.sub("(.)([A-Z][a-z]+)", r"\1_\2", str)
    return re.sub("([a-z0-9])([A-Z])", r"\1_\2", s1).lower()


class DepTest(unittest.TestCase):

    """Master class for all app unit tests. DepTest is never itself directly run.
    Any additional tests that are created should subclass this to leverage the created
    testbed. unittest.TestCase is subclassed to provide functionality for execution.

    Attributes:
        track_code_coverage (bool): Whether or not to save coverage on running applications.
    """

    track_code_coverage = True

    def setUp(self):
        """Sets up various environment variables needed for producing logs."""
        os.environ["GLOG_alsologtostderr"] = "1"
        os.environ["GLOG_stderrthreshold"] = "0"
        os.makedirs(self.io_args.output_root, exist_ok=True)

    @staticmethod
    def _bin_name_to_profile_name(binary_name, output_root):
        """Gets the name of a profiler of the specified binary.

        Args:
            binary_name (str): Name of the binary whose profile is desired.
            output_root (str): Directory where outputs are to be saved.

        Returns:
            str: Path to .prof.raw corresponding to this binary execution.
        """
        return os.path.join(output_root, binary_name + ".prof.raw")

    def check_binary_availability(self, binary_name):
        """Checks whether or not the binary exists. Exits with error code 1 if
        the binary does not exist.

        Args:
            binary_name (str): Name of the binary to be checked.
        """
        binary_path = os.path.join(self.binary_dir, binary_name)
        if not os.path.isfile(binary_path):
            print(f"No application {binary_name} found at {binary_path}")
            exit(1)

    def gen_command(self, binary_name, args_string, output_root, log_dir=None):
        """Constructs the command to be run to execute the desired binary with
        the arguments specified and saving the outputs in the specified location.

        Args:
            binary_name (str): Name of the binary.
            args_string (str): CLI arguments for the binary.
            output_root (str): Root directory where the outputs are to be saved.
            log_dir (str, optional): Path to the directory where glog files are to be saved.

        Returns:
            str: Command to run.
        """
        self.check_binary_availability(binary_name)
        if not log_dir:
            log_dir = os.path.join(self.io_args.output_root, "log")
        #### write out the file
        globals_prefix = "GLOG_alsologtostderr=1 GLOG_stderrthreshold=0 "
        globals_prefix = globals_prefix + "GLOG_log_dir=" + log_dir + " "
        if self.track_code_coverage:
            globals_prefix = (
                globals_prefix
                + "LLVM_PROFILE_FILE="
                + self._bin_name_to_profile_name(binary_name, output_root)
                + " "
            )

        return (
            globals_prefix
            + os.path.join(self.binary_dir, binary_name)
            + " "
            + args_string
        )

    def gen_command_flagfile(self, binary_name):
        """Constructs the command to be run to execute the desired binary with
        arguments pulled from the corresponding flagfile.

        Args:
            binary_name (str): Name of the binary.

        Returns:
            str: Command to run.
        """
        args_string = self.gen_args_flagfile(binary_name)
        return self.gen_command(
            binary_name, args_string, self.io_args.output_root, self.io_args.log_dir
        )

    def gen_args_flagfile(self, binary_name):
        """Constructs CLI arguments from the flagfile, assuming the format in res/test/.

        Args:
            binary_name (str): Name of the binary.

        Returns:
            str: Space-separated string of CLI arguments (e.g.
                "--example1 <X> --example2 <Y>")
        """
        parent_dir = os.path.dirname(os.path.abspath(__file__))
        res_dir = os.path.abspath(
            os.path.join(os.path.dirname(os.path.dirname(parent_dir)), "res")
        )
        flagfile = os.path.join(res_dir, "test", f"{camel_to_snake(binary_name)}.flags")
        with open(flagfile, "r") as f:
            flag_lines = f.readlines()

        flags = []
        for flag_line in flag_lines:
            flag_line = flag_line.strip()
            if "#" in flag_line:  # marks a comment in the flagfile
                continue

            if "=" not in flag_line:  # I/O flag where we must pull from the args
                flag_name = flag_line.replace("--", "")
                flag_value = self.io_args.__dict__[flag_name]
                flags.append(f"--{flag_name}={flag_value}")
            else:
                flags.append(flag_line)

        args_string = " ".join(flags)
        return args_string

    def run_app(self, binary_name, args=None, log_file=None):
        """Runs desired binary with arguments pulled from the corresponding flagfile if
        None are passed in.

        Args:
            binary_name (str): Name of the binary.
            args (str, optional): CLI arguments. If None is passed in, this is pulled from
                the corresponding flagfile from res/test/.
            stream (bool, optional): Whether or not to stream output to the screen.
        """
        if args is None:
            cmd = self.gen_command_flagfile(binary_name)
        else:
            cmd = self.gen_command(
                binary_name, args, self.io_args.output_root, self.io_args.log_dir
            )
        run_command(cmd, file_fn=log_file)

    def assert_within_atol(self, actual, desired, tolerance):
        """Asserts whether or not the actual value falls within an absolute tolerance of
        an expected result.

        Args:
            actual (float): Value to be used as our "actual" result.
            desired (float): Expected result of the metric.
            tolerance (float): Variance (two-sided) bound actual should fall within.

        Raises:
            AssertionError: If the absolute error is above the tolerance.
        """
        offset = abs(actual - desired)
        self.assertLess(offset, tolerance)

    def assert_within_rtol(self, actual, desired, tolerance):
        """Asserts whether or not the actual value falls within a relative tolerance of
        an expected result.

        Args:
            actual (float): Value to be used as our "actual" result.
            desired (float): Expected result of the metric.
            tolerance (float): Variance (two-sided) bound actual should fall within relative
                to the desired result (i.e. a percentage).

        Raises:
            AssertionError: If the relative error is above the tolerance.
        """
        offset = abs(actual - desired) / abs(desired)
        self.assertLess(offset, tolerance)

    def check_metrics(self, record, setup=None):
        """Asserts whether or not all the metrics present in record align with those expected
        from the truth to the specified tolerance.

        Args:
            record (dict[str, float]): Values of relevant metrics from the test.
            setup (dict[str, _], optional): Setup for the test.

        Raises:
            AssertionError: If any of the metrics does not fall within the specified tolerance
                corresponding to it in the truth.
        """
        if setup is None:
            setup = self.setup
        for metric in setup["metrics"]:
            if "absolute_tolerance" in metric:
                tolerance = metric["absolute_tolerance"]
                confirm_metric = self.assert_within_atol
            else:
                tolerance = metric["relative_tolerance"]
                confirm_metric = self.assert_within_rtol

            if isinstance(record[metric["name"]], list):
                for actual, desired in zip(
                    record[metric["name"]], metric["expected_result"]
                ):
                    confirm_metric(actual, desired, tolerance)
            else:
                confirm_metric(
                    record[metric["name"]], metric["expected_result"], tolerance
                )

    def check_correct_error(self, setup=None):
        """Assert whether or not the error produced by the binary being executed is what
        is expected from the truth.

        Args:
            setup (dict[str, _], optional): Setup for the test.

        Raises:
            AssertionError: If the error produced is different from that expected.
        """
        if setup is None:
            setup = self.setup
        correct_error = False
        error_log = os.path.join(self.io_args.log_dir, setup["error_location"])

        if os.path.exists(error_log):
            with open(error_log, "r") as f:
                for line in f:
                    if setup["error_string"] in line:
                        correct_error = True
                        break
        assert correct_error

    def check_against_truth(self, truth, output):
        """Asserts whether or not truth and output paths are identical.

        Args:
            truth (str): Path to file or directory on disk containing the expected result files.
            output (str): Path to file or directory on disk containing the test result files.

        Raises:
            AssertionError: If the visible entries in truth and output paths differ.
        """
        if os.path.isdir(output):
            assert dir_trees_equal(truth, output)
        else:
            assert test_file_cmp(truth, output)


def prepare_run(test_class, testing_dir, rig, binary_dir):
    """Sets up all the standard paths for the test execution. This will create a
    class attribute called io_args from which paths can be accessed (i.e. self.io_args.color).

    Args:
        test_class (class): Class that subclasses DepTest (e.g. AlignColorsTest).
        testing_dir (str): Directory where test results will be saved on disk.
        rig (str): Name of the rig json file.
        binary_dir (str): Path to directory where binaries are compiled on disk.
    """
    test_class.binary_dir = binary_dir
    test_class.dataset = os.path.basename(testing_dir)
    test_class.rig = rig

    io_dict = {}
    for image_type in image_type_paths:
        io_dict[image_type] = os.path.join(testing_dir, image_type_paths[image_type])
        os.makedirs(io_dict[image_type], exist_ok=True)

    # Additional attributes not defaulted by their image type
    io_dict["testing_dir"] = testing_dir
    io_dict["input_root"] = testing_dir
    io_dict["output"] = testing_dir
    io_dict["output_root"] = testing_dir

    io_dict["log_dir"] = os.path.join(testing_dir, "logs")
    io_dict["color_full"] = os.path.join(testing_dir, "color_full")
    io_dict["rig"] = os.path.join(testing_dir, rig)
    io_dict["rig_in"] = io_dict["rig"]

    # There are some datasets (and tests) where no frame images are needed
    input_dirs = [
        os.path.join(io_dict["color"], config.TEST_LEVEL),
        io_dict["color_full"],
    ]

    first, last = None, None
    for input_dir in input_dirs:
        try:
            first, last = min_max_frame_from_data_dir(input_dir)
            break
        except Exception:
            continue

    io_dict["first"] = first
    io_dict["last"] = last
    io_dict["frame"] = first
    io_dict["binary_dir"] = binary_dir

    test_class.io_args = SimpleNamespace(**io_dict)


def generic_main(test_classes, loader=None):
    """Generic main function for running tests across classes. Execution follows
    using the Python unittest library, meaning successes, errors, and failures
    are produced per the library's standard. If any test fails, we exit with
    an error code of 1.

    Args:
        test_classes (list[class]): List of classes for which tests are to be run.
    """
    args = parser.parse_args()
    res_dir = os.path.join(Path(__file__).parents[2], "res", "test")
    with open(os.path.join(res_dir, "translator.json")) as f:
        tests_setup = json.load(f)

    if loader is None:
        loader = test_io.Loader()

    testing_dir = "tmp"
    truth_dir = os.path.join(testing_dir, "truth")
    test_suites = []
    for test_class in test_classes:
        test_datasets = set()
        test_truths = set()
        for test_setup in tests_setup[test_class.__name__]:
            test_datasets = test_datasets.union(test_setup["datasets"])
            if "truth" in test_setup:
                test_truths.add(test_setup["truth"])

        for dataset in test_datasets:
            print(f"Preparing input: {dataset}...")
            if dataset in image_type_paths:
                dst = os.path.join(
                    testing_dir, os.path.dirname(image_type_paths[dataset])
                )
            else:
                dst = testing_dir
            loader.download(args.dataset_root, dataset, dst)

        for truth in test_truths:
            print(f"Preparing truth: {truth}...")
            loader.download(args.dataset_root, truth, truth_dir)

        prepare_run(test_class, testing_dir, test_setup["rig"], args.binary_dir)

        test_setups = tests_setup[test_class.__name__]
        test_class.setup = test_setups if len(test_setups) > 1 else test_setups[0]
        test_class.io_args.truth_dir = truth_dir
        test_class.replay_metadata = {}

        test_suites.append(unittest.TestLoader().loadTestsFromTestCase(test_class))

    result = unittest.TextTestRunner().run(unittest.TestSuite(test_suites))
    if len(result.errors) > 0 or len(result.failures) > 0:
        exit(1)
