/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <ostream>
#include <vector>

#include "source/util/MathUtil.h"

namespace fb360_dep {
namespace math_util {

template <typename T>
class MonotonicTable {
 protected:
  const int size;
  const std::shared_ptr<std::vector<T>> table;
  const T minX, maxX;
  const T rangeScale;

  virtual ~MonotonicTable() = default;

  // Function that is used to compute the table
  virtual T f(const T x) const = 0;

 public:
  MonotonicTable(const T minX_, const T maxX_, const int size_)
      : size(size_),
        table(new std::vector<T>),
        minX(minX_),
        maxX(maxX_),
        rangeScale(T(size - 1) / (maxX - minX)) {}

  void initTable() {
    const T dx = T(1) / rangeScale;
    for (int i = 0; i < size; ++i) {
      table->push_back(f(dx * i + minX));
    }
  }

  inline int getSize() const {
    return size;
  }

  T inline operator()(const T x) const {
    const int i = clamp(int((x - minX) * rangeScale), 0, size - 1);
    return (*table)[i];
  }
};

class Butterworth : public MonotonicTable<float> {
 private:
  const int order_;
  const float cutoffFreq_;

 protected:
  float f(const float x) const {
    return 1.0f / (1.0f + powf(x / cutoffFreq_, 2.0f * order_));
  }

 public:
  Butterworth(
      const float minX,
      const float maxX,
      const int size,
      const float cutoffFreq,
      const int order)
      : MonotonicTable<float>(minX, maxX, size),
        order_(order),
        cutoffFreq_(cutoffFreq > 0.0f ? cutoffFreq : 1.0e-6) {
    initTable();
  }
};

} // end namespace math_util
} // end namespace fb360_dep
