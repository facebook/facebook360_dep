#!/usr/bin/env python3
# Copyright 2004-present Facebook. All Rights Reserved.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Back end for the logging used for printing to the terminal box in the UI.

The LogReader provides general functionality for different logging methods (i.e. info, warnings,
and errors). It additionally handles the catching of output from run processes and displaying
them to the parent UI process. The LogReader is tied to a specific application run.

Example:
    Any tab that wishes to print to the UI-interfaced terminal should instantiate a LogReader,
    as seen in common.py:

        >>> log_reader = LogReader(qt_text_edit, parent, log_file)
"""

import datetime
import os
import re
import sys

from PyQt5 import QtCore
from PyQt5.QtCore import pyqtSignal, pyqtSlot, QProcess, QTextCodec
from PyQt5.QtGui import QTextCursor


class LogReader(QtCore.QObject):

    """Back end for the logging used for printing to the terminal box in the UI.

    Attributes:
        app_name (str): Name of the binary being executed.
        cursor_output (QtWidgets.QTextCursor): Cursor in the Qt Textbox.
        decoder_stdout (QTextCodec): Codec for decoding the output.
        gb (QtWidgets.QGroupBox): Group box for the tab.
        log_f (file handle): File handle to the local logging file.
        log_file (str): Path to the local logging file.
        log_textbox (QtWidgets.QPlainTextEdit): Textbox where logs are displayed.
        parent (App(QDialog)): Object corresponding to the parent UI element.
        process_signal (pyqtSignal): Signal tying signals caught by the LogReader to event.
        processes (dict[str, QtCore.QProcess]): Map from process names to processes.
        progress (str): Token used for displaying progress in the render application (used
            for merging double outputs).
        tab_index (int): Index of which tab the app is being run on.
        tab_widget (Union[Background, Calibration, DepthEstimation, Export]): Widget of any
            of the tabs. This can be extended if the UI is expanded.
    """

    process_signal = pyqtSignal(str)

    def __init__(self, log_textbox, parent, log_file):
        """Initializes a LogReader with the default display configuration.

        Args:
            log_textbox (QtWidgets.QPlainTextEdit): Textbox where logs are displayed.
            parent (App(QDialog)): Object corresponding to the parent UI element.
            log_file (str): Path to the logging file on disk.
        """
        super().__init__()
        self.parent = parent
        self.processes = {}
        self.decoder_stdout = QTextCodec.codecForLocale().makeDecoder()
        self.log_textbox = log_textbox
        self.cursor_output = self.log_textbox.textCursor()
        self.log_textbox.setReadOnly(True)
        self.log_textbox.setMaximumBlockCount(1000)  # limit console to 1000 lines
        self.log_str = ""
        self.progress = "█"
        self.process_signal.connect(self.append_output)
        self.app_name = ""
        self.gb = None

        # If log part of a tab widget we can switch to that tab when we have logs to show
        self.tab_widget = None
        self.tab_index = -1

        self.log_file = log_file
        os.makedirs(os.path.dirname(self.log_file), exist_ok=True)
        self.log_f = open(self.log_file, "w")

    def close_log_file(self):
        """Releases the file handle on the logging file."""
        self.log_f.close()

    def get_process(self, id):
        """Gets the QT process associated with a name.

        Args:
            id (str): Name of the process.

        Returns:
            QtCore.QProcess: Process that is being run.
        """
        return self.processes[id]

    def get_processes(self):
        """Finds all running processes.

        Returns:
            list[QtCore.QProcess]: List of running processes.
        """
        return self.processes

    def setup_process(self, id):
        """Configures environment variables and displays pre-run warnings.

        Args:
            id (str): Name of the process.
        """
        if id in self.processes:
            self.log_warning(f"Process with requested ID already exists: {id}")
            return
        process = QtCore.QProcess(self)
        process.setProcessChannelMode(QProcess.MergedChannels)
        self.processes[id] = process
        process.readyReadStandardOutput.connect(lambda: self.read_stdout(id))
        process.finished.connect(
            lambda exitCode, exitStatus: self.parent.on_process_finished(
                exitCode, exitStatus, id
            )
        )
        env = QtCore.QProcessEnvironment.systemEnvironment()
        env.insert("GLOG_alsologtostderr", "1")
        env.insert("GLOG_stderrthreshold", "0")
        env.insert("PYTHONUNBUFFERED", "1")
        env.insert("PYTHONIOENCODING", "utf-8")
        process.setProcessEnvironment(env)

    def start_process(self, id, cmd):
        """Begins executing a process.

        Args:
            id (str): Name of the process.
            cmd (str): Command to be run in the process.
        """
        if id in self.processes:
            self.processes[id].start(cmd)

    def remove_processes(self):
        """Removes all processes from the list of queued processes."""
        ids = list(self.processes.keys())
        for id in ids:
            self.remove_process(id)

    def remove_process(self, id):
        """Remove a process from the list of queued processes.

        Args:
            id (str): Name of the process.
        """
        if self.processes.pop(id, None):
            self.log_notice(f'Done: "{id}"')

    def end_process(self, id, signal=None):
        """Stops a running process.

        Args:
            id (str): Name of the process.
            signal (str, optional): Termination signal to send to the process.
        """
        if id in self.processes:
            self.log_notice(f'Ending process: "{id}"')
            self.processes[id].terminate()  # let it die

    def end_all_processes(self, signal="kill"):
        """Stops all running processes.

        Args:
            signal (str, optional): Termination signal to send to the process.
        """
        ids = list(self.processes.keys())
        for id in ids:
            self.end_process(id, signal)

    def kill_all_processes(self):
        """Sends a SIGKILL to all running processes."""
        self.end_all_processes("kill")

    def terminate_all_processes(self):
        """Sends a SIGTERM to all running processes."""
        self.end_all_processes("terminate")

    def is_running(self, id=None):
        """Check whether or not a process is running.

        Args:
            id (str, optional): Name of the process.

        Returns:
            QtCore.QProcess.ProcessState: State of the associated process.
        """
        if not id:
            return any(p.state() for p in self.processes.values())
        else:
            return self.process[id].state()

    def set_tab_widget(self, tab_widget, tab_index):
        self.tab_widget = tab_widget
        self.tab_index = tab_index

    @pyqtSlot()
    def read_stdout(self, id):
        """Print stdout from a particular process execution to the log.

        Args:
            id (str): Name of the process.
        """
        if id not in self.processes:
            return
        log = self.decoder_stdout.toUnicode(self.processes[id].readAllStandardOutput())
        self.process_signal.emit(log)

    def are_same_progress(self, str1, str2):
        """Whether or not two names correspond to the same process.

        Args:
            str1 (str): Name of the first process.
            str2 (str): Name of the second process.

        Returns:
            bool: Whether or not they are the same QProcess.
        """
        # Format: █ ProcessName: ...
        if all(x.startswith(self.progress) for x in [str1, str2]):
            # Format: █ ProcessName: ProcessSubname |███...
            return str1.split("|")[0] == str2.split("|")[0]
        return False

    def merge_progress_lines(self, lines):
        """Merge the stdout lines and overwrite the first with the second.

        Args:
            lines (list[str]): Lines of the log to be displayed.

        Returns:
            list[str]: List with lines merged that start with the same token.
        """
        lines_clean = []
        prev = lines[0]
        for i in range(1, len(lines)):
            if not self.are_same_progress(prev, lines[i]):
                lines_clean.append(prev)
            prev = lines[i]
        lines_clean.append(prev)
        return lines_clean

    @pyqtSlot(str)
    def append_output(self, text):
        """Print output to the end of the log.

        Args:
            text (str): Text being added.
        """
        # Remove trailing newlines and split text in lines
        # Also split on carriage returns so we can deal with progress bar
        text = text.strip()
        lines = re.split("\n|\r", text)

        # Merge multiple lines that show progress
        text_clean = "\n".join(self.merge_progress_lines(lines))

        # HACK: To avoid extraneous 0%, 0 workers, 00:00:00 progress with
        # not progress prefix
        if "0% (Workers: 0) (0:00:00)" in text_clean:
            return

        if text_clean == "":
            return

        # Select last line
        tb = self.log_textbox
        tb.moveCursor(QTextCursor.End, QTextCursor.MoveAnchor)
        tb.moveCursor(QTextCursor.StartOfLine, QTextCursor.MoveAnchor)
        tb.moveCursor(QTextCursor.End, QTextCursor.KeepAnchor)
        last_line = tb.textCursor().selectedText()

        # Replace last line with new line if previous and current lines show progress bar of the
        # same process
        if self.are_same_progress(last_line, lines[0]):
            # Delete last line
            tb.textCursor().removeSelectedText()
            tb.textCursor().deletePreviousChar()
        else:
            tb.moveCursor(QTextCursor.End, QTextCursor.MoveAnchor)
        text_clean = f"\n{text_clean}"

        if text_clean.startswith("<font color"):
            tb.appendHtml(text_clean)
        else:
            tb.insertPlainText(text_clean)
        self.log_f.write(text_clean)
        self.log_f.flush()

        # Scroll to last line
        cursor = self.log_textbox.textCursor()
        cursor.movePosition(QTextCursor.End)
        cursor.movePosition(
            QTextCursor.Up if cursor.atBlockStart() else QTextCursor.StartOfLine
        )
        self.log_textbox.setTextCursor(cursor)

    @pyqtSlot(str)
    def switch_tab(self):
        """Change the actively displayed tab to the current tab."""
        if self.tab_widget and self.tab_index > 0:
            self.tab_widget.setCurrentIndex(self.tab_index)

    @pyqtSlot(str)
    def log(self, str):
        """Write plain text to the terminal.

        Args:
            str (str): Text to be written to the UI terminal.
        """
        self.process_signal.emit(str)

    def colored_html(self, str, color):
        self.log_textbox.appendHtml(f'<font color="{color}">{str}</font>')
        self.log_textbox.appendHtml(f'<font color="white"></font>')

    @pyqtSlot(str)
    def log_notice(self, str):
        """Write plain text to the terminal in notice highlight (green).

        Args:
            str (str): Text to be written to the UI terminal.
        """
        str = f"[NOTICE] {str}"
        self.log(self.colored_html(str, "green"))
        self.switch_tab()

    @pyqtSlot(str)
    def log_warning(self, str):
        """Write plain text to the terminal in warning highlight (yellow).

        Args:
            str (str): Text to be written to the UI terminal.
        """
        str = f"[WARNING] {str}"
        self.log(self.colored_html(str, "yellow"))
        self.switch_tab()

    @pyqtSlot(str)
    def log_error(self, str):
        """Write plain text to the terminal in error highlight (red).

        Args:
            str (str): Text to be written to the UI terminal.
        """
        # Logs will not be seen after we quit the app. Show error in terminal
        print(str)
        sys.exit(0)

    @pyqtSlot(str)
    def log_header(self):
        """Write plain text header to the terminal."""
        self.log(
            f"----- {datetime.datetime.now().strftime('%Y-%m-%d %I:%M:%S %p')} -----"
        )
