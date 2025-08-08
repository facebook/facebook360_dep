/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/rig/AlignPointCloud.h"

#include <boost/algorithm/string/split.hpp>
#include <fmt/format.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/FileUtil.h>
#include <folly/Format.h>
#include <folly/dynamic.h>
#include <folly/json.h>

#include "source/calibration/Calibration.h"
#include "source/calibration/FeatureDetector.h"
#include "source/calibration/FeatureMatcher.h"
#include "source/calibration/MatchCorners.h"
#include "source/conversion/PointCloudUtil.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;
using namespace fb360_dep::calibration;
using namespace fb360_dep::point_cloud_util;

using Image = cv::Mat_<uint8_t>;

const std::string kUsageMessage = R"(
  - Aligns point cloud to camera rig. The transformation includes translation, rotation and scaling.

  - Example:
    ./AlignPointCloud \
    --color=/path/to/background/color \
    --point_cloud=/path/to/lidar/points.pts \
    --rig_in=/path/to/rigs/rig.json \
    --rig_out=/path/to/rigs/rig_aligned.json
  )";

DEFINE_string(cameras, "", "subset of cameras to use for aligment (comma-separated)");
DEFINE_string(debug_dir, "", "path to debug output");
DEFINE_double(lidar_match_score, 0.85, "minimum score for an accepted lidar match");
DEFINE_bool(lock_rotation, false, "don't rotate the rig");
DEFINE_bool(lock_scale, false, "don't scale the rig");
DEFINE_bool(lock_translation, false, "don't translate the rig");
DEFINE_double(outlier_factor, 5, "reject if error is factor * median");
DEFINE_string(point_cloud, "", "path to the point cloud file (required)");

using FeatureList = std::vector<Match3D>;

void saveDisparityImages(
    const Camera::Rig& rig,
    const std::vector<PointCloudProjection>& projectedPointClouds,
    const filesystem::path& outputDir) {
  filesystem::create_directories(outputDir);
  for (ssize_t i = 0; i < ssize(rig); ++i) {
    const std::string imageFilename = fmt::format("{}/{}.tif", outputDir.string(), rig[i].id);
    cv_util::imwriteExceptionOnFail(imageFilename, projectedPointClouds[i].disparityImage);
  }
}

void saveDebugImages(
    const Camera::Rig& rig,
    const std::vector<PointCloudProjection>& projectedPointClouds,
    const filesystem::path& outputDir) {
  filesystem::create_directories(outputDir);
  for (ssize_t i = 0; i < ssize(rig); ++i) {
    const std::string imageFilename = fmt::format("{}/{}.tif", outputDir.string(), rig[i].id);
    cv_util::imwriteExceptionOnFail(imageFilename, projectedPointClouds[i].image);
  }
}

FeatureList createFeatureList(
    const std::vector<Keypoint>& imageCorners,
    const std::vector<Keypoint>& lidarCorners,
    const Overlap& overlap,
    const std::string& camId,
    const cv::Mat_<cv::Point3f>& coordinateImage) {
  FeatureList features;
  for (const Match& match : overlap.matches) {
    if (match.score >= FLAGS_lidar_match_score) {
      Match3D match3D;

      match3D.coords = imageCorners[match.corners[0]].coords;
      match3D.lidarCoords = lidarCorners[match.corners[1]].coords;

      const cv::Point3f point = coordinateImage(match3D.lidarCoords.y(), match3D.lidarCoords.x());
      match3D.point.x() = point.x;
      match3D.point.y() = point.y;
      match3D.point.z() = point.z;

      match3D.score = match.score;

      // Only save matches that coorrespond to non-empty points
      if (point.x != 0 || point.y != 0 || point.z != 0) {
        features.emplace_back(match3D);
      }
    }
  }
  return features;
}

