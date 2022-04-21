#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Entrypoint for render pipeline front-end user interface.

This serves as the main method for interacting with the pipeline internals for any
users who wish to get results rather than modify the internals. We suggest using
run.py to spawn the UI, which calls this script, rather than directly calling this,
since it makes assumptions on targets being built. Running this script will create
a PyQT window if executed properly.

Example:
    Running dep.py will produce a UI if properly executed to facilitate interacting with all
    parts of the pipeline. Once again, run.py is the proper way to spawn the UI, but
    for people looking to extend the code, to execute this script:

        $ python dep.py \
          --host_os=0 \
          --project_root=/path/to

    Note: host_os refers to the OSType enum defined in scripts/util/system_util.py

Attributes:
    FLAGS (absl.flags._flagvalues.FlagValues): Globally defined flags for dep.py.
"""

import datetime
import os
import pickle
import signal
import sys

import qdarkstyle
from absl import app, flags
from PyQt5 import QtCore, uic
from PyQt5.QtCore import Qt
from PyQt5.QtGui import QFont
from PyQt5.QtWidgets import QApplication, QDialog

dir_scripts = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dir_root = os.path.dirname(dir_scripts)
sys.path.append(dir_root)
sys.path.append(os.path.join(dir_scripts, "aws"))
sys.path.append(os.path.join(dir_scripts, "render"))
sys.path.append(os.path.join(dir_scripts, "util"))

import common
import dep_util
import scripts.render.config as config
import scripts.render.glog_check as glog
import verify_data
from background import Background
from calibration import Calibration
from depth_estimation import DepthEstimation
from export import Export
from scripts.aws.util import AWSUtil
from scripts.render.network import Address
from scripts.render.setup import RepeatedTimer
from scripts.util.system_util import get_catchable_signals

FLAGS = flags.FLAGS

sections = []


def close_section_logs():
    """Cleanup method to release file handles on logging files."""
    for s in sections:
        if "log_reader" in dir(s):
            if FLAGS.verbose:
                print(glog.green(f"Closing {s.log_reader.log_file}"))
            s.log_reader.close_log_file()


def signal_handler(signum, frame):
    """Catches signals to ensure logs are closed in case of app termination.

    Args:
        signal (signal.signal): Type of signal.
        frame (frame): Stack frame.
    """
    print(glog.red(f"Caught signal {signal.Signals(signum).name}. Exiting..."))
    close_section_logs()
    sys.exit(0)


for sig in get_catchable_signals():
    signal.signal(sig, signal_handler)


class App(QDialog):

    """Main dialog box for the app initialized by QT.

    Attributes:
        aws_util (AWSUtil): Util instance for ease of interacting with AWS.
        background (Background): Instance of the background tab UI element.
        calibrate (Calibration): Instance of the calibration tab UI element.
        depth (DepthEstimation): Instance of the depth estimation tab UI element.
        dlg (App(QDialog)): Main dialog box for the app initialized by QT.
        export (Export): Instance of the export tab UI element.
        full_size (QSize): Size of the full-sized UI (i.e. size on spawn).
        is_aws (bool): Whether or not this is an AWS render.
        is_lan (bool): Whether or not this is a LAN render.
        path_project (str): Path to the project root.
        sections (list[_]): List of class instances corresponding to the tabs.
        ui_flags (absl.flags._flagvalues.FlagValues): Copy of globally defined flags for dep.py.
    """

    def __init__(self):
        """Creates the main UI dialog box and sets up the sections in the desired layout."""
        QDialog.__init__(self)
        self.font_type = "Courier"
        self.font_size = 10
        font = QFont(self.font_type, self.font_size)
        font.setFixedPitch(True)
        QApplication.setFont(font, "QPlainTextEdit")
        self.ts_start = datetime.datetime.now()
        self.ui_flags = FLAGS
        self.dlg = uic.loadUi("dep.ui", self)

        has_pickled_project = self.unpickle_project()

        self.verbose = FLAGS.verbose
        self.s3_ignore_fullsize_color = FLAGS.s3_ignore_fullsize_color
        self.s3_sample_frame = FLAGS.s3_sample_frame
        self.project_root = FLAGS.project_root
        self.path_project = config.DOCKER_INPUT_ROOT

        self.configure_farm()
        self.full_size = self.dlg.frameSize()

        verify_data.set_default_top_level_paths(self)

        if not has_pickled_project:
            verify_data.verify(self)

        dep_util.set_full_size_widths(self)

        self.calibrate = Calibration(self)
        self.background = Background(self)
        self.depth = DepthEstimation(self)
        self.export = Export(self)
        self.sections = [self.calibrate, self.background, self.depth, self.export]

        global sections
        sections = self.sections

        self.setup_sections_layout()
        self.setup_sections_signals()
        self.setup_project()
        self.setup_clock()

        self.dlg.show()

    def unpickle_project(self):
        pickle_fn = os.path.join(config.DOCKER_INPUT_ROOT, "project.pickle")
        if os.path.isfile(pickle_fn):
            with open(pickle_fn, "rb") as f:
                dict_frames = pickle.load(f)
                for k, v in dict_frames.items():
                    setattr(self, k, v)
                return True
        return False

    def setup_clock(self):
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.update_clock)
        self.timer.start(1000)  # every second
        self.dlg.lcd_clock.setDigitCount(8)
        font = QFont(self.font_type, 2 * self.font_size)
        font.setBold(True)
        self.dlg.lcd_clock.setFont(font)
        self.update_clock(self.ts_start)

    def update_clock(self, start=None):
        if start:
            self.ts_start_clock = start
        now = datetime.datetime.now()
        time_elapsed = str(now - self.ts_start_clock).split(".", 2)[0]
        self.dlg.lcd_clock.display(time_elapsed)

    def configure_farm(self):
        """Sets up credentials in the terminal for an AWS render."""
        project_address = Address(self.project_root)
        self.is_aws = project_address.protocol == "s3"
        self.is_lan = project_address.protocol == "smb"
        if self.is_aws:
            print(glog.green("Configuring AWS parameters..."))
            self.aws_util = AWSUtil(
                config.DOCKER_AWS_CREDENTIALS, s3_url=self.project_root
            )
            self.aws_util.configure_shell(run_silently=not self.verbose)
            fe = self.dlg.gb_file_explorer
            fe.setTitle(f"{fe.title()} (cache)")

            kube_workers = self.aws_util.ec2_get_kube_worker_instances()
            common.set_aws_workers(kube_workers)

    def closeEvent(self, event):
        """Callback event handler for the UI being closed.

        Args:
            event (QEvent): Caught instance of the closing event.
        """
        print(glog.green("Closing app..."))
        close_section_logs()
        event.accept()

    def keyPressEvent(self, event):
        if event.key() == Qt.Key_Escape:
            self.close()

    def auto_terminate_cluster(self):
        """Sets up a repeated background event to terminate AWS clusters."""
        if self.is_aws:
            stats = self.aws_util.ec2_get_kube_stats()
            if stats and stats < config.AUTO_TERMINATE_CPU:
                msg = "Low CPU utilization detected in k8s cluster! Auto-terminating..."
                print(glog.red(msg))
                self.depth.on_terminate_cluster()

    def get_current_section(self):
        """Returns the active section.

        Returns:
            Union[Background, Calibration, DepthEstimation, Export]: Whichever instance
                is active on screen.
        """
        return self.sections[self.dlg.w_steps.currentIndex()]

    def setup_section(self, section, mkdirs=True):
        """Sets up the layout and populates fields in a section.

        Args:
            section (Union[Background, Calibration, DepthEstimation, Export]): UI section.
            mkdirs (bool, optional): Whether or not to make the directories desired.
        """
        section.setup_project(mkdirs)

    def on_changed_section(self):
        """Callback event handler for changing tabs."""
        self.setup_section(self.get_current_section())

    def setup_sections_signals(self):
        """Sets up signal for changing tabs and initializes the start tab."""
        w = self.dlg.w_steps
        w.setCurrentIndex(0)
        w.currentChanged.connect(self.on_changed_section)
        w.setEnabled(False)

    def setup_sections_layout(self):
        """Force margins to 10 pixels on sides and bottom to not waste too much space"""
        for s in self.sections:
            tab = getattr(s.dlg, f"t_{s.tag}", None)
            tab.layout().setContentsMargins(10, 0, 10, 10)

    def setup_project(self):
        """Initializes the file explorer and sets up the tabs."""
        dlg = self.dlg
        w = dlg.w_steps
        w.setEnabled(True)
        w.show()
        common.setup_file_explorer(self)
        for section in self.sections:
            self.setup_section(section)

        for s in self.sections:
            if dep_util.is_tab_enabled(w, s.tag):
                dep_util.switch_tab(w, s.tag)
                break

        # Animate dialog resize
        dep_util.animate_resize(self.dlg, self.dlg.frameSize(), self.full_size)


def main(argv):
    """Runs the UI with the parameters passed in through command line args."""
    qapp = QApplication(sys.argv)
    qapp.setStyleSheet(qdarkstyle.load_stylesheet_pyqt5())
    app = App()
    auto_terminate = RepeatedTimer(config.POLLING_INTERVAL, app.auto_terminate_cluster)
    auto_terminate.start()
    qapp.exec_()
    auto_terminate.stop()


if __name__ == "__main__":
    flags.DEFINE_string("host_os", "", "OS type of calling host")
    flags.DEFINE_string(
        "local_bin", "", "Path local binaries (needed to run GPU-based viewers)"
    )
    flags.DEFINE_string(
        "master", "127.0.0.1", "IP of the master (if rendering on a farm)"
    )
    flags.DEFINE_string(
        "password", "", "Username for mounted network drive (only local farm)"
    )
    flags.DEFINE_string("potential_workers", "", "List of IPs for LAN workers")
    flags.DEFINE_string("project_root", None, "Input root of the project")
    flags.DEFINE_boolean(
        "s3_ignore_fullsize_color", False, "Do not download full-size color from S3"
    )
    flags.DEFINE_string("s3_sample_frame", "", "Sample frame to download from S3")
    flags.DEFINE_string(
        "username", "", "Password for mounted network drive (only local farm)"
    )
    flags.DEFINE_boolean("verbose", False, "Verbose mode")

    # Required FLAGS.
    flags.mark_flag_as_required("project_root")
    app.run(main)
