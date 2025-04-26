/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fstream>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/thirdparty/bc7_compressor/ISPCTextureCompressor/ispc/ispc_texcomp/ispc_texcomp.h"
#include "source/util/CvUtil.h"
#include "source/util/FilesystemUtil.h"
#include "source/util/RawUtil.h"
#include "source/util/SystemUtil.h"

namespace fb360_dep::bc7_util {

void writeDDSHeaderField(
    char* headerData,
    const int offset, // number of uint32_t's from start of header
    const uint32_t val) { // value to write

  union byte_uint32 {
    uint8_t bytes[sizeof(uint32_t)];
    uint32_t i;
  };

  byte_uint32 u;
  u.i = val;
  for (int i = 0; i < 4; ++i) {
    headerData[offset * sizeof(uint32_t) + i] = u.bytes[i];
  }
}

uint8_t gammaCorrect(const float val, const float gammaCorrection) {
  return uint8_t(std::pow(val, gammaCorrection) * 255.0f + 0.5f);
}

void compressBC7(
    const cv::Mat& image,
    const filesystem::path& destFilename,
    const float gammaCorrection = 2.2 / 1.8,
    const bool writeDDSHeader = true) {
  const cv::Mat_<cv::Vec3f> srcImg = cv_util::convertImage<cv::Vec3f>(image);

  // Pack the image data in the format the BC7 compressor expects
  const int w = srcImg.cols;
  const int h = srcImg.rows;
  const int bytesPerPixel = 4;
  std::vector<uint8_t> uncompressedImage(w * h * bytesPerPixel);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const cv::Vec3f srcColor = srcImg(y, x);
      const int offset = (x + y * w) * bytesPerPixel;
      uncompressedImage[offset + 0] = gammaCorrect(srcColor[2], gammaCorrection);
      uncompressedImage[offset + 1] = gammaCorrect(srcColor[1], gammaCorrection);
      uncompressedImage[offset + 2] = gammaCorrect(srcColor[0], gammaCorrection);
      uncompressedImage[offset + 3] = 255;
    }
  }

  rgba_surface surface;
  surface.width = w;
  surface.height = h;
  surface.stride = surface.width * bytesPerPixel;
  surface.ptr = uncompressedImage.data();

  bc7_enc_settings settings;
  GetProfile_veryfast(&settings);

  std::vector<unsigned char> bc7data(w * h);
  CompressBlocksBC7(&surface, bc7data.data(), &settings);

  std::ofstream outFile(destFilename.string(), std::ios::binary);

  if (writeDDSHeader) {
    static const int kHeaderSize = 148;
    char headerData[kHeaderSize] = {
        68, 68, 83, 32, 124, 0, 0, 0, 7, 16, 10, 0,  0,  8, 0, 0, -112, 9, 0, 0, 0, -128, 76, 0, 1,
        0,  0,  0,  1,  0,   0, 0, 0, 0, 0,  0,  0,  0,  0, 0, 0, 0,    0, 0, 0, 0, 0,    0,  0, 0,
        0,  0,  0,  0,  0,   0, 0, 0, 0, 0,  0,  0,  0,  0, 0, 0, 0,    0, 0, 0, 0, 0,    0,  0, 0,
        0,  32, 0,  0,  0,   4, 0, 0, 0, 68, 88, 49, 48, 0, 0, 0, 0,    0, 0, 0, 0, 0,    0,  0, 0,
        0,  0,  0,  0,  0,   0, 0, 0, 0, 16, 0,  0,  0,  0, 0, 0, 0,    0, 0, 0, 0, 0,    0,  0, 0,
        0,  0,  0,  99, 0,   0, 0, 3, 0, 0,  0,  0,  0,  0, 0, 1, 0,    0, 0, 0, 0, 0,    0};

    // write width, height, data size in header
    writeDDSHeaderField(headerData, 3, h);
    writeDDSHeaderField(headerData, 4, w);
    writeDDSHeaderField(headerData, 5, bc7data.size());
    outFile.write(headerData, kHeaderSize);
  }

  outFile.write((char*)bc7data.data(), bc7data.size());
}

void compressBC7(
    const filesystem::path& srcFilename,
    const filesystem::path& destFilename,
    const float gammaCorrection = 2.2 / 1.8,
    const bool writeDDSHeader = true) {
  const cv::Mat_<cv::Vec3f> srcImg = cv_util::loadImage<cv::Vec3f>(srcFilename);
  compressBC7(srcImg, destFilename, gammaCorrection, writeDDSHeader);
}

} // namespace fb360_dep::bc7_util
