/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/depth_estimation/TemporalBilateralFilter.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/depth_estimation/Derp.h"
#include "source/util/CvUtil.h"
#include "source/util/FilesystemUtil.h"
#include "source/util/ImageUtil.h"

using namespace fb360_dep;
using namespace fb360_dep::cv_util;
using namespace fb360_dep::depth_estimation;
using namespace fb360_dep::image_util;

static const int kTemporalSpaceRadiusMin = 1; // at coarsest level of the pyramid
static const int kTemporalSpaceRadiusMax = 1; // at finest level of the pyramid

const std::string kUsageMessage = R"(
  - Runs temporal filter across disparity frames using corresponding color frames as guides.

  - Example:
    ./TemporalBilateralFilter \
    --input_root=/path/to/ \
    --output_root=/path/to/output \
    --rig=/path/to/rigs/rig.json \
    --first=000000 \
    --last=000000
)";

DEFINE_string(color, "", "color directory");
DEFINE_string(cameras, "", "destination cameras");
DEFINE_string(disparity, "", "disparity directory");
DEFINE_string(first, "000000", "first frame to process (lexical)");
DEFINE_string(foreground_masks, "", "foreground masks directory");
DEFINE_string(input_root, "", "output root directory (required)");
DEFINE_string(last, "000000", "last frame to process (lexical)");
DEFINE_int32(level, 0, "pyramid level being processed");
DEFINE_string(output_formats, "", "saved formats, comma separated (exr, png, pfm supported)");
DEFINE_string(output_root, "", "output root directory (required)");
DEFINE_int32(resolution, 2048, "8192, 4096, 2048, 1024, 512, 256");
DEFINE_string(rig, "", "path to camera rig .json (required)");
DEFINE_double(sigma, 0.01, "spatio-temporal smoothing");
DEFINE_int32(space_radius, -1, "space filtering radius");
DEFINE_int32(threads, -1, "number of threads (-1 = auto, 0 = none)");
DEFINE_int32(time_radius, 2, "temporal filtering radius");
DEFINE_bool(use_foreground_masks, false, "use pre-computed foreground masks");
DEFINE_double(weight_b, 0.5, "Blue channel weight");
DEFINE_double(weight_g, 1.0, "Green channel weight");
DEFINE_double(weight_r, 1.0, "Red channel weight");

void saveDisparity(
    const std::string& outputFormatsIn,
    const cv::Mat_<float>& disparity,
    const std::string& dstId,
    const int frameIdx) {
  std::string outputFormatsStr = "pfm," + outputFormatsIn; // always save PFM
  std::vector<std::string> outputFormatsVec;
  folly::split(",", outputFormatsStr, outputFormatsVec);
  std::unordered_set<std::string> outputFormats(outputFormatsVec.begin(), outputFormatsVec.end());
  for (const std::string& outputFormat : outputFormats) {
    if (outputFormat != "exr" && outputFormat != "pfm" && outputFormat != "png") {
      continue;
    }

    const std::string frameName = image_util::intToStringZeroPad(frameIdx, 6);
    const filesystem::path fn = depth_estimation::genFilename(
        FLAGS_output_root,
        ImageType::disparity_time_filtered_levels,
        FLAGS_level,
        dstId,
        frameName,
        outputFormat);

    if (!boost::filesystem::exists(fn.parent_path())) {
      boost::filesystem::create_directories(fn.parent_path());
    }
    if (outputFormat != "pfm") {
      const cv::Mat scaledDisparity = cv_util::convertTo<uint16_t>(disparity);
      cv_util::imwriteExceptionOnFail(fn, scaledDisparity);
    } else {
      cv_util::writeCvMat32FC1ToPFM(fn, disparity);
    }
  }
}

void populateMinMaxFrame(
    const std::string& dir,
    const int level,
    const Camera& camRef,
    const int curFrameIdx,
    int& firstFrameIdx,
    int& lastFrameIdx) {
  const std::string levelDir =
      folly::sformat("{}/level_{}/{}", dir, std::to_string(level), camRef.id);
  const std::string ext = filesystem::getFirstExtension(levelDir);

  int localFirstFrameIdx = INT_MAX;
  int localLastFrameIdx = 0;
  for (int frameIdx = curFrameIdx - FLAGS_time_radius; frameIdx <= curFrameIdx + FLAGS_time_radius;
       ++frameIdx) {
    const std::string frameName = image_util::intToStringZeroPad(frameIdx, 6);
    if (boost::filesystem::exists(filesystem::path(levelDir) / (frameName + ext))) {
      localFirstFrameIdx = std::min(frameIdx, localFirstFrameIdx);
      localLastFrameIdx = std::max(frameIdx, localLastFrameIdx);
    }
  }

  firstFrameIdx = std::max(localFirstFrameIdx, firstFrameIdx);
  lastFrameIdx = std::min(localLastFrameIdx, lastFrameIdx);
}

