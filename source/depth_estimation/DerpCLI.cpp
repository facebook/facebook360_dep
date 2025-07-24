/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <random>

#include <boost/timer/timer.hpp>
#include <fmt/format.h>
#include <glog/logging.h>

#include <folly/Format.h>
#include <folly/String.h>

#include "source/depth_estimation/Derp.h"
#include "source/depth_estimation/UpsampleDisparityLib.h"

using namespace fb360_dep;
using namespace fb360_dep::cv_util;
using namespace fb360_dep::depth_estimation;
using namespace fb360_dep::image_util;

const std::string kUsageMessage = R"(
 - Runs depth estimation on a set of frames. We assume the inputs have already been resized into
 the appropriate pyramid level widths before execution. See scripts/render/config.py to see
 the assumed widths.

 - Example:
   ./DerpCLI \
   --input_root=/path/to/ \
   --output_root=/path/to/output \
   --rig=/path/to/rigs/rig.json \
   --first=000000 \
   --last=000000
 )";

DEFINE_string(background_disp, "", "path to background disparities");
DEFINE_string(background_frame, "000000", "background frame (lexical)");
DEFINE_string(cameras, "", "comma-separated destinations to render (empty for all)");
DEFINE_string(color, "", "path to input color images");
DEFINE_bool(do_bilateral_filter, true, "apply bilateral filter at each level");
DEFINE_bool(do_median_filter, true, "apply median filter to disparity at each level");
DEFINE_string(first, "000000", "first frame to process (lexical)");
DEFINE_string(foreground_masks, "", "path to foreground masks");
DEFINE_string(input_root, "", "path to input data (required)");
DEFINE_string(last, "000000", "last frame to process (lexical)");
DEFINE_int32(level_end, -1, "level to end at (-1 = finest)");
DEFINE_int32(level_start, -1, "level to start at (-1 = coarsest)");
DEFINE_double(max_depth_m, 1e4, "max depth (m)");
DEFINE_double(min_depth_m, .50, "min depth (m)");
DEFINE_int32(mismatches_start_level, -1, "(-1 = no mismatch handling)");
DEFINE_int32(num_levels, -1, "number of levels in the pyramid (-1 = uses highest level)");
DEFINE_string(output_formats, "", "saved formats, comma separated (exr, png, pfm supported)");
DEFINE_string(output_root, "", "path to output directory (required)");
DEFINE_bool(partial_coverage, false, "set to true if no 360 coverage");
DEFINE_int32(ping_pong_iterations, 1, "number of spatial propagation iterations");
DEFINE_int32(random_proposals, 2, "number of proposed random disparities before propagation");
DEFINE_int32(resolution, 2048, "Output resolution (width in pixels)");
DEFINE_string(rig, "", "path to camera rig .json");
DEFINE_bool(save_debug_images, false, "if true, save debugging output images");
DEFINE_int32(threads, -1, "number of threads (-1 = auto, 0 = none)");
DEFINE_bool(use_foreground_masks, false, "use pre-computed foreground masks");
DEFINE_double(var_high_thresh, 1e-3, "ignore variances higher than this threshold");
DEFINE_double(var_noise_floor, 4e-5, "noise variance floor on original, full-size images");

void verifyInputs() {
  CHECK_NE(FLAGS_input_root, "");
  CHECK_NE(FLAGS_output_root, "");
  if (FLAGS_level_start >= 0 && FLAGS_level_end >= 0) {
    CHECK_GE(FLAGS_level_start, FLAGS_level_end);
  }

  if (FLAGS_rig.empty()) {
    FLAGS_rig = FLAGS_input_root + "/rigs/rig_calibrated.json";
  }
  if (FLAGS_color.empty()) {
    FLAGS_color = getImageDir(FLAGS_input_root, ImageType::color_levels).string();
  }
  if (FLAGS_background_disp.empty()) {
    FLAGS_background_disp =
        getImageDir(FLAGS_input_root, ImageType::background_disp_levels).string();
  }
  if (FLAGS_foreground_masks.empty()) {
    FLAGS_foreground_masks =
        getImageDir(FLAGS_input_root, ImageType::foreground_masks_levels).string();
  }

  // Check flag values
  CHECK_GE(FLAGS_random_proposals, 0);
  CHECK_LE(FLAGS_first, FLAGS_last);

  const bool hasColorImages = filesystem::is_directory(FLAGS_color);
  CHECK(hasColorImages) << "No images in " << FLAGS_color;

  if (FLAGS_use_foreground_masks) {
    const bool hasBackgroundDisps = filesystem::is_directory(FLAGS_background_disp);
    CHECK(hasBackgroundDisps) << "Asked to use background but no background disparities found in "
                              << FLAGS_background_disp;

    const bool hasDstForegroundMasks = filesystem::is_directory(FLAGS_foreground_masks);
    CHECK(hasDstForegroundMasks)
        << "Asked to use foreground masks but no foreground masks found in "
        << FLAGS_foreground_masks;
  }

  std::vector<std::string> outputFormats;
  folly::split(',', FLAGS_output_formats, outputFormats);
  for (std::string& outputFormat : outputFormats) {
    // We allow size 0 inputs to ensure stray commas are ignored, i.e. exr,,png is fine
    CHECK(
        outputFormat.size() == 0 || outputFormat == "exr" || outputFormat == "png" ||
        outputFormat == "pfm")
        << "Invalid output format specified: " << outputFormat;
  }
}

