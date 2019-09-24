/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

const char* kUsage = R"(
  - Reads cameras and projects them to equirect at a given depth.

  - Example:
    ./ProjectCamerasToEquirects \
    --color=/path/to/video/color \
    --rig=/path/to/rigs/rig_calibrated.json \
    --first=000000 \
    --last=000000 \
    --output=/path/to/output
)";

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/gpu/GlUtil.h"
#include "source/gpu/GlfwUtil.h"
#include "source/render/CanopyScene.h"
#include "source/util/Camera.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;
using namespace fb360_dep::image_util;

DEFINE_string(cameras, "", "comma-separated cameras to render (empty for all)");
DEFINE_string(color, "", "path to input color images (required)");
DEFINE_double(depth, 1000, "depth to project at (m)");
DEFINE_int32(eqr_width, 1024, "equirect width (pixels)");
DEFINE_string(file_type, "png", "Supports any image type allowed in OpenCV");
DEFINE_string(first, "000000", "first frame to process (lexical)");
DEFINE_string(last, "000000", "last frame to process (lexical)");
DEFINE_string(output, "", "output directory (required)");
DEFINE_string(rig, "", "path to camera rig .json (required)");

void verifyInputs(const Camera::Rig& rig) {
  CHECK_NE(FLAGS_color, "");
  CHECK_NE(FLAGS_first, "");
  CHECK_NE(FLAGS_last, "");
  CHECK_NE(FLAGS_output, "");
  CHECK_GT(FLAGS_depth, 0);
  CHECK_GE(FLAGS_eqr_width, 0);
  CHECK_EQ(FLAGS_eqr_width % 2, 0) << "equirect width must be a multiple of 2";
  CHECK_GT(rig.size(), 0);

  if (!FLAGS_color.empty()) {
    verifyImagePaths(FLAGS_color, rig, FLAGS_first, FLAGS_last);
  }
}

void save(const filesystem::path& path, const cv::Mat_<cv::Vec4f>& result) {
  filesystem::create_directories(path.parent_path());
  cv::Mat out;
  if (FLAGS_file_type == "jpg") {
    out = cv_util::convertImage<cv::Vec3b>(result);
  } else {
    out = cv_util::convertImage<cv::Vec4w>(result);
  }
  cv_util::imwriteExceptionOnFail(path, out);
}

class GlOffscreenWindow : GlWindow {
  const Camera::Rig& rig;

 public:
  GlOffscreenWindow(const Camera::Rig& rig) : rig(rig) {}

  void display() override {
    // Generate disparities with constant depth
    std::vector<cv::Mat_<float>> disparities;
    for (const Camera& cam : rig) {
      disparities.emplace_back(cam.resolution.y(), cam.resolution.x(), 1.0f / FLAGS_depth);
    }

    const int first = std::stoi(FLAGS_first);
    const int last = std::stoi(FLAGS_last);
    for (int iFrame = first; iFrame <= last; ++iFrame) {
      const std::string frameName = image_util::intToStringZeroPad(iFrame, 6);
      LOG(INFO) << folly::sformat("Frame {}: Loading colors...", frameName);
      const std::vector<cv::Mat_<cv::Vec4f>> colors =
          loadImages<cv::Vec4f>(FLAGS_color, rig, frameName);
      CHECK_EQ(ssize(colors), ssize(rig));

      for (ssize_t i = 0; i < ssize(rig); ++i) {
        LOG(INFO) << folly::sformat("-- Frame {}: Projecting {}...", frameName, rig[i].id);

        // Create scene with just this camera
        const CanopyScene sceneColor({rig[i]}, {disparities[i]}, {colors[i]});
        const Eigen::Vector3f& pos = {0, 0, 0};
        const float ipd = 0.0f;
        const bool alphaBlend = false;
        const int height = FLAGS_eqr_width / 2.0f;
        const cv::Mat_<cv::Vec4f> eqr = sceneColor.equirect(height, pos, ipd, alphaBlend);
        const filesystem::path filename =
            filesystem::path(FLAGS_output) / rig[i].id / (frameName + "." + FLAGS_file_type);
        save(filename, eqr);
      }
    }
  }
};

int main(int argc, char** argv) {
  gflags::SetUsageMessage(kUsage);
  system_util::initDep(argc, argv);

  CHECK_NE(FLAGS_rig, "");
  const Camera::Rig rig = filterDestinations(Camera::loadRig(FLAGS_rig), FLAGS_cameras);

  verifyInputs(rig);

  // Setup offscreen rendering
  GlOffscreenWindow context(rig);

  context.display();

  return EXIT_SUCCESS;
}
