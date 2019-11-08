/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

const char* kUsage = R"(
  - Upscales the input disparity using the original color as guide.

  - Example:
    ./UpsampleDisparity \
    --rig=/path/to/rigs/rig.json \
    --disparity=/path/to/output/disparity \
    --color=/path/to/video/color \
    --foreground_masks_in=/path/to/video/foreground_masks/ \
    --foreground_masks_out=/path/to/video/foreground_masks_full_size/ \
    --output=/path/to/video/output/disparity_full_size \
    --frame=000000 \
    --background_disp=/path/to/background/disparity_full_size
)";

#include "source/depth_estimation/UpsampleDisparityLib.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/Format.h>

#include "source/depth_estimation/TemporalBilateralFilter.h"

using namespace fb360_dep;
using namespace fb360_dep::depth_estimation;

DEFINE_string(background_disp, "", "background disparity directory (output resolution)");
DEFINE_string(background_frame, "000000", "background frame (lexical)");
DEFINE_string(cameras, "", "destination cameras");
DEFINE_string(color, "", "color directory (output resolution)");
DEFINE_string(disparity, "", "disparity directory (input resolution) (required)");
DEFINE_string(first, "000000", "first frame to process (lexical)");
DEFINE_string(foreground_masks_in, "", "(optional) masks directory (input resolution)");
DEFINE_string(foreground_masks_out, "", "(optional) masks directory (output resolution)");
DEFINE_int32(height, -1, "output image height (aspect ratio maintained if unspecified)");
DEFINE_string(last, "000000", "last frame to process (lexical)");
DEFINE_string(output, "", "output directory (required)");
DEFINE_string(output_formats, "", "saved formats, comma separated (exr, png, pfm supported)");
DEFINE_int32(resolution, -1, "output resolution width in pixels (required)");
DEFINE_string(rig, "", "path to camera rig .json");
DEFINE_double(sigma, 0.05, "bilateral filter color difference sigma");
DEFINE_int32(threads, -1, "number of threads (-1 = auto, 0 = none)");
DEFINE_double(weight_b, 0.5, "bilateral filter blue channel weight");
DEFINE_double(weight_g, 0.5, "bilateral filter green channel weight");
DEFINE_double(weight_r, 1.0, "bilateral filter red channel weight");

using PixelType = cv::Vec3f;

void verifyInputs() {
  CHECK_NE(FLAGS_disparity, "");
  CHECK_NE(FLAGS_output, "");
  CHECK_NE(FLAGS_resolution, -1);
}

void upsampleFrame(const Camera::Rig& rigSrc, const Camera::Rig& rigDst, const std::string& frame) {
  const std::string exts = FLAGS_output_formats.empty() ? "pfm" : FLAGS_output_formats;
  std::vector<std::string> outputFormats;
  folly::split(",", exts, outputFormats);
  const std::vector<cv::Mat_<float>> disps =
      image_util::loadImages<float>(FLAGS_disparity, rigDst, frame, FLAGS_threads);
  CHECK_GT(disps.size(), 0);
  std::vector<cv::Mat_<PixelType>> colors;
  if (!FLAGS_color.empty()) {
    colors = image_util::loadImages<PixelType>(FLAGS_color, rigDst, frame, FLAGS_threads);
  }

  std::vector<cv::Mat_<float>> backgroundDispsUp;
  if (!FLAGS_background_disp.empty()) {
    backgroundDispsUp = image_util::loadImages<float>(
        FLAGS_background_disp, rigDst, FLAGS_background_frame, FLAGS_threads);
  } else {
    cv::Mat_<float> temp;
    std::vector<cv::Mat_<float>> backgroundDispsUpTemp(int(rigDst.size()), temp);
    backgroundDispsUp = backgroundDispsUpTemp;
  }

  int height;
  if (FLAGS_height == -1) {
    height =
        std::round(float(rigDst[0].resolution.y()) / rigDst[0].resolution.x() * FLAGS_resolution);
    height += height % 2; // force even height
  } else {
    height = FLAGS_height;
  }
  const cv::Size sizeUp(FLAGS_resolution, height);

  const bool useForegroundMasks = !FLAGS_foreground_masks_in.empty();
  std::vector<cv::Mat_<bool>> masks = useForegroundMasks
      ? image_util::loadImages<bool>(FLAGS_foreground_masks_in, rigDst, frame, FLAGS_threads)
      : cv_util::generateAllPassMasks(disps[0].size(), int(rigDst.size()));

  const std::vector<cv::Mat_<bool>> masksUp = !FLAGS_foreground_masks_out.empty()
      ? image_util::loadImages<bool>(FLAGS_foreground_masks_out, rigDst, frame, FLAGS_threads)
      : cv_util::generateAllPassMasks(sizeUp, int(rigDst.size()));

  std::vector<cv::Mat_<float>> dispsUp = upsampleDisparities(
      rigDst, disps, backgroundDispsUp, masks, masksUp, sizeUp, useForegroundMasks, FLAGS_threads);
  for (ssize_t i = 0; i < ssize(rigDst); ++i) {
    if (!FLAGS_color.empty()) {
      const int radius = getRadius(masks[i].size(), sizeUp);
      LOG(INFO) << folly::sformat(
          "Applying filter with radius {} to {}x{} disparity to {}...",
          radius,
          sizeUp.width,
          sizeUp.height,
          rigDst[i].id);
      const cv::Mat_<PixelType> colorUp = cv_util::resizeImage(colors[i], sizeUp);
      dispsUp[i] = depth_estimation::generalizedJointBilateralFilter<float, PixelType>(
          dispsUp[i],
          colorUp,
          colorUp,
          masksUp[i],
          radius,
          FLAGS_sigma,
          FLAGS_weight_b,
          FLAGS_weight_g,
          FLAGS_weight_r,
          FLAGS_threads);
    }

    LOG(INFO) << "Saving output images...";
    for (const std::string& ext : outputFormats) {
      const std::string frameFn = ext[0] == '.' ? frame + ext : frame + '.' + ext;
      const filesystem::path fn = filesystem::path(FLAGS_output) / rigDst[i].id / frameFn;
      boost::filesystem::create_directories(fn.parent_path());

      if (ext != "pfm") {
        cv_util::imwriteExceptionOnFail(fn, cv_util::convertTo<uint16_t>(dispsUp[i]));
      } else {
        cv_util::writeCvMat32FC1ToPFM(fn, dispsUp[i]);
      }
    }
  }
}

int main(int argc, char** argv) {
  gflags::SetUsageMessage(kUsage);
  system_util::initDep(argc, argv);

  verifyInputs();

  Camera::Rig rigSrc = Camera::loadRig(FLAGS_rig);
  Camera::Rig rigDst = image_util::filterDestinations(rigSrc, FLAGS_cameras);
  std::pair<int, int> frameRange =
      image_util::getFrameRange(FLAGS_disparity, rigDst, FLAGS_first, FLAGS_last);

  for (int iFrame = frameRange.first; iFrame <= frameRange.second; ++iFrame) {
    upsampleFrame(rigSrc, rigDst, image_util::intToStringZeroPad(iFrame, 6));
  }

  return EXIT_SUCCESS;
}