void saveLidarMatches(
    const Camera::Rig& rig,
    std::vector<FeatureList> allFeatures,
    const filesystem::path& outputDir) {
  filesystem::create_directories(outputDir);
  for (ssize_t i = 0; i < ssize(rig); ++i) {
    folly::dynamic allMatches = folly::dynamic::array;
    for (const Match3D& feature : allFeatures[i]) {
      folly::dynamic matchData = folly::dynamic::object;

      folly::dynamic featureData =
          folly::dynamic::object("x", feature.coords.x())("y", feature.coords.y());
      matchData["coords"] = featureData;

      folly::dynamic lidarData =
          folly::dynamic::object("x", feature.lidarCoords.x())("y", feature.lidarCoords.y());
      matchData["lidar_coords"] = lidarData;

      folly::dynamic pointData = folly::dynamic::object("x", feature.point.x())(
          "y", feature.point.y())("z", feature.point.z());
      matchData["point"] = pointData;

      matchData["score"] = feature.score;

      allMatches.push_back(matchData);
    }
    std::string filename = fmt::format("{}/{}.json", outputDir.string(), rig[i].id);
    LOG(INFO) << fmt::format("Saving matches to file: {}", filename);
    CHECK(folly::writeFile(folly::toPrettyJson(allMatches), filename.c_str()));
  }
}

void renderReprojections(
    const Camera::Rig& rig,
    std::vector<FeatureList> allFeatures,
    const filesystem::path& outputDir) {
  filesystem::create_directories(outputDir);
  const std::vector<cv::Mat_<cv::Vec3w>> images =
      image_util::loadImages<cv::Vec3w>(FLAGS_color, rig, FLAGS_frame, FLAGS_threads);

  const cv::Scalar green(0, 1, 0);
  const cv::Scalar red(0, 0, 1);
  for (int i = 0; i < int(rig.size()); ++i) {
    for (const Match3D& feature : allFeatures[i]) {
      // draw red line from image feature to reprojected world point
      // then continue in green in the same direction but scale x as far
      Camera::Vector2 proj = rig[i].pixel(feature.point);
      cv::line(
          images[i],
          cv::Point2f(proj.x(), proj.y()),
          cv::Point2f(feature.coords.x(), feature.coords.y()),
          green,
          2);
    }
    std::string errorsFile = fmt::format("{}/{}.png", outputDir.string(), rig[i].id);
    cv_util::imwriteExceptionOnFail(errorsFile, images[i]);
  }
}

std::vector<FeatureList> generateFeatures(const Camera::Rig& rig, const PointCloud& pointCloud) {
  LOG(INFO) << "Loading images";
  const std::vector<Image> images = loadChannels(rig);

  const std::vector<PointCloudProjection>& projectedPointClouds =
      generateProjectedImages(pointCloud, rig);

  if (!FLAGS_debug_dir.empty()) {
    const filesystem::path initialProjectionDir =
        filesystem::path(FLAGS_debug_dir) / "initial_projections";
    saveDebugImages(rig, projectedPointClouds, initialProjectionDir);
    const filesystem::path initialDisparityDir =
        filesystem::path(FLAGS_debug_dir) / "initial_disparities";
    saveDisparityImages(rig, projectedPointClouds, initialDisparityDir);
  }

  // For every camera
  std::vector<FeatureList> allFeatures;
  for (ssize_t i = 0; i < ssize(rig); ++i) {
    bool useNearest = false; // enable bilinear interpolation on the camera image
    std::vector<Keypoint> imageCorners = findCorners(rig[i], images[i], useNearest);

    Camera lidarCamera = rig[i];
    const Image& lidarImage = extractSingleChannelImage(projectedPointClouds[i].image);

    lidarCamera.id = fmt::format("{}_lidar", rig[i].id);
    useNearest = true; // don't interpolate the lidar projection
    std::vector<Keypoint> lidarCorners = findCorners(lidarCamera, lidarImage, useNearest);

    Overlap overlap =
        findMatches(images[i], imageCorners, rig[i], lidarImage, lidarCorners, lidarCamera);
    LOG(INFO) << fmt::format("Found {} matches", overlap.matches.size());

    FeatureList camFeatures = createFeatureList(
        imageCorners, lidarCorners, overlap, rig[i].id, projectedPointClouds[i].coordinateImage);
    allFeatures.emplace_back(camFeatures);
  }
  return allFeatures;
}

