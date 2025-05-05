/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

const char* kUsage = R"(
  - Reads a set of color and disparity images and produces an ascii file with a
  single point per line

  Each line contains "x y z 1 r g b", where
  - x y z is the position (in meters)
  - r g b is the color (0..255)

  The format can be imported as a .txt into meshlab with File -> Import Mesh
  set Separator to "SPACE" and set Point format to "X Y Z Reflectance R G B"

  - Example:
    ./ExportPointCloud \
    --output=/path/to/video/output \
    --color=/path/to/video/color \
    --disparity=/path/to/output/disparity \
    --rig=/path/to/rigs/rig.json \
    --frame=000000
)";

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <fstream>

#include "source/util/Camera.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"
#include "source/util/ThreadPool.h"

using namespace fb360_dep;
using namespace fb360_dep::image_util;

DEFINE_string(cameras, "", "comma-separated cameras to render (empty for all)");
DEFINE_bool(clip, false, "points beyond max_depth are clipped, not clamped");
DEFINE_string(color, "", "path to input color images (required)");
DEFINE_string(disparity, "", "path to disparity files (.pfm) (required)");
DEFINE_string(frame, "000000", "frame to process (lexical)");
DEFINE_bool(header_count, true, "add point count to the start of the file");
DEFINE_double(max_depth, INFINITY, "depth is clamped to this value (m). Use e.g. 20 to visualize");
DEFINE_string(output, "", "output filename (required)");
DEFINE_string(rig, "", "path to camera rig .json (required)");
DEFINE_int32(subsample, 1, "how often we sample (>= 1)");
DEFINE_int32(threads, -1, "number of threads (-1 = auto, 0 = none)");

// World coordinate (xyz) and corresponding color (rgb)
using WorldColor = Eigen::Array<float, 6, 1>;

void verifyInputs(const Camera::Rig& rig) {
  CHECK_NE(FLAGS_threads, 0);
  CHECK_NE(FLAGS_color, "");
  CHECK_NE(FLAGS_disparity, "");
  CHECK_NE(FLAGS_output, "");

  verifyImagePaths(FLAGS_color, rig, FLAGS_frame, FLAGS_frame);
  verifyImagePaths(FLAGS_disparity, rig, FLAGS_frame, FLAGS_frame, ".pfm");
}

std::vector<WorldColor> getPoints(const Camera& cam) {
  const std::string& camId = cam.id;
  LOG(INFO) << folly::sformat("Processing camera {}...", camId);

  // Load disparity and color, and resize color to size of disparity
  const cv::Mat_<float> disparity = loadImage<float>(FLAGS_disparity, camId, FLAGS_frame);

  // Load color (resize it to match disparity)
  const cv::Mat_<cv::Vec3f> color =
      loadResizedImage<cv::Vec3f>(FLAGS_color, camId, FLAGS_frame, disparity.size());

  // Rescale camera as disparity may not be full resolution
  const int w = disparity.cols;
  const int h = disparity.rows;
  const Camera camRescale = cam.rescale({w, h});

  // Transform each pixel to world coordinates and append to result
  std::vector<WorldColor> points(w * h);
  std::vector<bool> idxToDelete(w * h, false);

  ThreadPool threadPool(FLAGS_threads);
  const int edgeX = w;
  const int edgeY = 1;
  for (int yBegin = 0; yBegin < h; yBegin += edgeY) {
    for (int xBegin = 0; xBegin < w; xBegin += edgeX) {
      const int xEnd = std::min(xBegin + edgeX, w);
      const int yEnd = std::min(yBegin + edgeY, h);
      threadPool.spawn([&, xBegin, yBegin, xEnd, yEnd] {
        for (int y = yBegin; y < yEnd; ++y) {
          for (int x = xBegin; x < xEnd; ++x) {
            const int idx = w * y + x;
            if (FLAGS_subsample > 1 && rand() % FLAGS_subsample != 0) {
              idxToDelete[idx] = true;
              continue; // only retain 1 in subsample points
            }
            Camera::Vector2 pixel = {x + 0.5, y + 0.5};
            if (camRescale.isOutsideImageCircle(pixel)) {
              idxToDelete[idx] = true;
              continue;
            }
            WorldColor worldColor;
            const double m = 1 / disparity(y, x);
            Camera::Vector3 world = camRescale.rig(pixel, m);
            const Camera::Real depth = world.norm();
            if (depth > FLAGS_max_depth) {
              if (FLAGS_clip) {
                idxToDelete[idx] = true;
                continue;
              }
              world *= FLAGS_max_depth / depth;
            }
            worldColor.head<3>() = world.cast<float>();
            const cv::Vec3f c = color(y, x);
            worldColor.tail<3>() = Eigen::Array3f(c[2], c[1], c[0]);
            points[y * w + x] = worldColor;
          }
        }
      });
    }
  }
  threadPool.join();

  std::vector<WorldColor> pointsFiltered;
  for (ssize_t i = 0; i < ssize(points); ++i) {
    if (!idxToDelete[i]) {
      pointsFiltered.push_back(points[i]);
    }
  }

  return pointsFiltered;
}