filesystem::path getLevelDisparityDir(const int level) {
  return getImageDir(FLAGS_output_root, ImageType::disparity_levels, level);
}

filesystem::path getLevelColorDir(const int level) {
  return fmt::format("{}/level_{}", FLAGS_color, std::to_string(level));
}

filesystem::path getLevelForegroundMasksDir(const int level) {
  return fmt::format("{}/level_{}", FLAGS_foreground_masks, std::to_string(level));
}

filesystem::path getLevelBackgroundDisparityDir(const int level) {
  return fmt::format("{}/level_{}", FLAGS_background_disp, std::to_string(level));
}

// Verifies that we have all the frames we are asking for
void verifyInputImagePaths(
    const Camera::Rig& rigSrc,
    const Camera::Rig& rigDst,
    const int numLevels) {
  const int levelStart = FLAGS_level_start >= 0 ? FLAGS_level_start : numLevels - 1;
  verifyImagePaths(getLevelColorDir(levelStart), rigSrc, FLAGS_first, FLAGS_last);
  if (FLAGS_use_foreground_masks) {
    // We need just one background disparity, with one background mask per camera and frame
    verifyImagePaths(
        getLevelBackgroundDisparityDir(levelStart),
        rigDst,
        FLAGS_background_frame,
        FLAGS_background_frame);
    verifyImagePaths(getLevelForegroundMasksDir(levelStart), rigDst, FLAGS_first, FLAGS_last);
  }

  if (levelStart < numLevels - 1) {
    verifyImagePaths(getLevelDisparityDir(levelStart + 1), rigDst, FLAGS_first, FLAGS_last);
  }
}

int getLevelEnd(const std::map<int, cv::Size>& pyramidLevelSizes) {
  int levelEnd = 0;
  for (const auto& levelSize : pyramidLevelSizes) {
    if (levelSize.second.width <= FLAGS_resolution) {
      levelEnd = levelSize.first;
      break;
    }
  }

  if (FLAGS_level_end >= 0) {
    CHECK_GE(FLAGS_level_end, levelEnd) << fmt::format(
        "Requested end level {} ({}), which is larger than requested resolution ({})",
        FLAGS_level_end,
        pyramidLevelSizes.at(FLAGS_level_end).width,
        FLAGS_resolution);
  }

  levelEnd = std::max(levelEnd, FLAGS_level_end);
  return levelEnd;
}

