/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fmt/format.h>
#include <opencv2/imgproc/imgproc.hpp>

#include <folly/dynamic.h>

#include "source/util/CvUtil.h"

namespace fb360_dep::rephoto_util {

// Based on paper "Image Quality Assessment: From Error Visibility to Structural
// Similarity", by Z. Wang et al. 2004

template <typename T>
inline cv::Mat_<T> blur(const cv::Mat_<T>& in, const int blurRadius) {
  const float sigma = 1.5f; // original MSSIM implementation
  return cv_util::gaussianBlur(in, blurRadius, sigma);
}

// Assuming x, y in [0, 1]
// SSIM = L^a * C^b + S^g, where a, b, g > 0 and
// L = (2 * muX * muY + c1) / (muX^2 + muY^2 + c1)
// C = (2 * sigmaXY + c2) / (sigmaX^2 + sigmaY^2 + c2)
// S = (sigmaXY + c3) / (sigmaX * sigmaY + c3)
//
// Note that NCC = SSIM wih a = 0, b = 0, g = 1
//
// NOTE: this will not work with non floating point images
template <typename T>
inline cv::Mat_<T> computeSSIM(
    const cv::Mat_<T>& x,
    const cv::Mat_<T>& y,
    const int blurRadius,
    const float alpha = 1.0f,
    const float beta = 1.0f,
    const float gamma = 1.0f) {
  CHECK_EQ(x.size(), y.size());
  CHECK_EQ(x.channels(), 3); // RGB
  CHECK_EQ(x.channels(), y.channels());
  CHECK_GT(blurRadius, 0);
  CHECK_GE(alpha, 0.0f);
  CHECK_GE(beta, 0.0f);
  CHECK_GE(gamma, 0.0f);

  const cv::Mat_<T> muX = blur(x, blurRadius);
  const cv::Mat_<T> muY = blur(y, blurRadius);
  const cv::Mat_<T> mu2X = muX.mul(muX);
  const cv::Mat_<T> mu2Y = muY.mul(muY);
  const cv::Mat_<T> muXY = muX.mul(muY);
  const cv::Mat_<T> xMuX2 = (x - muX).mul(x - muX);
  const cv::Mat_<T> sig2X = blur(xMuX2, blurRadius);
  const cv::Mat_<T> yMuY2 = (y - muY).mul(y - muY);
  const cv::Mat_<T> sig2Y = blur(yMuY2, blurRadius);
  const cv::Mat_<T> xMuxYMuy = (x - muX).mul(y - muY);
  const cv::Mat_<T> sigXY = blur(xMuxYMuy, blurRadius);
  cv::Mat_<T> sigX;
  cv::sqrt(sig2X, sigX);
  cv::Mat_<T> sigY;
  cv::sqrt(sig2Y, sigY);

  // Default constants in the SSIM index formula: K = [0.01 0.03], L = 1
  // c1 = (k * L)^2, k = 0.01, L = 1
  // c2 = (k * L)^2, k = 0.03, L = 1
  // c3 = c2 / 2
  const cv::Scalar c1 = cv::Scalar::all(0.0001f);
  const cv::Scalar c2 = cv::Scalar::all(0.0009f);
  const cv::Scalar c3 = c2 / 2.0f;

  cv::Mat_<T> luminance = (2 * muXY + c1).mul(1.0f / (mu2X + mu2Y + c1));
  cv::pow(luminance, alpha, luminance);
  cv::Mat_<T> contrast = (2 * sigX.mul(sigY) + c2).mul(1.0f / (sig2X + sig2Y + c2));
  cv::pow(contrast, beta, contrast);
  cv::Mat_<T> structure = (sigXY + c3).mul(1.0f / (sigX.mul(sigY) + c3));
  cv::pow(structure, gamma, structure);
  return contrast.mul(luminance).mul(structure);
}

inline cv::Scalar averageScore(const cv::Mat& scoreMap, const cv::Mat& mask = cv::Mat()) {
  cv::Scalar result;
  std::vector<cv::Mat> channels(scoreMap.channels());
  cv::split(scoreMap, channels);
  for (int i = 0; i < scoreMap.channels(); ++i) {
    cv::Mat maskNan(mask.size(), mask.type());
    mask.copyTo(maskNan);
    for (int yy = 0; yy < channels[i].rows; ++yy) {
      for (int xx = 0; xx < channels[i].cols; ++xx) {
        const float v = channels[i].at<float>(yy, xx);
        if (std::isnan(v)) {
          maskNan.at<uint8_t>(yy, xx) = 0;
        }
      }
    }
    result[i] = cv::mean(channels[i], maskNan)[0];
  }
  return result;
}

template <typename T>
inline cv::Mat_<T> computeScoreMap(
    const std::string& method,
    const cv::Mat_<T>& x,
    const cv::Mat_<T>& y,
    const int blurRadius) {
  if (method == "MSSIM") {
    return computeSSIM(x, y, blurRadius, 1, 1, 1);
  } else if (method == "NCC") {
    return computeSSIM(x, y, blurRadius, 0, 0, 1);
  } else {
    CHECK(false) << "Invalid method " << method;
  }
}

inline std::string formatResults(const cv::Scalar& scoreAvg) {
  return fmt::format(
      "R {:.2f}%, G {:.2f}%, B {:.2f}%",
      100 * scoreAvg.val[2],
      100 * scoreAvg.val[1],
      100 * scoreAvg.val[0]);
}

template <typename T, typename U>
inline cv::Mat_<cv::Vec3b> stackResults(
    const std::vector<cv::Mat_<T>>& reference, // color (and depth)
    const std::vector<cv::Mat_<T>>& rendered, // color (and depth)
    const cv::Mat_<U>& ssim,
    const cv::Scalar& mssim,
    const cv::Mat& mask = cv::Mat()) {
  CHECK_GE(reference.size(), 1); // at least color
  CHECK_LE(reference.size(), 2); // at most color and depth
  CHECK_EQ(reference.size(), rendered.size());

  CHECK_EQ(ssim.channels(), 3);

  // Stack all images
  std::vector<cv::Mat_<cv::Vec3b>> images;

  // Reference and rendered images are converted to 8 bit
  for (int i = 0; i < int(reference.size()); ++i) {
    cv::Mat_<cv::Vec3b> ref = cv_util::convertImage<cv::Vec3b>(reference[i]);
    images.push_back(ref);
  }
  for (int i = 0; i < int(rendered.size()); ++i) {
    cv::Mat_<cv::Vec3b> ren = cv_util::convertImage<cv::Vec3b>(rendered[i]);
    if (!mask.empty()) {
      ren.setTo(cv::Scalar::all(0), ~mask);
    }
    images.push_back(ren);
  }

  // SSIM is converted to 8 bit and has a color map applied
  cv::Mat_<cv::Vec3b> ssim8 = cv_util::convertImage<cv::Vec3b>(ssim);

  // Convert to heatmap
  // JET makes [0, 255] -> [blue, red], but we want [0, 255] -> [red, blue]
  ssim8 = cv::Scalar::all(255) - ssim8;
  cv::applyColorMap(ssim8, ssim8, cv::COLORMAP_JET);

  // Mask pixels outside mask
  ssim8.setTo(cv::Scalar::all(0), ~mask);

  images.push_back(ssim8);
  cv::Mat_<cv::Vec3b> plot = cv_util::stackHorizontal(images);

  // Add text to image showing MSSIM values
  const std::string text = formatResults(mssim);
  const int textFont = cv::FONT_HERSHEY_PLAIN;
  const double textScale = 2;
  const cv::Scalar textColor(0, 255, 0); // green
  cv::putText(plot, text, cv::Point2f(20, 50), textFont, textScale, textColor);

  return plot;
}

} // namespace fb360_dep::rephoto_util