void writePoints(
    std::ofstream& file,
    const std::vector<std::vector<WorldColor>>& pointClouds,
    const int lines) {
  LOG(INFO) << fmt::format("Writing {} points to file...", lines);

  // Merge all points
  std::vector<WorldColor> pointsAll;
  pointsAll.reserve(lines); // preallocate memory
  for (const std::vector<WorldColor>& points : pointClouds) {
    pointsAll.insert(pointsAll.end(), points.begin(), points.end());
  }

  ThreadPool threadPool(FLAGS_threads);
  const int threads = threadPool.getMaxThreads();
  const int pointsPerThread = std::ceil((float)pointsAll.size() / threads);

  std::vector<std::stringstream> filestreams(threads);
  for (int i = 0; i < threads; ++i) {
    const int start = i * pointsPerThread;
    const int end = std::min((i + 1) * pointsPerThread - 1, int(pointsAll.size()) - 1);
    threadPool.spawn([&, i, start, end] {
      for (int j = start; j <= end; ++j) {
        // A line in a pts file represents x y z "intensity" r g b
        // x y z are in meters, we arbitrarily set "intensity" to 1, and rgb is between 0 and 255
        const WorldColor& point = pointsAll[j];
        filestreams[i] << fmt::format("{} {} {} 1 ", point[0], point[1], point[2]);
        filestreams[i] << fmt::format(
            "{:.0f} {:.0f} {:.0f}\n", 255 * point[3], 255 * point[4], 255 * point[5]);
      }
      // filestreams[i] = std::move(fileI);
    });
  }
  threadPool.join();

  LOG(INFO) << "Merging files...";
  for (const auto& filestream : filestreams) {
    file << filestream.rdbuf();
  }
}

int main(int argc, char** argv) {
  gflags::SetUsageMessage(kUsage);
  system_util::initDep(argc, argv);

  CHECK_NE(FLAGS_rig, "");
  const Camera::Rig rig = filterDestinations(Camera::loadRig(FLAGS_rig), FLAGS_cameras);

  verifyInputs(rig);

  std::vector<std::vector<WorldColor>> pointClouds;
  for (const Camera& cam : rig) {
    pointClouds.push_back(getPoints(cam));
  }

  const filesystem::path fnOut = filesystem::path(FLAGS_output);
  filesystem::create_directories(fnOut.parent_path());
  std::ofstream file(fnOut.string());
  CHECK(file.is_open()) << folly::sformat("Cannot open file for writing: {}", fnOut.string());

  int lines = 0;
  for (const std::vector<WorldColor>& pointCloud : pointClouds) {
    lines += pointCloud.size();
  }

  if (FLAGS_header_count) {
    file << fmt::format("{}\n", lines);
  }

  writePoints(file, pointClouds, lines);

  LOG(INFO) << fmt::format("{} lines written", lines);

  return EXIT_SUCCESS;
}
