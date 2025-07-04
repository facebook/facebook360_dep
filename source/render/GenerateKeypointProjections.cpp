/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/format.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <opencv2/opencv.hpp>

#include "source/calibration/Calibration.h"
#include "source/calibration/FeatureMatcher.h"
#include "source/util/Camera.h"
#include "source/util/CvUtil.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;

const std::string kUsageMessage = R"(
  - Reprojects a grid of keypoints to another camera at different depths.

  - Example:
    ./GenerateKeypointProjections \
    --color=/path/to/video/color \
    --frame=000000 \
    --rig=/path/to/rigs/rig.json \
    --output_dir=/path/to/output
  )";

DEFINE_double(height_stride, 0.125, "x grid stride in percent");
DEFINE_double(length_stride, 0.125, "y grid stride in percent");
DEFINE_string(output_dir, "", "path to output directory");
DEFINE_string(rig, "", "path to camera rig .json file");

DECLARE_int32(search_radius);

int main(int argc, char* argv[]) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_rig, "");
  CHECK_NE(FLAGS_output_dir, "");

  filesystem::create_directories(FLAGS_output_dir);

  // Load camera rig
  const Camera::Rig rig = Camera::loadRig(FLAGS_rig);
  const std::vector<cv::Mat_<cv::Vec4f>> images =
      image_util::loadImages<cv::Vec4f>(FLAGS_color, rig, FLAGS_frame, FLAGS_threads);

  const double height = rig[0].resolution.y();
  const double width = rig[0].resolution.x();

  for (int c0Idx = 0; c0Idx < int(rig.size()); ++c0Idx) {
    for (int c1Idx = c0Idx + 1; c1Idx < int(rig.size()); ++c1Idx) {
      cv::Mat_<cv::Vec4f> keypointProjection(height, width, cv::Scalar(0, 0, 0, 0));

      // Project a grid of points seen by the camera at c1Idx to the camera at c0Idx at different
      // depths
      for (double width_frac = 0; width_frac <= 1; width_frac += FLAGS_length_stride) {
        for (double height_frac = 0; height_frac <= 1; height_frac += FLAGS_height_stride) {
          const Camera::Vector2 c1Point(width * width_frac, height * height_frac);
          if (rig[c1Idx].isOutsideImageCircle(
                  Camera::Vector2(c1Point.x() + 0.5, c1Point.y() + 0.5))) {
            continue;
          }

          int depthSample = -1;
          double disparity = 0;
          cv::Rect2f box1(0, 0, 0, 0);

          // Unique color for this grid point (all depths will be the same color)
          const cv::Vec4f color = cv::Vec4f(height_frac, width_frac, (1 - height_frac), 1);
          while (calibration::getNextDepthSample(
              depthSample, disparity, box1, rig[c1Idx], c1Point, rig[c0Idx])) {
            const Camera::Vector3 cornerWorldProjection = rig[c1Idx].rig(c1Point, 1 / disparity);
            Camera::Vector2 projectedPoint;
            // Draw a circle centered at the reprojected point
            // Thickness of -1 indicates the circle is filled in
            if (rig[c0Idx].sees(cornerWorldProjection, projectedPoint)) {
              cv::rectangle(
                  keypointProjection,
                  cv::Point2d(
                      projectedPoint.x() - 0.5 - FLAGS_search_radius,
                      projectedPoint.y() - 0.5 - FLAGS_search_radius),
                  cv::Point2d(
                      projectedPoint.x() - 0.5 + FLAGS_search_radius,
                      projectedPoint.y() - 0.5 + FLAGS_search_radius),
                  color,
                  -1);
            }
          }
        }
      }

      cv::Mat_<cv::Vec4f> blend;
      cv::addWeighted(images[c0Idx], 1, keypointProjection, 0.6, 0, blend);
      const std::string fn =
          fmt::format("{}/{}_{}.png", FLAGS_output_dir, rig[c0Idx].id, rig[c1Idx].id);
      cv_util::imwriteExceptionOnFail(fn, 255.0f * blend);
    }
  }

  return EXIT_SUCCESS;
}