void filterFrame(const int curFrameIdx, const Camera::Rig& rigDst) {
  const size_t numDsts = rigDst.size();

  std::vector<std::vector<cv::Mat_<depth_estimation::PixelType>>> colorFrames(numDsts);
  std::vector<std::vector<cv::Mat_<float>>> disparities(numDsts);
  std::vector<std::vector<cv::Mat_<bool>>> masks(numDsts);

  const Camera& camRef = rigDst[0];
  int firstFrameIdx = 0;
  int lastFrameIdx = INT_MAX;
  populateMinMaxFrame(FLAGS_color, FLAGS_level, camRef, curFrameIdx, firstFrameIdx, lastFrameIdx);
  populateMinMaxFrame(
      FLAGS_disparity, FLAGS_level, camRef, curFrameIdx, firstFrameIdx, lastFrameIdx);
  if (FLAGS_use_foreground_masks) {
    populateMinMaxFrame(
        FLAGS_foreground_masks, FLAGS_level, camRef, curFrameIdx, firstFrameIdx, lastFrameIdx);
  }

  for (int frameIdx = firstFrameIdx; frameIdx <= lastFrameIdx; ++frameIdx) {
    const std::string frameName = image_util::intToStringZeroPad(frameIdx, 6);
    const std::vector<cv::Mat_<PixelType>> colorImages =
        loadLevelImages<PixelType>(FLAGS_color, FLAGS_level, rigDst, frameName, FLAGS_threads);
    const std::vector<cv::Mat_<float>> disparitiesImages =
        loadLevelImages<float>(FLAGS_disparity, FLAGS_level, rigDst, frameName, FLAGS_threads);

    std::map<int, cv::Size> sizes;
    getPyramidLevelSizes(sizes, FLAGS_color);
    const std::vector<cv::Mat_<bool>> foregroundMaskImages = FLAGS_use_foreground_masks
        ? loadLevelImages<bool>(
              FLAGS_foreground_masks, FLAGS_level, rigDst, frameName, FLAGS_threads)
        : cv_util::generateAllPassMasks(sizes.at(FLAGS_level), numDsts);
    const std::vector<cv::Mat_<bool>> fovMaskImages =
        generateFovMasks(rigDst, sizes.at(FLAGS_level), FLAGS_threads);

    for (size_t camIdx = 0; camIdx < numDsts; ++camIdx) {
      colorFrames[camIdx].push_back(colorImages[camIdx]);
      disparities[camIdx].push_back(disparitiesImages[camIdx]);
      masks[camIdx].push_back(foregroundMaskImages[camIdx] & fovMaskImages[camIdx]);
    }
  }

  LOG(INFO) << "Filtering images...";
  for (size_t camIdx = 0; camIdx < numDsts; ++camIdx) {
    cv::Mat_<float> disparity;
    const float scale = std::pow(depth_estimation::kLevelScale, FLAGS_level);
    const int spaceRadius = FLAGS_space_radius == -1
        ? std::max(std::ceil(kTemporalSpaceRadiusMax * scale), float(kTemporalSpaceRadiusMin))
        : FLAGS_space_radius;
    temporalJointBilateralFilter(
        colorFrames[camIdx],
        disparities[camIdx],
        masks[camIdx],
        curFrameIdx - firstFrameIdx,
        FLAGS_sigma,
        spaceRadius,
        FLAGS_weight_b,
        FLAGS_weight_g,
        FLAGS_weight_b,
        disparity,
        FLAGS_threads);

    saveDisparity(FLAGS_output_formats, disparity, rigDst[camIdx].id, curFrameIdx);
  }
}

int main(int argc, char** argv) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_rig, "");
  CHECK_NE(FLAGS_input_root, "");
  CHECK_NE(FLAGS_output_root, "");

  if (FLAGS_color.empty()) {
    FLAGS_color = getImageDir(FLAGS_input_root, ImageType::color_levels).string();
  }
  if (FLAGS_foreground_masks.empty()) {
    FLAGS_foreground_masks =
        getImageDir(FLAGS_input_root, ImageType::foreground_masks_levels).string();
  }
  if (FLAGS_disparity.empty()) {
    FLAGS_disparity = getImageDir(FLAGS_output_root, ImageType::disparity_levels).string();
  }

  Camera::Rig rigSrc = Camera::loadRig(FLAGS_rig);
  Camera::Rig rigDst = image_util::filterDestinations(rigSrc, FLAGS_cameras);

  // Necessary for generating FOV masks
  Camera::normalizeRig(rigDst);
  for (int frameIdx = std::stoi(FLAGS_first); frameIdx <= std::stoi(FLAGS_last); ++frameIdx) {
    filterFrame(frameIdx, rigDst);
  }
}
