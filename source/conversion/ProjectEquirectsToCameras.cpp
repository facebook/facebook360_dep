/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

const char* kUsage = R"(
  - Reads equirect masks and projects them to individual cameras assuming a given depth.

  - Example:
    ./ProjectEquirectsToCameras \
    --eqr_masks=/path/to/video/equirect_masks/ \
    --rig=/path/to/rigs/rig.json \
    --first=000000 \
    --last=000000 \
    --output=/path/to/output/
)";

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/util/Camera.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"
#include "source/util/ThreadPool.h"

using namespace fb360_dep;
using namespace fb360_dep::image_util;

DEFINE_string(cameras, "", "comma-separated cameras to render (empty for all)");
DEFINE_double(depth, 1000, "depth to project at (m)");
DEFINE_string(eqr_masks, "", "path to input equirect masks (required)");
DEFINE_string(file_type, "png", "Supports any image type allowed in OpenCV");
DEFINE_string(first, "000000", "first frame to process (lexical) (required)");
DEFINE_string(last, "000000", "last frame to process (lexical) (required)");
DEFINE_string(output, "", "output directory (required)");
DEFINE_string(rig, "", "path to camera rig .json (required)");
DEFINE_int32(threads, -1, "number of threads (-1 = auto, 0 = none)");
DEFINE_int32(width, 0, "width of projected camera images (0 = size from rig file)");

void verifyInputs(const Camera::Rig& rig) {
  CHECK_NE(FLAGS_eqr_masks, "");
  CHECK_NE(FLAGS_first, "");
  CHECK_NE(FLAGS_last, "");
  CHECK_NE(FLAGS_output, "");
  CHECK_GT(FLAGS_depth, 0);
  CHECK_GE(FLAGS_width, 0);
  CHECK_EQ(FLAGS_width % 2, 0) << "equirect width must be a multiple of 2";
  CHECK_GT(rig.size(), 0);

  if (!FLAGS_eqr_masks.empty()) {
    verifyImagePaths(FLAGS_eqr_masks, rig, FLAGS_first, FLAGS_last);
  }
}

void rescaleCameras(Camera::Rig& rig) {
  for (Camera& cam : rig) {
    if (FLAGS_width > 0) {
      int height = ceil(FLAGS_width * cam.resolution.y() / float(cam.resolution.x()));
      height += height % 2; // force even number of rows
      cam = cam.rescale({FLAGS_width, height});
    }

    LOG(INFO) << folly::sformat(
        "{} output resolution: {}x{}", cam.id, cam.resolution.x(), cam.resolution.y());
  }
}

int main(int argc, char** argv) {
  gflags::SetUsageMessage(kUsage);
  system_util::initDep(argc, argv);

  CHECK_NE(FLAGS_rig, "");
  Camera::Rig rig = filterDestinations(Camera::loadRig(FLAGS_rig), FLAGS_cameras);

  verifyInputs(rig);
  rescaleCameras(rig);

  const int first = std::stoi(FLAGS_first);
  const int last = std::stoi(FLAGS_last);
  for (int iFrame = first; iFrame <= last; ++iFrame) {
    const std::string frameName = image_util::intToStringZeroPad(iFrame, 6);
    LOG(INFO) << folly::sformat("Frame {}: Loading equirect masks...", frameName);
    const std::vector<cv::Mat_<bool>> eqrMasks = loadImages<bool>(FLAGS_eqr_masks, rig, frameName);
    CHECK_EQ(ssize(eqrMasks), ssize(rig));

    for (ssize_t i = 0; i < ssize(rig); ++i) {
      const Camera& cam = rig[i];
      LOG(INFO) << folly::sformat("-- Frame {}: Projecting to {}...", frameName, cam.id);

      // For each pixel in the current camera find out where in equirect space we land
      ThreadPool threadPool(FLAGS_threads);
      cv::Mat_<bool> camMask(cam.resolution.y(), cam.resolution.x(), false);
      const cv::Mat_<bool>& eqrMask = eqrMasks[i];

      int h = camMask.rows;
      int w = camMask.cols;
      const int edgeX = w;
      const int edgeY = 1;
      for (int yBegin = 0; yBegin < h; yBegin += edgeY) {
        for (int xBegin = 0; xBegin < w; xBegin += edgeX) {
          const int xEnd = std::min(xBegin + edgeX, w);
          const int yEnd = std::min(yBegin + edgeY, h);
          threadPool.spawn([&, xBegin, yBegin, xEnd, yEnd] {
            for (int y = yBegin; y < yEnd; ++y) {
              for (int x = xBegin; x < xEnd; ++x) {
                const Camera::Vector3 world = cam.rig({x + 0.5, y + 0.5}, FLAGS_depth);
                const Camera::Vector2 pEqr =
                    image_util::worldToEquirect(world, eqrMask.cols, eqrMask.rows);
                if (pEqr.x() < 0 || pEqr.y() < 0 || pEqr.x() >= eqrMask.cols ||
                    pEqr.y() >= eqrMask.rows) {
                  continue; // rounding can put us at image edge. Ignore these cases
                }
                if (eqrMask(pEqr.y(), pEqr.x())) {
                  camMask(y, x) = true;
                }
              }
            }
          });
        }
      }
      threadPool.join();

      const filesystem::path filename =
          filesystem::path(FLAGS_output) / cam.id / (frameName + "." + FLAGS_file_type);
      filesystem::create_directories(filename.parent_path());
      cv_util::imwriteExceptionOnFail(filename, 255.0f * camMask);
    }
  }

  return EXIT_SUCCESS;
}