int main(int argc, char* argv[]) {
  system_util::initDep(argc, argv, kUsageMessage);

  boost::timer::cpu_timer matchTimer;
  verifyInputs();

  Camera::Rig rigSrc = Camera::loadRig(FLAGS_rig);
  const int numSrcs = rigSrc.size();
  CHECK_GT(numSrcs, 0) << "no source cameras!";

  Camera::Rig rigDst = filterDestinations(rigSrc, FLAGS_cameras);
  const int numDsts = rigDst.size();
  CHECK_GT(numDsts, 0) << "no destination cameras!";
  const std::vector<int> dst2srcIdxs = mapSrcToDstIndexes(rigSrc, rigDst);

  // Get pyramid level sizes from both the disparity and color directories
  std::map<int, cv::Size> pyramidLevelSizes;
  getPyramidLevelSizes(pyramidLevelSizes, FLAGS_color);
  getPyramidLevelSizes(
      pyramidLevelSizes, getImageDir(FLAGS_output_root, ImageType::disparity_levels));
  const int numLevels =
      FLAGS_num_levels == -1 ? pyramidLevelSizes.rbegin()->first + 1 : FLAGS_num_levels;

  // Get largest level smaller or equal to the requested resolution
  const int levelStart = FLAGS_level_start >= 0 ? FLAGS_level_start : numLevels - 1;
  const int levelEnd = getLevelEnd(pyramidLevelSizes);

  CHECK_LE(FLAGS_level_start, numLevels);
  const int numFrames = std::stoi(FLAGS_last) - std::stoi(FLAGS_first) + 1;
  verifyInputImagePaths(rigSrc, rigDst, numLevels);
  filesystem::create_directories(FLAGS_output_root);

  // These must be computed before normalizing to determine the correct resolutions
  const Camera& camRef = rigDst[0];
  const int widthFullSize = camRef.resolution.x();
  const int heightFullSize = camRef.resolution.y();

  // Normalize cameras (needed to generate FOV masks and to process frames)
  Camera::normalizeRig(rigSrc);
  Camera::normalizeRig(rigDst);

  for (int level = levelStart; level >= levelEnd; --level) {
    // Create level output directories
    createLevelOutputDirs(FLAGS_output_root, level, rigDst, FLAGS_save_debug_images);

    // Create dst FOV masks for current level size
    const cv::Size& sizeLevel = pyramidLevelSizes.at(level);
    const std::vector<cv::Mat_<bool>> dstFovMasks =
        generateFovMasks(rigDst, sizeLevel, FLAGS_threads);

    for (int iFrame = 0; iFrame < numFrames; ++iFrame) {
      // Load current level data
      const std::string frameName =
          image_util::intToStringZeroPad(iFrame + std::stoi(FLAGS_first), 6);

      // Color
      std::vector<cv::Mat_<PixelType>> colorImagesLevel =
          loadLevelImages<PixelType>(FLAGS_color, level, rigSrc, frameName, FLAGS_threads);

      // Foreground masks
      std::vector<cv::Mat_<bool>> srcForegroundMasksLevel = FLAGS_use_foreground_masks
          ? loadLevelImages<bool>(FLAGS_foreground_masks, level, rigSrc, frameName, FLAGS_threads)
          : cv_util::generateAllPassMasks(sizeLevel, numSrcs);

      // Background disparities
      std::vector<cv::Mat_<float>> dstBackgroundDisparitiesLevel(rigDst.size());
      if (FLAGS_use_foreground_masks) {
        dstBackgroundDisparitiesLevel = loadLevelImages<float>(
            FLAGS_background_disp, level, rigDst, FLAGS_background_frame, FLAGS_threads);
      }

      PyramidLevel<PixelType> framePyramidLevel(
          iFrame,
          frameName,
          numFrames,
          level,
          numLevels,
          pyramidLevelSizes,
          rigSrc,
          rigDst,
          dst2srcIdxs,
          colorImagesLevel,
          srcForegroundMasksLevel,
          dstFovMasks,
          dstBackgroundDisparitiesLevel,
          widthFullSize,
          heightFullSize,
          FLAGS_color,
          FLAGS_var_noise_floor,
          FLAGS_var_high_thresh,
          FLAGS_use_foreground_masks,
          FLAGS_output_root,
          FLAGS_threads);

      // Generate/link reprojections
      precomputeProjections(framePyramidLevel, FLAGS_threads);

      if (level < numLevels - 1) {
        // Allocate masks but only populate them if needed
        std::vector<cv::Mat_<bool>> dstForegroundMasksLevel(numDsts);
        std::vector<cv::Mat_<bool>> dstForegroundMasksCoarse(numDsts);
        if (FLAGS_use_foreground_masks) {
          dstForegroundMasksLevel = loadLevelImages<bool>(
              FLAGS_foreground_masks, level, rigDst, frameName, FLAGS_threads);
          dstForegroundMasksCoarse = loadLevelImages<bool>(
              FLAGS_foreground_masks, level + 1, rigDst, frameName, FLAGS_threads);
        }

        const std::vector<cv::Mat_<float>> dstDispsCoarse =
            loadImages<float>(getLevelDisparityDir(level + 1), rigDst, frameName, FLAGS_threads);

        const std::vector<cv::Mat_<float>> dstDispsNextLevel = upsampleDisparities(
            rigDst,
            dstDispsCoarse,
            dstBackgroundDisparitiesLevel,
            dstForegroundMasksCoarse,
            dstForegroundMasksLevel,
            sizeLevel,
            FLAGS_use_foreground_masks,
            FLAGS_threads);

        for (int dstIdx = 0; dstIdx < numDsts; ++dstIdx) {
          framePyramidLevel.dsts[dstIdx].disparity = dstDispsNextLevel[dstIdx];
        }
      }

      processLevel(
          framePyramidLevel,
          FLAGS_output_formats,
          FLAGS_use_foreground_masks,
          FLAGS_output_root,
          FLAGS_random_proposals,
          FLAGS_partial_coverage,
          FLAGS_min_depth_m,
          FLAGS_max_depth_m,
          FLAGS_do_median_filter,
          FLAGS_save_debug_images,
          FLAGS_ping_pong_iterations,
          FLAGS_mismatches_start_level,
          FLAGS_do_bilateral_filter,
          FLAGS_threads);
    }

    LOG(INFO) << fmt::format("-- Elapsed time: {}", matchTimer.format());
  }

  LOG(INFO) << fmt::format("-- TOTAL: {}", matchTimer.format());

  return EXIT_SUCCESS;
}
