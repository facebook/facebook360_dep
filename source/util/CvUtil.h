/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <folly/Format.h>

#include "source/util/FilesystemUtil.h"
#include "source/util/MathUtil.h"
#include "source/util/RawUtil.h"
#include "source/util/SystemUtil.h"
#include "source/util/ThreadPool.h"

namespace fb360_dep {
namespace cv_util {

// wrapper for cv::imread which throws an exception if loading fails
cv::Mat imreadExceptionOnFail(const filesystem::path& filename, const int flags = cv::IMREAD_COLOR);

// wrapper for cv::imwrite which throws an exception if writing fails
void imwriteExceptionOnFail(
    const filesystem::path& filename,
    const cv::Mat& image,
    const std::vector<int>& params = {});

// given a vector of images, stack them horizontally or vertically to form a larger image
template <typename T>
cv::Mat_<T> stackHorizontal(const std::vector<cv::Mat_<T>>& images) {
  assert(!images.empty());
  if (images.size() == 1) {
    return images[0];
  }
  cv::Mat_<T> stacked = images[0].clone();
  for (int i = 1; i < int(images.size()); ++i) {
    cv::hconcat(stacked, images[i], stacked);
  }
  return stacked;
}

template <typename T>
cv::Mat_<T> stackVertical(const std::vector<cv::Mat_<T>>& images) {
  assert(!images.empty());
  if (images.size() == 1) {
    return images[0];
  }
  cv::Mat_<T> stacked = images[0].clone();
  for (int i = 1; i < int(images.size()); ++i) {
    vconcat(stacked, images[i], stacked);
  }
  return stacked;
}

// returns the first 3 elements of a 4-d vector
static inline cv::Vec3f head3(const cv::Vec4f& v) {
  return cv::Vec3f(v[0], v[1], v[2]);
}

// for 1-channel float mats (e.g., depth maps), we use PFM format.
// notes on the PFM format: http://www.pauldebevec.com/Research/HDR/PFM/
void writeCvMat32FC1ToPFM(const filesystem::path& path, const cv::Mat_<float>& m);

cv::Mat_<float> readCvMat32FC1FromPFM(const filesystem::path& path);

template <typename T>
const T& clampToEdge(const cv::Mat_<T>& src, const int x, const int y) {
  return src(math_util::clamp(y, 0, src.rows - 1), math_util::clamp(x, 0, src.cols - 1));
}

template <typename T>
T bilerp(const T& p00, const T& p01, const T& p10, const T& p11, float xw, float yw) {
  return (1 - xw) * (1 - yw) * p00 + xw * (1 - yw) * p01 + (1 - xw) * yw * p10 + xw * yw * p11;
}

// partial specialization for opencv's Vec<T, N> type as all the rounding and clamping cause
// severe performance and precision problems
template <typename T, int N>
cv::Vec<float, N> bilerp(
    const cv::Vec<T, N>& p00,
    const cv::Vec<T, N>& p01,
    const cv::Vec<T, N>& p10,
    const cv::Vec<T, N>& p11,
    float xw,
    float yw) {
  cv::Vec<float, N> result;
  for (int i = 0; i < N; ++i) {
    result[i] = bilerp(p00[i], p01[i], p10[i], p11[i], xw, yw);
  }
  return result;
}

// perform bilinear interpolation with clamp-to-edge semantics
// NOTE: uses normal coordinate convention, i.e. integer pixel corners. NOT OPENCV's
template <typename T, typename ResultType = T>
ResultType getPixelBilinear(const cv::Mat_<T>& src, const float x, const float y) {
  const float xf = round(x);
  const float yf = round(y);
  const int xi = xf;
  const int yi = yf;
  return bilerp(
      clampToEdge(src, xi - 1, yi - 1),
      clampToEdge(src, xi, yi - 1),
      clampToEdge(src, xi - 1, yi),
      clampToEdge(src, xi, yi),
      x - xf + 0.5f,
      y - yf + 0.5f);
}

inline cv::Mat removeAlpha(const cv::Mat& src) {
  if (src.channels() < 4) {
    return src.clone();
  }
  cv::Mat dst;
  cvtColor(src, dst, cv::COLOR_BGRA2BGR);
  return dst;
}

inline cv::Mat extractAlpha(const cv::Mat& src) {
  CHECK_EQ(src.channels(), 4) << "no alpha channel!";
  cv::Mat alpha;
  cv::extractChannel(src, alpha, 3);
  return alpha;
}

template <typename T>
inline cv::Mat_<T>
resizeImage(const cv::Mat_<T>& image, const cv::Size& size, const int type = cv::INTER_AREA) {
  if (image.empty() || image.size() == size) {
    return image;
  }
  cv::Mat_<T> imageResized;
  cv::resize(image, imageResized, size, 0, 0, type);
  return imageResized;
}

template <typename T>
inline cv::Mat_<T>
scaleImage(const cv::Mat_<T>& image, const double scaleFactor, const int type = cv::INTER_AREA) {
  const cv::Size size(std::round(image.cols * scaleFactor), std::round(image.rows * scaleFactor));
  return resizeImage(image, size, type);
}

template <typename T>
inline std::vector<cv::Mat_<T>> resizeImages(
    const std::vector<cv::Mat_<T>>& imagesIn,
    const cv::Size& size,
    const int type = cv::INTER_AREA,
    const int numThreads = -1) {
  std::vector<cv::Mat_<T>> imagesOut(imagesIn.size());
  ThreadPool threadPool(numThreads);
  for (int i = 0; i < int(imagesIn.size()); ++i) {
    threadPool.spawn([&, i] { imagesOut[i] = resizeImage<T>(imagesIn[i], size, type); });
  }
  threadPool.join();
  return imagesOut;
}

inline float maxPixelValueFromCvDepth(int cvDepth) {
  if (cvDepth == CV_8U) {
    return 255.0f;
  } else if (cvDepth == CV_16U) {
    return 65535.0f;
  } else if (cvDepth == CV_32F) {
    return 1.0f;
  } else {
    LOG(FATAL) << folly::sformat("Depth not supported: {}", cvDepth);
    return 0.0f;
  }
}

template <typename _Tp, int cn>
inline static cv::Vec<_Tp, cn> absdiff(const cv::Vec<_Tp, cn>& a, const cv::Vec<_Tp, cn>& b) {
  cv::Vec<_Tp, cn> result;
  for (int i = 0; i < cn; ++i) {
    result[i] = abs(a[i] - b[i]);
  }
}

inline float maxPixelValue(const cv::Mat& mat) {
  return maxPixelValueFromCvDepth(mat.depth());
}

inline cv::Mat convertTo(const cv::Mat& srcImage, const int cvDepth) {
  cv::Mat dstImage;
  if (srcImage.depth() == cvDepth) {
    srcImage.copyTo(dstImage);
  } else {
    srcImage.convertTo(
        dstImage, cvDepth, maxPixelValueFromCvDepth(cvDepth) / maxPixelValue(srcImage));
  }
  CHECK_EQ(dstImage.depth(), cvDepth) << " was expecting depth: " << cvDepth
                                      << ", dstImage.depth() is actually: " << dstImage.depth();
  return dstImage;
}

template <typename T>
inline cv::Mat convertTo(const cv::Mat& srcImage);

template <>
inline cv::Mat convertTo<uint8_t>(const cv::Mat& srcImage) {
  return convertTo(srcImage, CV_8U);
}

template <>
inline cv::Mat convertTo<uint16_t>(const cv::Mat& srcImage) {
  return convertTo(srcImage, CV_16U);
}

template <>
inline cv::Mat convertTo<float>(const cv::Mat& srcImage) {
  return convertTo(srcImage, CV_32F);
}

template <typename T>
inline cv::Mat_<T> convertImage(const cv::Mat& imageIn) {
  // Create a dummy mat that will hold type, depth and channels for a given T
  cv::Mat_<T> infoMat;

  // Convert to desired depth
  cv::Mat imageOut = convertTo(imageIn, infoMat.depth());

  // Special case: OpenCV doesn't have a bool type, so Mat_<bool> is CV_8U [0..255]
  // We need to force values to be in [0..1]
  if (std::is_same<T, bool>::value) {
    cv::threshold(imageOut, imageOut, 127, 1, cv::THRESH_BINARY);
  }

  // Convert to desired number of channels
  const int chI = imageIn.channels();
  const int chO = infoMat.channels();
  if (chI == chO) {
    return imageOut;
  }

  if (chI == 1 && chO == 3) {
    cv::cvtColor(imageOut, imageOut, cv::COLOR_GRAY2BGR);
  } else if (chI == 1 && chO == 4) {
    cv::cvtColor(imageOut, imageOut, cv::COLOR_GRAY2BGRA);
  } else if (chI == 3 && chO == 1) {
    cv::cvtColor(imageOut, imageOut, cv::COLOR_BGR2GRAY);
  } else if (chI == 3 && chO == 4) {
    cv::cvtColor(imageOut, imageOut, cv::COLOR_BGR2BGRA);
  } else if (chI == 4 && chO == 1) {
    cv::cvtColor(imageOut, imageOut, cv::COLOR_BGRA2GRAY);
  } else if (chI == 4 && chO == 3) {
    cv::cvtColor(imageOut, imageOut, cv::COLOR_BGRA2BGR);
  } else {
    CHECK(false) << "Conversion from " << chI << " channels to " << chO
                 << " channels not supported";
  }

  return imageOut;
}

inline cv::Mat loadImageUnchanged(const filesystem::path& filename) {
  cv::Mat image;
  if (filename.extension() == ".raw") {
    image = rawToRgb(filename);
  } else if (filename.extension() == ".pfm") {
    image = cv_util::readCvMat32FC1FromPFM(filename);
  } else {
    image = imreadExceptionOnFail(filename, cv::IMREAD_UNCHANGED);
  }
  return image;
}

template <typename T>
inline cv::Mat_<T> loadImage(const filesystem::path& filename) {
  cv::Mat image = loadImageUnchanged(filename);
  return convertImage<T>(image);
}

template <typename T>
inline cv::Mat_<T> loadScaledImage(
    const filesystem::path& filename,
    const double scaleFactor,
    const int type = cv::INTER_LINEAR) {
  return scaleImage(loadImage<T>(filename), scaleFactor, type);
}

template <typename T>
inline cv::Mat_<T> loadResizedImage(
    const filesystem::path& filename,
    const cv::Size& size,
    const int type = cv::INTER_LINEAR) {
  return resizeImage(loadImage<T>(filename), size, type);
}

template <typename T>
inline cv::Mat_<T>
gaussianBlur(const cv::Mat_<T>& mat, const int blurRadius, const float sigma = 0.0f) {
  if (blurRadius < 1) {
    return mat;
  }
  cv::Mat_<T> matBlur;
  const int w = 2 * blurRadius + 1;
  cv::GaussianBlur(mat, matBlur, cv::Size(w, w), sigma, 0);
  return matBlur;
}

template <typename T>
inline cv::Mat_<T> blur(const cv::Mat_<T>& mat, const int blurRadius = 1) {
  if (blurRadius < 1) {
    return mat;
  }
  cv::Mat_<T> matBlur;
  const int w = 2 * blurRadius + 1;
  cv::blur(mat, matBlur, cv::Size(w, w));
  return matBlur;
}

inline cv::Mat_<bool> dilate(const cv::Mat_<bool>& mat, const int dilateRadius = 1) {
  if (dilateRadius < 1) {
    return mat;
  }
  cv::Mat_<bool> matDilated;
  const int w = 2 * dilateRadius + 1;
  const cv::Mat element = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(w, w));
  cv::dilate(mat, matDilated, element);
  return matDilated;
}

inline cv::Mat_<float> maskedMedianBlur(
    const cv::Mat_<float>& mat,
    const cv::Mat_<float>& background,
    const cv::Mat_<bool>& mask,
    const int radius,
    const bool ignoreNan = true) {
  cv::Mat_<float> blurred(mat.size(), 0.0);
  for (int y = 0; y < mat.rows; ++y) {
    for (int x = 0; x < mat.cols; ++x) {
      std::vector<float> values;
      if (!mask(y, x)) {
        if (!background.empty()) {
          blurred(y, x) = background(y, x);
        }
        continue;
      }
      for (int yy = y - radius; yy <= y + radius; ++yy) {
        for (int xx = x - radius; xx <= x + radius; ++xx) {
          // Ignore out of bounds
          if (0 > yy || yy >= mat.rows || 0 > xx || xx >= mat.cols) {
            continue;
          }

          // Ignore outside mask
          if (!mask(yy, xx)) {
            continue;
          }

          // Ignore NAN values if specified to do so
          if (ignoreNan && (std::isnan(mat(yy, xx)) || mat(yy, xx) == 0)) {
            continue;
          }

          values.push_back(mat(yy, xx));
        }
      }

      if (values.size() != 0) {
        const size_t n = values.size() / 2;
        std::partial_sort(values.begin(), values.begin() + n + 1, values.end());
        if (values.size() % 2 == 1) {
          blurred(y, x) = values[n];
        } else {
          blurred(y, x) = (values[n - 1] + values[n]) / 2.0;
        }
      }
    }
  }
  return blurred;
}

// Convert a BGR(A) cv::Mat to a RGBA vector<uint8_t>
inline std::vector<uint8_t> getRGBA8Vector(const cv::Mat& src) {
  const int numChannels = src.channels();
  CHECK(numChannels == 3 || numChannels == 4) << "unexpected " << numChannels;
  const bool hasAlpha = numChannels == 4;
  cv::Mat imageMat = convertTo<uint8_t>(src.clone());

  // Add an alpha-channel if necessary (the textures expect one), and
  // re-order BGRA->RGBA
  cvtColor(imageMat, imageMat, hasAlpha ? cv::COLOR_BGRA2RGBA : cv::COLOR_BGR2RGBA);
  static const int kChannels = 4;
  const int imageDataNumBytes = imageMat.rows * imageMat.cols * kChannels;
  std::vector<uint8_t> imageBytes(imageDataNumBytes);
  for (int i = 0; i < imageDataNumBytes; ++i) {
    imageBytes[i] = imageMat.ptr<uint8_t>()[i];
  }
  return imageBytes;
}

template <typename T>
inline T createBGR(const float b, const float g, const float r);

template <>
inline cv::Vec3f createBGR(const float b, const float g, const float r) {
  return cv::Vec3f(b, g, r) * maxPixelValueFromCvDepth(CV_32F);
}

// Values < 1 will be truncated to 0 when creating unsigned short. Need to scale it beforehand
template <>
inline cv::Vec3w createBGR(const float b, const float g, const float r) {
  const float maxVal = maxPixelValueFromCvDepth(CV_16U);
  return cv::Vec3w(b * maxVal, g * maxVal, r * maxVal);
}

template <typename T>
inline T createBGRA(const float b, const float g, const float r, const float a);

template <>
inline cv::Vec4f createBGRA(const float b, const float g, const float r, const float a) {
  return cv::Vec4f(b, g, r, a) * maxPixelValueFromCvDepth(CV_32F);
}

// Values < 1 will be truncated to 0 when creating unsigned short. Need to scale it beforehand
template <>
inline cv::Vec4w createBGRA(const float b, const float g, const float r, const float a) {
  const float maxVal = maxPixelValueFromCvDepth(CV_16U);
  return cv::Vec4w(b * maxVal, g * maxVal, r * maxVal, a * maxVal);
}

inline std::vector<cv::Mat_<bool>> generateAllPassMasks(const cv::Size& size, const int numMasks) {
  const cv::Mat_<bool> allPass(size, true);
  const std::vector<cv::Mat_<bool>> masks(numMasks, allPass);
  return masks;
}

} // namespace cv_util
} // namespace fb360_dep