void solve(ceres::Problem& problem) {
  ceres::Solver::Options options;
  options.use_inner_iterations = true;
  options.max_num_iterations = 500;
  options.minimizer_progress_to_stdout = false;
  ceres::Solver::Summary summary;

  Solve(options, &problem, &summary);
  LOG(INFO) << summary.BriefReport();
}

double calcPercentile(std::vector<double> values, double percentile = 0.5) {
  if (values.empty()) {
    return NAN;
  }
  CHECK_LT(percentile, 1);
  size_t index(percentile * values.size());
  std::nth_element(values.begin(), values.begin() + index, values.end());
  return values[index];
}

void logMedianErrors(const Camera::Rig& rig, const std::vector<FeatureList>& allFeatures) {
  std::vector<std::vector<Camera::Real>> errors;

  for (int i = 0; i < int(rig.size()); ++i) {
    std::vector<Camera::Real> cameraErrors;
    for (const auto& feature : allFeatures[i]) {
      Camera::Real residual = (rig[i].pixel(feature.point) - feature.coords).norm();
      cameraErrors.push_back(residual);
    }
    errors.push_back(cameraErrors);
  }

  // compute median for each camera
  std::vector<Camera::Real> medians(ssize(errors));
  for (ssize_t i = 0; i < ssize(errors); ++i) {
    medians[i] = calcPercentile(errors[i]);
    LOG(INFO) << fmt::format(
        "{} median: {} 25%: {} 90%: {} 95%: {}",
        rig[i].id,
        calcPercentile(errors[i]),
        calcPercentile(errors[i], 0.25),
        calcPercentile(errors[i], 0.90),
        calcPercentile(errors[i], 0.95));
  }
}

std::vector<FeatureList> removeOutliers(
    const Camera::Rig& rig,
    const std::vector<FeatureList>& allFeatures) {
  std::vector<FeatureList> inlyingFeatures;
  for (ssize_t i = 0; i < ssize(rig); ++i) {
    FeatureList cameraFeatures;
    std::vector<Camera::Real> cameraErrors;
    for (const Match3D& feature : allFeatures[i]) {
      const Camera::Real residual = (rig[i].pixel(feature.point) - feature.coords).norm();
      cameraErrors.push_back(residual);
    }
    const double median = calcPercentile(cameraErrors);
    LOG(INFO) << fmt::format("Median {} {}", rig[i].id, median);
    for (ssize_t j = 0; j < ssize(cameraErrors); ++j) {
      if (cameraErrors[j] < FLAGS_outlier_factor * median) {
        cameraFeatures.push_back(allFeatures[i][j]);
      }
    }

    LOG(INFO) << fmt::format(
        "{} median unfiltered: {} outlier threshold: {} unfiltered match count: {} accepted matches count: {}",
        rig[i].id,
        median,
        FLAGS_outlier_factor * median,
        cameraErrors.size(),
        cameraFeatures.size());
    inlyingFeatures.push_back(cameraFeatures);
  }

  return inlyingFeatures;
}

