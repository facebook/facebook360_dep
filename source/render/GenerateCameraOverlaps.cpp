/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/Format.h>

#include "source/util/Camera.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"
#include "source/util/ThreadPool.h"

using namespace fb360_dep;
using namespace fb360_dep::image_util;

const std::string kUsageMessage = R"(
   - Generates a series of images of the rig cameras projected into destination cameras over
   a series of fixed depths.

   - Example:
     ./GenerateCameraOverlaps \
     --frame=000000 \
     --output=/path/to/output \
     --rig=/path/to/rigs/rig.json \
     --color=/path/to/video/color

     A typical extension of this is creating a video over the series of depth generated, i.e.:

     ffmpeg -framerate 10 -pattern_type glob \
     -i '/path/to/output/overlaps/cam0/*.png' -c:v libx264 -pix_fmt yuv420p \
     -vf "scale=trunc(iw/2)*2:trunc(ih/2)*2" /path/to/output/overlaps/cam0.mp4 -y
 )";

DEFINE_string(cameras, "", "cameras to render (comma-separated)");
DEFINE_string(color, "", "path to input color images (required)");
DEFINE_string(frame, "000000", "frame to process (lexical)");
DEFINE_uint64(max_depth_m, 10, "max depth in cm");
DEFINE_uint64(min_depth_m, 1, "min depth in cm");
DEFINE_uint64(num_depths, 50, "num depths");
DEFINE_string(output, "", "path to output directory (required)");
DEFINE_string(rig, "", "path to camera rig .json (required)");
DEFINE_double(scale, 0.5, "image scale factor");

using PixelType = cv::Vec4f;
using Image = cv::Mat_<PixelType>;

Image projectSrcsToDst(
    const Camera& camDst,
    const Camera::Rig& rigSrc,
    const std::vector<Image>& imagesSrc,
    const float disparity) {
  Image colorDst(camDst.resolution.y(), camDst.resolution.x(), cv::Scalar(0));

  for (int y = 0; y < colorDst.rows; ++y) {
    for (int x = 0; x < colorDst.cols; ++x) {
      Camera::Vector2 dstPixel = {x + 0.5, y + 0.5};
      if (camDst.isOutsideImageCircle(dstPixel)) {
        colorDst(y, x) = 0.0f;
        continue;
      }
      const Camera::Vector3 world = camDst.rig(dstPixel, 1.0f / disparity);
      int count = 0;
      PixelType sum = cv_util::createBGRA<PixelType>(0, 0, 0, 0);
      for (int iSrc = 0; iSrc < int(rigSrc.size()); ++iSrc) {
        Camera::Vector2 srcPixel;
        if (rigSrc[iSrc].sees(world, srcPixel)) {
          sum += cv_util::getPixelBilinear(imagesSrc[iSrc], srcPixel.x(), srcPixel.y());
          ++count;
        }
      }
      colorDst(y, x) = 1.0f / count * sum;
    }
  }

  return colorDst;
}

void dumpOverlaps(
    const Camera::Rig& rigSrc,
    const Camera::Rig& rigDst,
    const std::vector<Image>& imagesSrc,
    const int numDisps,
    const float minDisparity,
    const float maxDisparity,
    const filesystem::path& outputDir) {
  ThreadPool threadPool;

  for (const Camera& camDst : rigDst) {
    filesystem::create_directories(outputDir / camDst.id);
  }

  // Loop through disparities
  for (int d = 0; d < numDisps; ++d) {
    LOG(INFO) << folly::sformat("Depth {} of {}...", (d + 1), numDisps);
    threadPool.spawn([&, d] {
      const float disparity = probeDisparity(d, numDisps, minDisparity, maxDisparity);
      for (const Camera& camDst : rigDst) {
        const Image colorDst = projectSrcsToDst(camDst, rigSrc, imagesSrc, disparity);

        // Add text to image showing current depth
        const float depth = 1.0f / disparity;
        const float depthCm = depth * 100;
        const std::string depthStr = std::to_string(int(depthCm));
        const cv::Point2f textPos(
            (80.0f / 100.0f) * colorDst.cols, (6.0f / 100.0f) * colorDst.rows);
        const int textFont = cv::FONT_HERSHEY_PLAIN;
        const double textScale = 2;
        const cv::Scalar textColor(0, 1, 0, 1); // green
        cv::putText(colorDst, depthStr + " cm", textPos, textFont, textScale, textColor);

        // Pad filename with zeros so they are saved in lexicographical order
        const std::string filename =
            folly::sformat("{}/{}/{:05}_cm.png", outputDir.string(), camDst.id, int(depthCm));
        cv_util::imwriteExceptionOnFail(filename, 255.0f * colorDst);
      }
    });
  }
  threadPool.join();
}

int main(int argc, char* argv[]) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_color, "");
  CHECK_NE(FLAGS_rig, "");
  CHECK_NE(FLAGS_output, "");

  Camera::Rig rigSrc = Camera::loadRig(FLAGS_rig);
  for (Camera& src : rigSrc) {
    src = src.rescale(src.resolution * FLAGS_scale);
  }
  const Camera::Rig rigDst = filterDestinations(rigSrc, FLAGS_cameras);
  CHECK_GT(rigDst.size(), 0) << "no destinations!";

  LOG(INFO) << "Loading images...";
  const std::vector<Image> imagesSrc =
      loadScaledImages<PixelType>(FLAGS_color, rigSrc, FLAGS_frame, FLAGS_scale);
  CHECK_EQ(imagesSrc.size(), rigSrc.size());

  const filesystem::path overlapsDir = filesystem::path(FLAGS_output) / "overlaps";
  dumpOverlaps(
      rigSrc,
      rigDst,
      imagesSrc,
      FLAGS_num_depths,
      1.0f / FLAGS_min_depth_m,
      1.0f / FLAGS_max_depth_m,
      overlapsDir);

  return EXIT_SUCCESS;
}
