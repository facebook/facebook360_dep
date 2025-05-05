/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <algorithm>

#ifdef WIN32
#define _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING
#include <experimental/filesystem> // C++-standard header file name
#include <filesystem> // Microsoft-specific implementation header file name
#else
#define BOOST_FILESYSTEM_NO_DEPRECATED
#include <boost/filesystem.hpp>
#endif

#include <glog/logging.h>

namespace filesystem {
#ifdef WIN32
using namespace std::experimental::filesystem;
#else
using namespace boost::filesystem;
#endif

inline bool isHidden(const path& p) {
  return p.filename().string()[0] == '.';
}

inline path getFirstFile(
    const path& dir,
    const bool includeHidden = true,
    const bool exceptOnEmpty = true,
    const std::string extension = "",
    const std::string ignoreExtension = "") {
  path res = "";
  auto directoryContents = recursive_directory_iterator(dir);
  for (const directory_entry& entry : directoryContents) {
    if (is_regular_file(entry) && (includeHidden || !isHidden(entry.path()))) {
      const path p = entry.path();
      if ((extension.empty() || p.extension().string() == extension) &&
          (ignoreExtension.empty() || p.extension().string() != ignoreExtension)) {
        return p;
      }
    }
  }
  CHECK(!exceptOnEmpty) << "Could not find files in " << dir;
  return res;
}

inline std::vector<path> getFilesSorted(const path& dir, const bool includeHidden = true) {
  std::vector<path> result;
  for (const directory_entry& entry : directory_iterator(dir)) {
    if (is_regular_file(entry) && (includeHidden || !isHidden(entry.path()))) {
      result.push_back(entry.path());
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

inline std::vector<path> getVisibleFilesSorted(const path& dir) {
  return getFilesSorted(dir, false);
}

inline std::vector<std::string> getVisibleFilenamesSorted(const path& dir) {
  const std::vector<path> paths = getFilesSorted(dir, false);
  std::vector<std::string> filenames;
  for (const path& p : paths) {
    filenames.push_back(p.filename().string());
  }
  return filenames;
}

inline std::vector<path> getDirectoriesSorted(const path& dir) {
  std::vector<path> result;
  for (const directory_entry& entry : directory_iterator(dir)) {
    if (is_directory(entry)) {
      result.push_back(entry.path());
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

// Obtains file extension from first file inside given directory
inline const std::string getFirstExtension(const filesystem::path& dir) {
  const std::vector<filesystem::path> paths = getVisibleFilesSorted(dir);
  CHECK_GT(paths.size(), 0) << "no visible files in " << dir;
  return paths[0].extension().string();
}

} // namespace filesystem
