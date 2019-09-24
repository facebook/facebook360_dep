/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <assert.h>
#ifdef WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "source/thirdparty/dirent/dirent.h"
#else
#include <dirent.h>
#endif
#include <math.h>

#include <glog/logging.h>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "source/util/FilesystemUtil.h"
#include "source/util/ssize.h"

namespace fb360_dep {
namespace system_util {

// this should be the first line of most main() function in this project. sets up glog,
// gflags, and enables stack traces to be triggered when the program stops due to an
// exception
void initDep(int& argc, char**& argv, const std::string kUsageMessage="");

} // namespace system_util
} // namespace fb360_dep
