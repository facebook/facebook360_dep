/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/util/SystemUtil.h"

#include <signal.h>
#include <exception>
#include <stdexcept>

#include <fmt/format.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/Format.h>

DECLARE_bool(help);
DECLARE_bool(helpshort);

namespace fb360_dep {
namespace system_util {

void terminateHandler() {
  std::exception_ptr exptr = std::current_exception();
  if (exptr != 0) {
    try {
      rethrow_exception(exptr);
    } catch (std::exception& ex) {
      LOG(FATAL) << fmt::format("Terminated with exception: {}", ex.what());
    } catch (...) {
      LOG(FATAL) << "Terminated with unknown exception";
    }
  } else {
    LOG(FATAL) << "Terminated due to unknown reason";
  }
}

void sigHandler(int signal) {
#ifdef WIN32
  switch (signal) {
    case SIGINT:
      LOG(FATAL) << "SIGINT";
      break;
    case SIGILL:
      LOG(FATAL) << "SIGILL";
      break;
    case SIGFPE:
      LOG(FATAL) << "SIGFPE";
      break;
    case SIGSEGV:
      LOG(FATAL) << "SIGSEGV";
      break;
    case SIGTERM:
      LOG(FATAL) << "SIGTERM";
      break;
    case SIGBREAK:
      LOG(FATAL) << "SIGBREAK";
      break;
    case SIGABRT:
      LOG(FATAL) << "SIGABRT";
      break;
    default:
      LOG(FATAL) << "UNKNOWN SIGNAL";
      break;
  }
#else
  LOG(FATAL) << strsignal(signal);
#endif
}

bool isSubstring(const std::string& haystack, const std::string& needle) {
  return strstr(haystack.c_str(), needle.c_str()) != NULL;
}

void logFlags() {
  std::vector<gflags::CommandLineFlagInfo> flags;
  gflags::GetAllFlags(&flags); // flags are sorted by filename, then flagname
  const std::string substring = "facebook360_dep"; // only get flags from our project

  // Find name padding to align printed flag values
  int padding = 0;
  for (const auto& flag : flags) {
    if (isSubstring(flag.filename, substring)) {
      padding = std::max(padding, int(flag.name.size()));
    }
  }

  LOG(INFO) << "Flags:";
  for (const auto& flag : flags) {
    if (isSubstring(flag.filename, substring)) {
      LOG(INFO) << fmt::format("--{:<{}} = {}", flag.name, padding, flag.current_value);
    }
  }
}

void initDep(int& argc, char**& argv, const std::string kUsageMessage) {
  if (kUsageMessage != "") {
    gflags::SetUsageMessage(kUsageMessage);
  }

  // Initialize Google's logging library
  google::InitGoogleLogging(argv[0]);

  static const bool kRemoveFlags = true;
  gflags::ParseCommandLineNonHelpFlags(&argc, &argv, kRemoveFlags);
  FLAGS_helpshort |= FLAGS_help;
  FLAGS_help = false;
  gflags::HandleCommandLineHelpFlags();

  if (FLAGS_log_dir != "") {
    ::filesystem::create_directories(FLAGS_log_dir);
  }

  logFlags();

  // setup signal and termination handlers
  std::set_terminate(terminateHandler);

  // terminate process: interrupt program
  signal(SIGINT, sigHandler);

  // create core image: illegal instruction
  signal(SIGILL, sigHandler);

  // create core image: floating-point exception
  signal(SIGFPE, sigHandler);

  // create core image: segmentation violation
  signal(SIGSEGV, sigHandler);

  // terminate process: software termination signal
  signal(SIGTERM, sigHandler);

#ifndef WIN32
  // terminate process: terminal line hangup
  signal(SIGHUP, sigHandler);

  // create core image: quit program
  signal(SIGQUIT, sigHandler);

  // create core image: trace trap
  signal(SIGTRAP, sigHandler);

  // terminate process: kill program
  signal(SIGKILL, sigHandler);

  // create core image: bus error
  signal(SIGBUS, sigHandler);

  // create core image: non-existent system call invoked
  signal(SIGSYS, sigHandler);

  // terminate process: write on a pipe with no reader
  signal(SIGPIPE, sigHandler);
#endif
}

} // namespace system_util
} // namespace fb360_dep
