/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "source/util/MathUtil.h"

namespace fb360_dep {
namespace isp {

// Boundary functor's
template <typename T>
struct WrapBoundary {
  WrapBoundary() {}
  inline T operator()(const T x, const T r) const {
    return math_util::wrap(x, r);
  }
};

template <typename T>
struct ReflectBoundary {
  ReflectBoundary() {}
  inline T operator()(const T x, const T r) const {
    return math_util::reflect(x, r);
  }
};

// Implements a two-tap IIR low pass filter
template <typename H, typename V, typename P>
void iirLowPass(
    const cv::Mat_<P>& inputImage,
    const float amount,
    cv::Mat_<P>& lpImage,
    const H& hBoundary,
    const V& vBoundary,
    const float maxVal = 255.0f) {
  const float alpha = powf(amount, 1.0f / 4.0f);
  cv::Mat_<cv::Vec3f> buffer(std::max(inputImage.rows, inputImage.cols), 1);

  // Horizontal pass
  for (int i = 0; i < lpImage.rows; ++i) {
    // Causal pass
    cv::Vec3f v(inputImage(i, lpImage.cols - 1));
    for (int j = 0; j < lpImage.cols; ++j) {
      cv::Vec3f ip(inputImage(i, j));
      v = math_util::lerp(ip, v, alpha);
      buffer(hBoundary(j - 1, lpImage.cols), 0) = v;
    }

    // Anticausal pass
    v = buffer(0, 0);
    for (int j = lpImage.cols - 1; j >= 0; --j) {
      cv::Vec3f ip(buffer(math_util::wrap(j, lpImage.cols), 0));
      v = math_util::lerp(ip, v, alpha);
      lpImage(i, hBoundary(j + 1, lpImage.cols))[0] = math_util::clamp(v[0], 0.0f, maxVal);
      lpImage(i, hBoundary(j + 1, lpImage.cols))[1] = math_util::clamp(v[1], 0.0f, maxVal);
      lpImage(i, hBoundary(j + 1, lpImage.cols))[2] = math_util::clamp(v[2], 0.0f, maxVal);
    }
  }

  // Vertical pass
  for (int j = 0; j < lpImage.cols; ++j) {
    // Causal pass
    cv::Vec3f v(lpImage(1, j));
    for (int i = 0; i < lpImage.rows; ++i) {
      cv::Vec3f ip(lpImage(i, j));
      v = math_util::lerp(ip, v, alpha);
      buffer(vBoundary(i - 1, lpImage.rows), 0) = v;
    }
    // Anticausal pass
    v = buffer(lpImage.rows - 2, 0);
    for (int i = lpImage.rows - 1; i >= -1; --i) {
      cv::Vec3f ip = buffer(math_util::reflect(i, lpImage.rows), 0);
      v = math_util::lerp(ip, v, alpha);
      lpImage(vBoundary(i + 1, lpImage.rows), j)[0] = math_util::clamp(v[0], 0.0f, maxVal);
      lpImage(vBoundary(i + 1, lpImage.rows), j)[1] = math_util::clamp(v[1], 0.0f, maxVal);
      lpImage(vBoundary(i + 1, lpImage.rows), j)[2] = math_util::clamp(v[2], 0.0f, maxVal);
    }
  }
}

template <typename T>
void sharpenWithIirLowPass(
    cv::Mat_<T>& inputImage,
    const cv::Mat_<T>& lpImage,
    const float rAmount,
    const float gAmount,
    const float bAmount,
    const float noiseCore = 100.0f,
    const float maxVal = 255.0f) {
  // Iir unsharp mask with noise coring
  for (int i = 0; i < inputImage.rows; ++i) {
    for (int j = 0; j < inputImage.cols; ++j) {
      const cv::Vec3f lp = lpImage(i, j);
      T& p = inputImage(i, j);

      // High pass signal - just the residual of the low pass
      // subtracted from the original signal.
      const cv::Vec3f hp(p[0] - lp[0], p[1] - lp[1], p[2] - lp[2]);

      // Noise coring
      const cv::Vec3f ng(
          1.0f - expf(-(math_util::square(hp[0]) * noiseCore)),
          1.0f - expf(-(math_util::square(hp[1]) * noiseCore)),
          1.0f - expf(-(math_util::square(hp[2]) * noiseCore)));

      // Unsharp mask with coring
      p[0] = math_util::clamp(lp[0] + hp[0] * ng[0] * rAmount, 0.0f, maxVal);
      p[1] = math_util::clamp(lp[1] + hp[1] * ng[1] * gAmount, 0.0f, maxVal);
      p[2] = math_util::clamp(lp[2] + hp[2] * ng[2] * bAmount, 0.0f, maxVal);
    }
  }
}

} // namespace isp
} // end namespace fb360_dep
