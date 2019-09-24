#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Unit test class for Calibration.

This class subclasses the DepTest class, which provides some utility functions around
the base unittest.TestCase class. This script can be run as part of the overall test suite
via run_tests.py or standalone.

Example:
    To run the test independently (which will produce the standard Python unittest success
    output), simply run:

        $ python test_calibration.py \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import os

import test_config as config
from test_master_class import DepTest, generic_main


class CalibrationTest(DepTest):

    """Unit test class for Calibration.

    Attributes:
        name (str): String representation of the class name.
    """

    def setup_flags(self):
        """Defines default I/O paths for calibration tests."""
        self.io_args.color = self.io_args.color_full
        self.io_args.rig_in = self.io_args.rig
        self.io_args.matches = os.path.join(self.io_args.output_root, "matches.json")
        self.io_args.rig_out = os.path.join(self.io_args.output_root, "rig.json")

    def _get_setup(self, dataset_name):
        """Get the test setup for a dataset.

        Args:
            dataset_name (str): Name of the dataset.
            app_name (str): Name of the app to be tested.

        Returns:
            dict[str, _]: Map of names to attributes for the particular test.
        """
        for potential_setup in self.setup:
            for dataset in potential_setup["datasets"]:
                if dataset_name in dataset:
                    test_setup = potential_setup
                    self.io_args.color = os.path.join(self.io_args.input_root, dataset)
                    return test_setup

    def _calibration_error_test(self, dataset_name, app_name):
        """Generic error test on a dataset configured with res/test/translator.json.

        Args:
            dataset_name (str): Name of the dataset.
            app_name (str): Name of the app to be tested.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        setup = self._get_setup(dataset_name)
        try:
            self.run_app(app_name)
        except Exception:
            self.check_correct_error(setup=setup)
        else:
            self.check_correct_error(setup=setup)

    def _calibration_test(self, dataset_name):
        """Generic test for Calibration on a dataset configured with res/test/translator.json.

        Args:
            dataset_name (str): Name of the dataset.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        self.setup_flags()
        test_setup = self._get_setup(dataset_name)
        if test_setup["error_test"]:
            self._calibration_error_test(dataset_name, "Calibration")
        else:
            self.run_app("Calibration")
            record = parse_calibration_results(
                self.io_args.log_dir, bin_name="Calibration"
            )
            self.check_metrics(record, test_setup)

    def test_shuffled(self):
        """Run test on matches shuffled around for Calibration.

        Raises:
            AssertionError: If incorrect error is produced.
        """
        self.setup_flags()
        self.io_args.matches = os.path.join(
            self.io_args.output_root, "shuffled", "matches.json"
        )
        self._calibration_error_test("shuffled", "GeometricCalibration")

    def test_blank(self):
        """Run test on blank images. Error indicating too few features should result.

        Raises:
            AssertionError: If incorrect or no error is produced.
        """
        self._calibration_test("blank")

    def test_rotated(self):
        """Run test on rotated images for Calibration. Error indicating rotated images
        should result.

        Raises:
            AssertionError: If incorrect or no error is produced.
        """
        self._calibration_test("rotated")

    def test_color(self):
        """Run test on full color images for Calibration.

        Raises:
            AssertionError: If incorrect results are produced.
        """
        self._calibration_test("color_full")


def _get_line_with_str(lines, string, index):
    """Extracts the index-th line containing a string.

    Args:
        lines (list[str]): List of strings (typically lines from a file).
        string (str): Substring to filter lines by.
        index (int): Which filtered string to return.

    Returns:
        str: The index-th fitlered string. Returns None if no such string exists.
    """
    relevant_lines = [line for line in lines if string in line]
    if len(relevant_lines) == 0 or (index != -1 and len(relevant_lines) <= index):
        return None
    return relevant_lines[index]


def _get_time_split(timing_line):
    """Extracts timing information from a boost-formatted timestamp, i.e.:
    <timestamp> GeometricCalibration.cpp:<line_number> Aggregate timing:  <wall>s wall, <user>s user + <system>s system = <cpu>s CPU (<pct>%)

    Args:
        timing_line (str): Line of the above format.

    Returns:
        dict[str, float]: Map with keys "cpu" and "wall" for the respective readings.
    """
    cpu_time_half = timing_line.split("CPU")[0].strip()
    cpu_time = float(cpu_time_half.split(" ")[-1].replace("s", ""))

    wall_time_half = timing_line.split("wall")[0].strip()
    wall_time = float(wall_time_half.split(" ")[-1].replace("s", ""))

    return {"cpu": cpu_time, "wall": wall_time}


def parse_match_corners_results(log_dir, bin_name="MatchCorners", parse_timing=False):
    """Extracts results of the most recent MatchCorners glog file.

    Args:
        log_dir (str): Directory with the saved glog files.
        bin_name (str): Name of the executed binary (must match prefix of filename).
        parse_timing (bool): Whether or not to parse timing from the glog file.

    Returns:
        dict[str, float]: Parsed values from the MatchCorners glog file.
    """
    info_path = os.path.join(log_dir, f"{bin_name}.INFO")
    with open(info_path, "r") as f:
        lines = f.readlines()

    records = {}

    features_str = "cam0 accepted corners:"
    features_line = _get_line_with_str(lines, features_str, 0)
    features_half = features_line.split(features_str)[1].strip()
    records["match_corners_count"] = int(features_half.split(" ")[0])

    if parse_timing:
        param_to_timing_str = {
            "match_corners": "Matching stage time",
            "find_corners": "Find corners stage time",
        }

        for param in param_to_timing_str:
            timing_str = param_to_timing_str[param]
            timing_line = _get_line_with_str(lines, timing_str, 0)
            times = _get_time_split(timing_line)
            records[f"{param}_cpu_time"] = times["cpu"]
            records[f"{param}_wall_time"] = times["wall"]
    return records


def parse_calibration_results(
    log_dir, bin_name="GeometricCalibration", parse_timing=False
):
    """Extracts results of the most recent GeometricCalibration glog file.

    Args:
        log_dir (str): Directory with the saved glog files.
        bin_name (str): Name of the executed binary (must match prefix of filename).
        parse_timing (bool): Whether or not to parse timing from the glog file.

    Returns:
        dict[str, float]: Parsed values from the GeometricCalibration glog file.
    """
    info_path = os.path.join(log_dir, f"{bin_name}.INFO")
    with open(info_path, "r") as f:
        lines = f.readlines()
        for line in lines:
            if "Warning" in line:
                lines.remove(line)

    records = {}

    traces_str = "nonempty traces"
    traces_line = _get_line_with_str(lines, traces_str, 0)
    traces_half = traces_line.split("found ")[1].strip()
    records["calibration_trace_count"] = int(traces_half.split(" ")[0])

    error_str = "median"
    error_line = _get_line_with_str(lines, error_str, -1)
    error_half = error_line.split(error_str)[1].strip()
    records["calibration_median_error"] = float(error_half.split(" ")[0])

    if parse_timing:
        timing_str = "Aggregate timing"
        timing_line = _get_line_with_str(lines, timing_str, 0)
        times = _get_time_split(timing_line)
        records["calibration_cpu_time"] = (times["cpu"],)
        records["calibration_wall_time"] = times["wall"]
    return records


if __name__ == "__main__":
    generic_main([CalibrationTest])
