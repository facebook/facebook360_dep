#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Runs all the unit tests defined in res/test/translator.json.

This is the main entrypoint for running the comprehensive test suite defined across
our applications. All the scripts desired by the specified "type" CLI argument will be run from
the test/ directory. If only a certain subset of the tests are desired, this can be specified in
a separate .json file and passed using the --static CLI flag.

Example:
    For running all the CPU tests, use:

        $ python run_tests.py \
          --type=cpu
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset

    For running a statically-defined subset of the GPU tests, use:

        $ python run_tests.py \
          --type=gpu \
          --static=/path/to/facebook360_dep/static.json \
          --binary_dir=/path/to/facebook360_dep/build/bin \
          --dataset_root=s3://example/dataset
"""

import json
import os
import sys
from pathlib import Path

from test_align_colors import AlignColorsTest
from test_calibration import CalibrationTest
from test_calibration_lib_main import CalibrationLibMainTest
from test_convert_to_binary import ConvertToBinaryTest
from test_derp_cli import DerpCLITest
from test_export_point_cloud import ExportPointCloudTest
from test_generate_camera_overlaps import GenerateCameraOverlapsTest
from test_generate_foreground_masks import GenerateForegroundMasksTest
from test_import_point_cloud import ImportPointCloudTest
from test_layer_disparities import LayerDisparitiesTest
from test_master_class import generic_main, parser
from test_project_equirects_to_cameras import ProjectEquirectsToCamerasTest
from test_raw_to_rgb import RawToRgbTest
from test_rig_aligner import RigAlignerTest
from test_rig_analyzer import RigAnalyzerTest
from test_rig_compare import RigCompareTest
from test_rig_simulator import RigSimulatorTest
from test_simple_mesh_renderer import SimpleMeshRendererTest
from test_upsample_disparity import UpsampleDisparityTest

try:
    import networkx as nx

    load_static = False
except Exception:
    load_static = True


def get_ordered_tests(tests_setup, test_type):
    """Determines the order of tests to be run, filtered to only return the specified type.

    Args:
        tests_setup (dict): Map of test name to its configuration (see: res/test/translator.json).
        test_type (str): Which apps are to be tested. Must be one of "cpu", "gpu", or "both".

    Returns:
        list[str]: Names of the applications in the order they are to be run.
    """
    test_graph = nx.DiGraph()
    for test_app in tests_setup:
        tests = tests_setup[test_app]
        for test in tests:
            if "truth" in test:
                output_node = test["truth"]
            else:
                output_node = f"placeholder_{test_app}"

            test_graph.add_nodes_from(test["datasets"])
            test_graph.add_nodes_from([output_node])
            for dataset in test["datasets"]:
                if test_type == "both" or test["type"] == test_type:
                    print(dataset, output_node)
                    test_graph.add_edge(dataset, output_node, name=test_app)

    ordered_nodes = list(nx.topological_sort(test_graph))
    ordered_tests = []
    for node in ordered_nodes:
        for neighbor in test_graph.neighbors(node):
            test_app = test_graph.get_edge_data(node, neighbor)["name"]
            if test_app not in ordered_tests:
                ordered_tests.append(test_app)
    return ordered_tests


def run_tests(loader=None):
    """Runs tests of the variant specified by CLI arguments. If "cpu" is specified,
    CPU-only tests will be run and similarly for "gpu." Both are run if "both" is
    passed in. If "static" is specified, the tests are run per their order in the
    given static json file. Otherwise, the test order is automatically determined.
    """
    parser.add_argument(
        "--type", help="Type of tests to run (one of: cpu, gpu, both)", required=True
    )
    parser.add_argument(
        "--static",
        help="Static json w/ list of tests (use ONLY if NetworkX unavailable)",
    )
    args = parser.parse_args()

    translator_path = os.path.join(
        Path(os.path.abspath(__file__)).parents[2], "res", "test", "translator.json"
    )
    with open(translator_path) as f:
        tests_setup = json.load(f)

    if load_static or args.static:
        with open(args.static, "r") as f:
            ordered_json = json.load(f)
        ordered_tests = []
        if (args.type == "both" or args.type == "cpu") and "cpu" in ordered_json:
            ordered_tests += ordered_json["cpu"]
        if (args.type == "both" or args.type == "gpu") and "gpu" in ordered_json:
            ordered_tests += ordered_json["gpu"]
    else:
        ordered_tests = get_ordered_tests(tests_setup, args.type)
    test_classes = []
    for test in ordered_tests:
        test_classes.append(getattr(sys.modules[__name__], test))
    generic_main(test_classes, loader)


if __name__ == "__main__":
    run_tests()
