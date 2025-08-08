/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/algorithm/string/split.hpp>
#include <fmt/format.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/Format.h>

#include "source/gpu/GlUtil.h"
#include "source/gpu/GlfwUtil.h"
#include "source/render/CanopyScene.h"
#include "source/render/DisparityColor.h"
#include "source/render/RephotographyUtil.h"
#include "source/util/CvUtil.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;
using namespace fb360_dep::image_util;

using PixelType = cv::Vec4f;
using PixelTypeNoAlpha = cv::Vec3f;

const std::string kUsageMessage = R"(
   - Computes rephotography error for a set of frames. Rephotography error for a single frame is
   computed by generating cubemaps for both the reference and the rendered data, translating the
   cubemap origin to the center of the reference camera, and computing the MSSIM for each camera.

   - Example:
     ./ComputeRephotographyErrors \
     --first=000000 \
     --last=000000 \
     --output=/path/to/output \
     --rig=/path/to/rigs/rig.json \
     --color=/path/to/video/color \
     --disparity=/path/to/output/disparity
 )";

DEFINE_string(cameras, "", "comma-separated cameras to render (empty for all)");
DEFINE_string(color, "", "path to input color images (required)");
DEFINE_string(disparity, "", "path to disparity images (required)");
DEFINE_string(first, "", "first frame to process (lexical) (required)");
DEFINE_string(last, "", "last frame to process (lexical) (required)");
DEFINE_string(method, "MSSIM", "MSSIM or NCC");
DEFINE_string(output, "", "path to output directory (required)");
DEFINE_string(rig, "", "path to camera rig .json (required)");
DEFINE_int32(stat_radius, 1, "local statistics window radius");

template <typename T>
std::vector<T> removeOne(int skip, const std::vector<T>& v) {
  CHECK_LT(skip, v.size());
  std::vector<T> result = v;
  result.erase(result.begin() + skip);
  return result;
}

template <typename T>
cv::Mat_<T> zeroOutNans(const cv::Mat_<T>& imageIn) {
  cv::Mat_<T> imageOut = imageIn.clone();

  // Some versions of OpenCV require the mask to be 8UC1
  const cv::Mat_<uint8_t> mask = cv_util::convertImage<uint8_t>(imageOut != imageOut);

  if (!mask.empty()) {
    imageOut.setTo(0, mask);
  }
  return imageOut;
}

std::vector<cv::Mat_<PixelType>> generateCubemaps(
    const Camera::Rig& rig,
    const std::vector<cv::Mat_<PixelType>>& colors,
    const std::vector<cv::Mat_<float>>& disparities,
    const int cubeHeight,
    const Eigen::Vector3f& center) {
  CHECK_EQ(colors.size(), disparities.size());
  std::vector<cv::Mat_<PixelType>> cubemaps;

  const CanopyScene sceneColor(rig, disparities, colors);
  cubemaps.push_back(zeroOutNans(sceneColor.cubemap(cubeHeight, center)));

  const std::vector<cv::Mat_<PixelType>> dispColors =
      disparityColors(rig, disparities, center, metersToGrayscale);
  const CanopyScene sceneDisparity(rig, disparities, dispColors);
  cubemaps.push_back(zeroOutNans(sceneDisparity.cubemap(cubeHeight, center)));

  return cubemaps;
}

class OffscreenWindow : public GlWindow {
 protected:
  Camera::Rig& rig;

 public:
  OffscreenWindow(Camera::Rig& rig) : GlWindow::GlWindow(), rig(rig) {}

