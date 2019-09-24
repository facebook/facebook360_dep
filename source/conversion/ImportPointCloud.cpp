/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

const char* kUsage = R"(
  - Reads a point cloud as an ASCII file with a single point per line and generates a disparity
  image per camera.

  Supports multiple point cloud formats, but only extracts the xyz coordinates.

  The input file can have a single line header with a point count.

  - Example:
    ./ImportPointCloud \
    --output=/path/to/output \
    --rig=/path/to/rigs/rig.json \
    --point_cloud=/path/to/points.xyz

    Where points.xyz may be of the form:

    10000
    -0.04503071680665016 -2.2521071434020996 4.965743541717529 1 90 104 136
    -0.005194493103772402 -2.323836088180542 4.938142776489258 1 94 110 143
    0.046292994171381 -2.2623345851898193 4.609960079193115 1 101 122 149
    ...
)";

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/conversion/PointCloudUtil.h"
#include "source/util/Camera.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"
#include "source/util/ThreadPool.h"

using namespace fb360_dep;
using namespace fb360_dep::image_util;
using namespace fb360_dep::point_cloud_util;

DEFINE_string(cameras, "", "comma-separated cameras to render (empty for all)");
DEFINE_double(max_depth, INFINITY, "ignore depths farther than this value (m)");
DEFINE_double(min_depth, 0, "ignore depths closer than this value (m)");
DEFINE_string(output, "", "output directory (required)");
DEFINE_string(point_cloud, "", "input point cloud (required)");
DEFINE_string(rig, "", "path to camera rig .json (required)");
DEFINE_int32(threads, -1, "number of threads (-1 = auto, 0 = none)");
DEFINE_int32(width, 1024, "width of output camera images (0 = size from rig file)");

void verifyInputs(const Camera::Rig& rig) {
  CHECK_NE(FLAGS_point_cloud, "");
  CHECK_NE(FLAGS_output, "");
  CHECK_GE(FLAGS_width, 0);
  CHECK_EQ(FLAGS_width % 2, 0) << "width must be a multiple of 2";
  CHECK_GT(rig.size(), 0);
}

void rescaleCameras(Camera::Rig& rig) {
  for (Camera& cam : rig) {
    if (FLAGS_width > 0) {
      int height = std::round(FLAGS_width * cam.resolution.y() / float(cam.resolution.x()));
      height += height % 2; // force even number of rows
      cam = cam.rescale({FLAGS_width, height});
    }

    LOG(INFO) << folly::sformat(
        "{} output resolution: {}x{}", cam.id, cam.resolution.x(), cam.resolution.y());
  }
}

std::vector<cv::Mat_<float>> projectPointsToCameras(
    const PointCloud& points,
    const Camera::Rig& rig) {
  LOG(INFO) << "Projecting points to cameras...";

  std::vector<cv::Mat_<float>> disparities;
  for (const Camera& cam : rig) {
    disparities.emplace_back(cam.resolution.y(), cam.resolution.x(), 0.0f);
  }

  ThreadPool threadPool(FLAGS_threads);
  const int threads = threadPool.getMaxThreads();

  // Evenly distribute lines across threads
  const int pointCount = points.size();
  const int pointsPerThread = float(pointCount) / threads;
  const int remain = pointCount % threads;

  for (int i = 0; i < threads; ++i) {
    threadPool.spawn([&, i] {
      const int begin =
          i < remain ? i * (pointsPerThread + 1) : pointCount - (threads - i) * pointsPerThread;
      const int end = begin + pointsPerThread + (i < remain);
      for (int j = begin; j < end; ++j) {
        // Project point to all cameras
        for (ssize_t i = 0; i < ssize(rig); ++i) {
          const Camera::Vector3& pWorld = points[j].coords;
          Camera::Vector2 pSrc;
          if (!rig[i].sees(pWorld, pSrc)) {
            continue; // Outside src FOV, ignore
          }
          cv::Mat_<float>& disparity = disparities[i];
          const int xSrc = math_util::clamp(int(std::round(pSrc.x())), 0, disparity.cols - 1);
          const int ySrc = math_util::clamp(int(std::round(pSrc.y())), 0, disparity.rows - 1);
          float depth = pWorld.norm();
          if (depth < FLAGS_min_depth || depth > FLAGS_max_depth) {
            depth = INFINITY;
          }
          disparity(ySrc, xSrc) =
              std::max(disparity(ySrc, xSrc), 1.0f / depth); // get closest value
        }
      }
    });
  }
  threadPool.join();

  return disparities;
}

void saveImages(const std::vector<cv::Mat_<float>>& disparities, const Camera::Rig& rig) {
  LOG(INFO) << "Saving images...";
  const filesystem::path dirOut = filesystem::path(FLAGS_output);
  for (ssize_t i = 0; i < ssize(rig); ++i) {
    const filesystem::path fn = dirOut / rig[i].id / "000000.png";
    filesystem::create_directories(fn.parent_path());
    cv_util::imwriteExceptionOnFail(fn, cv_util::convertTo<uint16_t>(disparities[i]));
  }
}

int main(int argc, char** argv) {
  gflags::SetUsageMessage(kUsage);
  system_util::initDep(argc, argv);

  CHECK_NE(FLAGS_rig, "");
  Camera::Rig rig = filterDestinations(Camera::loadRig(FLAGS_rig), FLAGS_cameras);

  verifyInputs(rig);
  rescaleCameras(rig);
  const int pointCount = getPointCount(FLAGS_point_cloud);
  const PointCloud points = extractPoints(FLAGS_point_cloud, pointCount, FLAGS_threads);
  const std::vector<cv::Mat_<float>> disparities = projectPointsToCameras(points, rig);
  saveImages(disparities, rig);

  return EXIT_SUCCESS;
}
