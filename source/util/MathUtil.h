/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <iostream>
#include <limits>
#include <numeric>
#include <vector>

namespace fb360_dep {
namespace math_util {

static inline float randf0to1() {
  return float(rand()) / float(RAND_MAX);
}

static inline float toRadians(const float deg) {
  return deg * static_cast<float>(M_PI) / 180.0f;
}

template <typename T>
inline T square(const T x) {
  return x * x;
}

template <typename T>
inline T clamp(const T& x, const T& a, const T& b) {
  return x < a ? a : x > b ? b : x;
}

template <typename T>
inline T reflect(const T x, const T r) {
  return x < T(0) ? -x : x >= r ? 2 * r - x - 1 : x;
}

template <typename T>
inline T wrap(const T x, const T r) {
  return x < T(0) ? r + x : x >= r ? x - r : x;
}

template <typename V, typename T>
inline V lerp(const V x0, const V x1, const T alpha) {
  return x0 * (T(1) - alpha) + x1 * alpha;
}

template <typename T>
inline T lerp(const T x0, const T x1, const T alpha) {
  return x0 * (T(1) - alpha) + x1 * alpha;
}

const static double LOG_E = log(M_E);
template <typename T>
class GaussianApproximation {
 protected:
  const T xMin, xMax, yMin, yMax;
  const T xRangeRecip, yRange;
  const T sigma, scale;
  const T a0, a2, a3; // a1 == 0
  const T b0, b1, b2, b3;

 public:
  GaussianApproximation(const T xMin_, const T xMax_, const T yMin_, const T yMax_)
      : xMin(xMin_),
        xMax(xMax_),
        yMin(yMin_),
        yMax(yMax_),
        xRangeRecip(2 / (xMax_ - xMin_)),
        yRange(yMax_ - yMin_),
        sigma(sqrt(2.0) * 0.21), // 0.21 instead of 0.2 to keep b(x) > 0
        scale(1 / (2 * sigma * sigma)),
        a0(1),
        a2(-(-2 * LOG_E * scale + 12 * exp(scale / 4) - 12) / exp(scale / 4)),
        a3((-4 * LOG_E * scale + 16 * exp(scale / 4) - 16) / exp(scale / 4)),
        b0((2 * LOG_E * scale - 4) / exp(scale / 4)),
        b1(-(8 * LOG_E * scale - 24) / exp(scale / 4)),
        b2((10 * LOG_E * scale - 36) / exp(scale / 4)),
        b3(-(4 * LOG_E * scale - 16) / exp(scale / 4)) {}

  T inline operator()(const T x) const {
    const T xr = (x - xMin) * xRangeRecip - T(1);
    const T xp = xr > T(0) ? xr : -xr;
    const T yp = // Horner's rule
        (xp < T(1.0))
        ? ((xp < T(0.5)) ? a0 + xp * (xp * (a2 + xp * a3)) : b0 + xp * (b1 + xp * (b2 + xp * b3)))
        : T(0.0);
    return yp * yRange + yMin;
  }
};

template <typename T, typename V>
class BezierCurve {
 protected:
  std::vector<V> points_;

 public:
  BezierCurve() {}

  BezierCurve(std::vector<V> points) {
    for (auto p : points) {
      points_.push_back(p);
    }
  }

  void addPoint(const V p) {
    points_.push_back(p);
  }

  void clearPoints() {
    points_.clear();
  }

  inline V operator()(const int i, const int j, const T t) const {
    return (i == j) ? points_[i] : lerp((*this)(i, j - 1, t), (*this)(i + 1, j, t), t);
  }

  inline V operator()(const T t) const {
    return (*this)(0, points_.size() - 1, t);
  }
};

// Returns the (row, col)-tuple corresponding to the kth element in a (numRows x numCols) matrix
static inline std::tuple<int, int>
linearToMatrixIndex(int k, int numRows, int numCols, bool isRowMajor = true) {
  return isRowMajor ? std::make_tuple(k / numCols, k % numCols)
                    : std::make_tuple(k % numRows, k / numRows);
}

// Returns the linear index corresponding to the (row, col)-tuple rowCik in a (numRows x numCols)
// matrix
static inline int
matrixToLinearIndex(std::tuple<int, int> rowCol, int numRows, int numCols, bool isRowMajor = true) {
  return isRowMajor ? std::get<0>(rowCol) * numCols + std::get<1>(rowCol)
                    : std::get<0>(rowCol) + numRows * std::get<1>(rowCol);
}

// Descending sort function for pairs (sort by first element)
template <class T, class U>
bool sortdescPair(const std::pair<T, U>& a, const std::pair<T, U>& b) {
  return (std::get<0>(a) > std::get<0>(b));
}

} // end namespace math_util
} // end namespace fb360_dep
