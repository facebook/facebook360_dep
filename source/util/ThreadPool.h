/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <thread>
#include <vector>

#include "source/util/SystemUtil.h"

namespace fb360_dep {

inline int getThreadCount() {
  return std::max<int>(1, std::thread::hardware_concurrency());
}

struct ThreadPool {
  ThreadPool(const int maxThreadsFlag) {
    maxThreads = ThreadPool::getThreadCountFromFlag(maxThreadsFlag);
  }
  ThreadPool() {
    maxThreads = ThreadPool::getThreadCountFromFlag(-1);
  }
  static int getThreadCountFromFlag(const int maxThreadsFlag) {
    return (maxThreadsFlag < 0) ? getThreadCount() : maxThreadsFlag;
  }
  int getMaxThreads() {
    return maxThreads;
  }
  template <class Fn, class... Args>
  void spawn(Fn&& fn, Args&&... args) {
    if (maxThreads == 0) {
      fn(std::forward<Args>(args)...);
    } else {
      if (int(threads.size()) == maxThreads) {
        join();
      }
      threads.emplace_back(std::forward<Fn>(fn), std::forward<Args>(args)...);
    }
  }
  void join() {
    for (std::thread& thread : threads) {
      thread.join();
    }
    threads.clear();
  }

 private:
  int maxThreads;
  std::vector<std::thread> threads;
};
} // namespace fb360_dep
