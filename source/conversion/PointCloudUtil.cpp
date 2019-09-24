/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/conversion/PointCloudUtil.h"

#include <regex>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/lexical_cast.hpp>

#include "source/util/ThreadPool.h"

using Image = cv::Mat_<cv::Vec3b>;
using DisparityImage = cv::Mat_<float>;
using CoordinateImage = cv::Mat_<cv::Point3f>;

namespace fb360_dep {
namespace point_cloud_util {

std::vector<PointCloudProjection> generateProjectedImages(
    const PointCloud& pointCloud,
    const Camera::Rig& rig) {
  std::vector<PointCloudProjection> projections;
  for (const auto& camera : rig) {
    PointCloudProjection projection;
    projection.image =
        cv::Mat(camera.resolution[1], camera.resolution[0], CV_8UC3, cv::Scalar(0, 0, 0));
    projection.disparityImage =
        cv::Mat(camera.resolution[1], camera.resolution[0], CV_32F, cv::Scalar(0));
    projection.coordinateImage =
        cv::Mat(camera.resolution[1], camera.resolution[0], CV_32FC3, cv::Scalar(0, 0, 0));
    projections.emplace_back(projection);
  }

  for (const auto& point : pointCloud) {
    for (int i = 0; i < int(rig.size()); ++i) {
      if (rig[i].sees(point.coords)) {
        const Camera::Vector2& projectedCoors = rig[i].pixel(point.coords);
        PointCloudProjection& projection = projections[i];
        const float depth = (point.coords - rig[i].position).norm();
        const float disparity = 1.0f / depth;
        if (projection.disparityImage(projectedCoors.y(), projectedCoors.x()) < disparity) {
          projection.disparityImage(projectedCoors.y(), projectedCoors.x()) = disparity;
          projection.image(projectedCoors.y(), projectedCoors.x()) = point.bgrColor;
          projection.coordinateImage(projectedCoors.y(), projectedCoors.x()).x = point.coords.x();
          projection.coordinateImage(projectedCoors.y(), projectedCoors.x()).y = point.coords.y();
          projection.coordinateImage(projectedCoors.y(), projectedCoors.x()).z = point.coords.z();
        }
      }
    }
  }
  return projections;
}

void goToLine(std::ifstream& file, const int lineNumber) {
  file.seekg(0, std::ios::beg);
  for (int i = 1; i < lineNumber; ++i) {
    file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }
}

// PCL header entries must be specified in the following order
// Comment
// VERSION
// FIELDS
// SIZE
// TYPE
// COUNT
// WIDTH
// HEIGHT
// VIEWPOINT
// POINTS
// DATA
void verifyPCLHeader(std::ifstream& file) {
  std::string line;

  // Make sure FIELDS starts with x y z
  const int lineFields = 3;
  goToLine(file, lineFields);
  std::getline(file, line);
  CHECK(boost::starts_with(line, "FIELDS x y z")) << "PCL header: FIELDS must start with x y z";

  // Make DATA is ascii
  const int lineData = 11;
  goToLine(file, lineData);
  std::getline(file, line);
  CHECK_EQ(line, "DATA ascii") << "PCL header: DATA must be ascii";
}

int extractPCLPointCount(std::ifstream& file) {
  goToLine(file, 0);

  std::string line;
  const int linePoints = 10;
  goToLine(file, linePoints);
  std::getline(file, line);
  CHECK(boost::starts_with(line, "POINTS"))
      << folly::sformat("PCL header: expected point count in line {}, got {}", linePoints, line);

  std::regex rgx("POINTS (\\w+)");
  std::smatch match;
  const std::string lineConst = line;
  CHECK(std::regex_search(lineConst.begin(), lineConst.end(), match, rgx))
      << "Could not parse point count from line " << line;
  const std::string pointCountStr = match[1];
  try {
    return boost::lexical_cast<int>(pointCountStr);
  } catch (boost::bad_lexical_cast&) {
    CHECK(false) << folly::sformat("PCL header: invalid point count {}", pointCountStr);
  }
}

int extractASCIIPointCount(std::ifstream& file) {
  goToLine(file, 0);

  std::string line;
  file >> line;
  try {
    return boost::lexical_cast<int>(line);
  } catch (boost::bad_lexical_cast&) {
    CHECK(false) << "First line should contain point count: " << line;
  }
}

int getPointCount(const std::string& pointCloudFile) {
  // Check if first line is point counter
  std::ifstream srcFile(pointCloudFile);
  CHECK(srcFile.good()) << "File does not exist: " << pointCloudFile;

  const std::string pcExt = filesystem::path(pointCloudFile).extension().string();
  if (pcExt == ".pcd") {
    verifyPCLHeader(srcFile);
    return extractPCLPointCount(srcFile);
  } else {
    return extractASCIIPointCount(srcFile);
  }
}

PointCloud
extractPoints(const std::string& pointCloudFile, const int pointCount, const int maxThreads) {
  LOG(INFO) << folly::sformat("Extracting {} points from {}...", pointCount, pointCloudFile);

  ThreadPool threadPool(maxThreads);
  const int threads = threadPool.getMaxThreads();

  // Evenly distribute lines across threads
  const int pointsPerThread = float(pointCount) / threads;
  const int remain = pointCount % threads;

  const std::string pcExt = filesystem::path(pointCloudFile).extension().string();
  const int headerNumLines = pcExt == ".pcd" ? 11 : 1;

  PointCloud points(pointCount);
  for (int i = 0; i < threads; ++i) {
    threadPool.spawn([&, i] {
      const int begin = headerNumLines + 1 + // skip header
          (i < remain ? i * (pointsPerThread + 1) : pointCount - (threads - i) * pointsPerThread);
      const int end = begin + pointsPerThread + (i < remain);

      // Lines may not have the same length in characters, we have to loop our way to the start
      // line
      std::ifstream file(pointCloudFile);
      goToLine(file, begin);

      for (int j = begin; j < end; ++j) {
        // Read XYZ coordinates and discard the rest of the line
        BGRPoint& point = points[j];
        int ignored, r, g, b;
        file >> point.coords.x() >> point.coords.y() >> point.coords.z() >> ignored >> r >> g >> b;
        point.bgrColor[0] = b;
        point.bgrColor[1] = g;
        point.bgrColor[2] = r;
        file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
      }
    });
  }
  threadPool.join();

  LOG(INFO) << folly::sformat("Extracted {} points.", points.size());
  if (pointCount > 0) {
    CHECK_EQ(pointCount, points.size()) << folly::sformat(
        "Point count in header ({}) does not match number of extracted points ({})",
        pointCount,
        points.size());
  }

  return points;
}

PointCloud extractPoints(const std::string& pointCloudFile, const int maxThreads) {
  const int pointCount = getPointCount(pointCloudFile);
  return extractPoints(pointCloudFile, pointCount, maxThreads);
}

} // namespace point_cloud_util
} // namespace fb360_dep
