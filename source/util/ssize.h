/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

// use ssize_t and ssize() for safe, easy and potentially faster container index iteration:
//   for (ssize_t i = 0; i < ssize(container); ++i) {
//   for (ssize_t i = 0; i < ssize(container) - 1; ++i) { // doesn't work with int
//   for (ssize_t i = ssize(container); i >= 0; --i) { // doesn't work with size_t
#pragma once

namespace fb360_dep {

#ifndef _SSIZE_T
using ssize_t = ptrdiff_t;
#endif

template <typename T>
constexpr ssize_t ssize(const T& container) {
  return container.size();
}

template <class T, ssize_t N>
constexpr ssize_t ssize(const T (&array)[N]) {
  return N;
}

} // namespace fb360_dep
