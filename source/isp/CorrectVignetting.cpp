/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/util/CvUtil.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;

const std::string kUsageMessage = R"(
   - Correct vignetting in a single image.

   - Example:
     ./CorrectVignetting \
     --out=/path/to/output/image \
     --principal_x=1680 \
     --principal_y=1080 \
     --raw=/path/to/raw/image
     --vignetting_x="1.5,1.0,1.0,1.0,1.0,1.5" \
     --vignetting_y="1.5,1.0,1.0,1.0,1.0,1.5"
 )";

DEFINE_string(out, "", "path to output image");
DEFINE_double(principal_x, -1, "principal x-coord (< 0 = width / 2)");
DEFINE_double(principal_y, -1, "principal y-coord (< 0 = height / 2)");
DEFINE_string(raw, "", "path to raw image");
DEFINE_string(vignetting_x, "", "x-axis comma-separated vignetting values");
DEFINE_string(vignetting_y, "", "y-axis comma-separated vignetting values");

float* vignettingTableX = nullptr;
float* vignettingTableY = nullptr;

template <typename T>
inline T clamp(const T& x, const T& a, const T& b) {
  return x < a ? a : x > b ? b : x;
}

template <typename V, typename T>
inline V lerp(const V x0, const V x1, const T alpha) {
  return x0 * (T(1) - alpha) + x1 * alpha;
}

template <typename T>
inline T lerp(const T x0, const T x1, const T alpha) {
  return x0 * (T(1) - alpha) + x1 * alpha;
}

namespace {

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

} // anonymous namespace

void initGflags(int& argc, char**& argv) {
  static const bool kRemoveFlags = true;
  gflags::ParseCommandLineNonHelpFlags(&argc, &argv, kRemoveFlags);
  gflags::HandleCommandLineHelpFlags();
}

std::vector<float> splitString(const std::string& csv) {
  std::vector<float> values;
  std::stringstream ss(csv);
  float val;
  while (ss >> val) {
    values.push_back(val);
    if (ss.peek() == ',') {
      ss.ignore();
    }
  }
  return values;
}

void getBezierCenterShift(int& bezierShiftX, int& bezierShiftY, const int width, const int height) {
  int principalX = FLAGS_principal_x;
  int principalY = FLAGS_principal_y;
  if (principalX < 0) {
    principalX = width / 2;
  }
  if (principalY < 0) {
    principalY = height / 2;
  }

  CHECK_LT(principalX, width) << "principal_x out of bounds";
  CHECK_LT(principalY, height) << "principal_y out of bounds";

  bezierShiftX = principalX - width / 2;
  bezierShiftY = principalY - height / 2;
}

void buildVignettingTables(const int width, const int height) {
  LOG(INFO) << "Pre-computing vignetting tables...";

  // NOTE: These tables only need to be computed once, and can then be applied
  // to all input images
  vignettingTableX = new float[width];
  vignettingTableY = new float[height];

  // Build Bezier X and Y curves
  // NOTE: The more anchor points the Bezier curve has the slower it will be,
  // since it has to interpolate between anchor points
  BezierCurve<float, float> vignetteCurveX(splitString(FLAGS_vignetting_x));
  BezierCurve<float, float> vignetteCurveY(splitString(FLAGS_vignetting_y));

  // Bezier template is circular and centered at the center of the image, so we
  // need to shift smallest dimension by (max dimension - min dimension) / 2
  int dX = 0;
  int dY = 0;
  int maxDimension;
  if (width > height) {
    dY = (width - height) / 2;
    maxDimension = width;
  } else {
    dX = (height - width) / 2;
    maxDimension = height;
  }

  for (int y = 0; y < height; ++y) {
    vignettingTableY[y] = vignetteCurveY((y + dY) / float(maxDimension));
    for (int x = 0; x < width; ++x) {
      vignettingTableX[x] = vignetteCurveX((x + dX) / float(maxDimension));
    }
  }
}

// Load raw image
// NOTE: loading grayscale to simulate one of the color channels
// NOTE: the exact same process would be applied to all the channels (same
// vignetting curve)
cv::Mat_<float> loadImage(const std::string& fn) {
  cv::Mat raw = cv::imread(fn, cv::IMREAD_GRAYSCALE | cv::IMREAD_ANYDEPTH);
  CHECK(!raw.empty()) << "Failed to load image";

  // Convert 16-bit to 32-bit floating point in range [0..1]
  cv::Mat_<float> output;
  raw.convertTo(output, CV_32F, 1.0f / 65535.0f);
  return output;
}

int main(int argc, char** argv) {
  system_util::initDep(argc, argv, kUsageMessage);

  cv::Mat_<float> image = loadImage(FLAGS_raw);
  const int width = image.cols;
  const int height = image.rows;

  // Pre-compute vignetting tables
  buildVignettingTables(width, height);

  // Center of Bezier is shifted by the distance of the principal to the image
  // center
  int bezierShiftX;
  int bezierShiftY;
  getBezierCenterShift(bezierShiftX, bezierShiftY, width, height);

  // Apply vignetting correction
  LOG(INFO) << "Applying vignetting correction...";
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int xx = clamp(x + bezierShiftX, 0, width - 1);
      const int yy = clamp(y + bezierShiftY, 0, height - 1);
      image(y, x) *= vignettingTableX[xx] * vignettingTableY[yy];
    }
  }

  // Save corrected image
  cv::imwrite(FLAGS_out, 255.0f * image);

  return EXIT_SUCCESS;
}
