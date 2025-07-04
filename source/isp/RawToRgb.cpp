/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>

#include <boost/timer/timer.hpp>
#include <fmt/format.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/Format.h>

#include "source/isp/CameraIsp.h"
#include "source/util/FilesystemUtil.h"
#include "source/util/RawUtil.h"

using namespace fb360_dep;
using namespace fb360_dep::cv_util;
using namespace fb360_dep::system_util;

const std::string kUsageMessage = R"(
   - Converts a RAW image to RGB using a given ISP configuration.

   - Example:
     ./RawToRgb \
     --input_image_path=/path/to/video/color/000000.raw \
     --output_image_path=/path/to/video/color/000000.png \
     --isp_config_path=/path/to/video/isp.json
 )";

DEFINE_bool(apply_tone_curve, true, "Apply tone curve to image");
DEFINE_uint32(
    demosaic_filter,
    static_cast<unsigned int>(DemosaicFilter::BILINEAR),
    "Demosaic filter type: 0=Bilinear(fast), 1=Frequency, 2=Edge aware, 3=Chroma supressed bilinear");
DEFINE_string(input_image_path, "", "input image path (required)");
DEFINE_string(isp_config_path, "", "ISP config file path. Defaults to <input_image_path>/isp.json");
DEFINE_string(output_dng_path, "", "optional path to output a DNG version of the raw file.");
DEFINE_string(output_image_path, "", "output image path (required)");
DEFINE_int32(
    pow2_downscale_factor,
    1,
    "Amount to \"bin-down\" the input. Legal values are 1, 2, 4, and 8");

int main(int argc, char* argv[]) {
  initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_input_image_path, "");

  std::vector<std::string> inputFiles;
  std::vector<std::string> outputFiles;
  std::vector<std::string> dngFiles;

  if (filesystem::is_directory(FLAGS_input_image_path)) {
    for (filesystem::recursive_directory_iterator itr(FLAGS_input_image_path);
         itr != filesystem::recursive_directory_iterator();
         ++itr) {
      if (filesystem::is_regular_file(*itr) && itr->path().extension() == ".raw") {
        filesystem::path inputFile = itr->path();
        inputFiles.emplace_back(inputFile.c_str());
        outputFiles.emplace_back(inputFile.replace_extension(".png").c_str());

        if (!FLAGS_output_dng_path.empty()) {
          dngFiles.emplace_back(inputFile.replace_extension(".dng").c_str());
        }
      }
    }
  } else {
    CHECK_NE(FLAGS_output_image_path, "");
    inputFiles.emplace_back(FLAGS_input_image_path);
    outputFiles.emplace_back(FLAGS_output_image_path);
    if (!FLAGS_output_dng_path.empty()) {
      dngFiles.emplace_back(FLAGS_output_dng_path);
    }
  }

  for (ssize_t i = 0; i < ssize(inputFiles); i++) {
    boost::timer::cpu_timer timer;
    cv::Mat outputImage = rawToRgb(
        inputFiles[i],
        FLAGS_isp_config_path,
        FLAGS_pow2_downscale_factor,
        (DemosaicFilter)FLAGS_demosaic_filter,
        FLAGS_apply_tone_curve);
    LOG(INFO) << fmt::format("Runtime = {}", timer.format());
    imwriteExceptionOnFail(outputFiles[i], outputImage);

    if (!FLAGS_output_dng_path.empty()) {
      writeDng(inputFiles[i], dngFiles[i], FLAGS_isp_config_path);
    }
  }

  return 0;
}