Camera::Rig alignPointCloud(
    const Camera::Rig& rig,
    const std::string& includeCamList,
    const std::vector<FeatureList>& allFeatures,
    bool lockRotation = false,
    bool lockTranslation = false,
    bool lockScale = false) {
  ceres::Problem problem;
  Camera::Vector3 rotation(0, 0, 0);
  Camera::Vector3 translation(0, 0, 0);
  Eigen::UniformScaling<double> scale(1);

  const std::vector<FeatureList>& inlyingFeatures = removeOutliers(rig, allFeatures);

  logMedianErrors(rig, inlyingFeatures);

  std::vector<std::string> includeCams;
  boost::split(includeCams, includeCamList, [](char c) { return c == ','; });

  int alignmentCameras = 0;
  for (int i = 0; i < int(rig.size()); ++i) {
    if (!includeCamList.empty() &&
        std::find(includeCams.begin(), includeCams.end(), rig[i].id) == includeCams.end()) {
      LOG(INFO) << fmt::format("Excluding camera {} from calibration ", rig[i].id);
      continue;
    }
    alignmentCameras++;
    for (const Match3D& feature : inlyingFeatures[i]) {
      PointCloudFunctor::addResidual(problem, rotation, translation, scale, rig[i], feature);
    }
  }

  if (alignmentCameras == 1) {
    LOG(INFO) << "Single camera aligment detected. Locking rig scale to 1.";
    lockScale = true;
  }

  problem.SetParameterLowerBound(&scale.factor(), 0, 0.25);
  problem.SetParameterLowerBound(rotation.data(), 0, -M_PI);
  problem.SetParameterLowerBound(rotation.data(), 1, -M_PI);
  problem.SetParameterLowerBound(rotation.data(), 2, -M_PI / 2);
  problem.SetParameterUpperBound(rotation.data(), 0, M_PI);
  problem.SetParameterUpperBound(rotation.data(), 1, M_PI);
  problem.SetParameterUpperBound(rotation.data(), 2, M_PI / 2);

  if (lockRotation) {
    problem.SetParameterBlockConstant(rotation.data());
  }
  if (lockTranslation) {
    problem.SetParameterBlockConstant(translation.data());
  }
  if (lockScale) {
    problem.SetParameterBlockConstant(&scale.factor());
  }

  solve(problem);

  LOG(INFO) << fmt::format("New rotation values: {} {} {}", rotation[0], rotation[1], rotation[2]);
  LOG(INFO) << fmt::format(
      "New translation values: {} {} {}", translation[0], translation[1], translation[2]);
  LOG(INFO) << fmt::format("New scale: {}", scale.factor());
  const Camera::Rig transformedRig = transformRig(rig, rotation, translation, scale);

  logMedianErrors(transformedRig, inlyingFeatures);

  return transformedRig;
}

int main(int argc, char* argv[]) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_rig_in, "");
  CHECK_NE(FLAGS_color, "");
  CHECK_NE(FLAGS_point_cloud, "");
  CHECK_NE(FLAGS_rig_out, "");

  // Read in the rig and reference rig
  LOG(INFO) << "Loading the cameras";
  const Camera::Rig rig =
      image_util::filterDestinations(Camera::loadRig(FLAGS_rig_in), FLAGS_cameras);

  const int validFrame = image_util::getSingleFrame(FLAGS_color, rig, FLAGS_frame);
  FLAGS_frame = image_util::intToStringZeroPad(validFrame);

  LOG(INFO) << "Loading point cloud";
  const PointCloud& pointCloud = extractPoints(FLAGS_point_cloud, FLAGS_threads);

  std::vector<FeatureList> allFeatures = generateFeatures(rig, pointCloud);

  if (!FLAGS_debug_dir.empty()) {
    const filesystem::path matchesDir = filesystem::path(FLAGS_debug_dir) / "matches";
    saveLidarMatches(rig, allFeatures, matchesDir);
    const filesystem::path initialReprojectionDir =
        filesystem::path(FLAGS_debug_dir) / "initial_reprojections";
    renderReprojections(rig, allFeatures, initialReprojectionDir);
  }

  const Camera::Rig& transformedRig = alignPointCloud(
      rig,
      FLAGS_cameras,
      allFeatures,
      FLAGS_lock_rotation,
      FLAGS_lock_translation,
      FLAGS_lock_scale);

  if (!FLAGS_debug_dir.empty()) {
    const filesystem::path finalReprojectionDir =
        filesystem::path(FLAGS_debug_dir) / "final_reprojections";
    renderReprojections(transformedRig, allFeatures, finalReprojectionDir);

    const filesystem::path finalProjectionDir =
        filesystem::path(FLAGS_debug_dir) / "final_projections";
    const std::vector<PointCloudProjection>& projectedPointClouds =
        generateProjectedImages(pointCloud, transformedRig);
    saveDebugImages(transformedRig, projectedPointClouds, finalProjectionDir);

    const filesystem::path finalDisparityDir =
        filesystem::path(FLAGS_debug_dir) / "final_disparities";
    saveDisparityImages(rig, projectedPointClouds, finalDisparityDir);
  }

  Camera::saveRig(FLAGS_rig_out, transformedRig);

  return 0;
}