  void display() override {
    // Create output directories
    const filesystem::path rephotoDir = filesystem::path(FLAGS_output) / "rephoto";
    for (const Camera& cam : rig) {
      filesystem::create_directories(rephotoDir / cam.id);
    }

    cv::Scalar totalScore = cv::Scalar::all(0);
    const int numFrames = std::stoi(FLAGS_last) - std::stoi(FLAGS_first) + 1;
    CHECK_GT(numFrames, 0);

    for (int iFrame = 0; iFrame < numFrames; ++iFrame) {
      const std::string frameName =
          image_util::intToStringZeroPad(iFrame + std::stoi(FLAGS_first), 6);
      LOG(INFO) << fmt::format("Processing frame {}...", frameName);

      LOG(INFO) << "Loading color and disparity images...";

      std::vector<cv::Mat_<float>> disps = loadPfmImages(FLAGS_disparity, rig, frameName);
      CHECK_EQ(rig.size(), disps.size());

      // Need to scale to m to match convention
      const std::vector<cv::Mat_<PixelType>> colors = loadResizedImages<PixelType>(
          FLAGS_color, rig, frameName, disps[0].size(), cv::INTER_AREA);
      CHECK_EQ(colors.size(), disps.size());

      const int cubeHeight = colors[0].rows;
      cv::Scalar frameScore = cv::Scalar::all(0);
      std::vector<std::string> cameras;
      if (!FLAGS_cameras.empty()) {
        boost::split(cameras, FLAGS_cameras, [](char c) { return c == ','; });
      }
      for (ssize_t i = 0; i < ssize(rig); ++i) {
        const std::string camId = rig[i].id;
        if (!cameras.empty()) {
          if (std::find(cameras.begin(), cameras.end(), camId) == cameras.end()) {
            continue;
          }
        }

        LOG(INFO) << fmt::format("Processing {} - {}...", frameName, camId);
        const Eigen::Vector3f center = rig[i].position.cast<float>();

        std::vector<cv::Mat_<PixelType>> cubesRef =
            generateCubemaps({rig[i]}, {colors[i]}, {disps[i]}, cubeHeight, center);
        std::vector<cv::Mat_<PixelType>> cubesRender = generateCubemaps(
            removeOne(i, rig), removeOne(i, colors), removeOne(i, disps), cubeHeight, center);

        // Create mask
        const int kColor = 0;
        const cv::Mat_<float> alpha = cv_util::extractAlpha(cubesRef[kColor]);
        const cv::Mat_<uint8_t> mask = 255 * (alpha > 0);

        // Remove color alphas
        cv::Mat_<PixelTypeNoAlpha> cubesRefColorNoAlpha = cv_util::removeAlpha(cubesRef[kColor]);
        cv::Mat_<PixelTypeNoAlpha> cubesRenderColorNoAlpha =
            cv_util::removeAlpha(cubesRender[kColor]);

        // Compute scores
        const cv::Mat_<PixelTypeNoAlpha> scoreMap = rephoto_util::computeScoreMap(
            FLAGS_method, cubesRefColorNoAlpha, cubesRenderColorNoAlpha, FLAGS_stat_radius);

        const cv::Scalar avgScore = rephoto_util::averageScore(scoreMap, mask);
        LOG(INFO) << fmt::format(
            "{} {}: {}", camId, FLAGS_method, rephoto_util::formatResults(avgScore));
        frameScore += avgScore;

        // Plot results
        const cv::Mat_<cv::Vec3b> plot =
            rephoto_util::stackResults(cubesRef, cubesRender, scoreMap, avgScore, mask);
        const std::string filename =
            fmt::format("{}/{}/{}.png", rephotoDir.string(), camId, frameName);
        cv_util::imwriteExceptionOnFail(filename, plot);
      }

      const int n = !cameras.empty() ? cameras.size() : rig.size();
      frameScore.val[0] /= n;
      frameScore.val[1] /= n;
      frameScore.val[2] /= n;
      LOG(INFO) << fmt::format(
          "{} average {}: {}", frameName, FLAGS_method, rephoto_util::formatResults(frameScore));
      totalScore += frameScore;
    }

    totalScore.val[0] /= numFrames;
    totalScore.val[1] /= numFrames;
    totalScore.val[2] /= numFrames;
    LOG(INFO) << fmt::format(
        "TOTAL average {}: {}", FLAGS_method, rephoto_util::formatResults(totalScore));
  }
};

int main(int argc, char** argv) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_color, "");
  CHECK_NE(FLAGS_disparity, "");
  CHECK_NE(FLAGS_rig, "");
  CHECK_NE(FLAGS_output, "");
  CHECK_NE(FLAGS_first, "");
  CHECK_NE(FLAGS_last, "");
  CHECK_GT(FLAGS_stat_radius, 0);
  CHECK(FLAGS_method == "MSSIM" || FLAGS_method == "NCC") << "invalid method " << FLAGS_method;

  Camera::Rig rig = Camera::loadRig(FLAGS_rig);
  CHECK_GT(rig.size(), 0);

  verifyImagePaths(FLAGS_color, rig, FLAGS_first, FLAGS_last);
  verifyImagePaths(FLAGS_disparity, rig, FLAGS_first, FLAGS_last, ".pfm");

  // Prepare for offscreen rendering
  OffscreenWindow window(rig);

  // Render
  window.display();
  return EXIT_SUCCESS;
}
