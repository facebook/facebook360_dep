/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/util/CvUtil.h"

#include <fstream>
#include <string>
#include <vector>

#include <folly/Format.h>
#include <glog/logging.h>

namespace fb360_dep {
namespace cv_util {

using namespace math_util;

cv::Mat imreadExceptionOnFail(const filesystem::path& filename, const int flags) {
  CHECK_NE(filename.extension(), ".pfm")
      << folly::sformat("Cannot imread .pfm with OpenCV: ", filename.string());
  const cv::Mat image = cv::imread(filename.string(), flags);
  CHECK(!image.empty()) << folly::sformat("failed to load image: {}", filename.string());
  return image;
}

void imwriteExceptionOnFail(
    const filesystem::path& filename,
    const cv::Mat& image,
    const std::vector<int>& params) {
  CHECK(imwrite(filename.string(), image, params))
      << folly::sformat("failed to save image: {}", filename.string());
}

void writeCvMat32FC1ToPFM(const filesystem::path& path, const cv::Mat_<float>& m) {
  const int height = m.rows;
  const int width = m.cols;

  std::ofstream file(path.string(), std::ios::binary);
  file << "Pf\n";
  file << width << " " << height << "\n";
  file << "-1.0\n";
  CHECK_EQ(m.step[0], width * sizeof(float)) << "expected contiguous float Mat";
  file.write((char*)m.ptr(), width * height * sizeof(float));
}

cv::Mat_<float> readCvMat32FC1FromPFM(const filesystem::path& path) {
  std::ifstream file(path.string(), std::ios::binary);

  CHECK(file.good()) << "cannot load file: " << path;

  std::string format;
  getline(file, format);
  CHECK_EQ(format, "Pf") << folly::sformat(
      "expected 'Pf' in 1-channel .pfm file header: {}", path.string());

  int width, height;
  file >> width >> height;

  double endian;
  file >> endian;
  CHECK_LE(endian, 0.0) << folly::sformat(
      "only little endian .pfm files supported: ", path.string());
  file.ignore(); // eat newline

  cv::Mat_<float> m(cv::Size(width, height));
  file.read((char*)m.ptr(), width * height * sizeof(float));
  return m;
}

} // namespace cv_util
} // namespace fb360_dep
