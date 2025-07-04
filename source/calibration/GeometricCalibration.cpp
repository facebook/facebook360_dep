/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/calibration/GeometricCalibration.h"

#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <unordered_map>

#include <boost/algorithm/string.hpp>
#include <boost/timer/timer.hpp>
#include <fmt/format.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <opencv2/opencv.hpp>

#include <folly/FileUtil.h>
#include <folly/Format.h>
#include <folly/dynamic.h>
#include <folly/json.h>

#include "source/calibration/Calibration.h"
#include "source/util/Camera.h"
#include "source/util/CvUtil.h"
#include "source/util/MathUtil.h"
#include "source/util/ThreadPool.h"

using namespace fb360_dep;
using namespace fb360_dep::calibration;

DEFINE_int32(cap_traces, 0, "speed up solver by capping the number of traces");
DEFINE_double(ceres_function_tolerance, 1e-6, "ceres function tolerance");
DEFINE_int32(
    ceres_threads,
    -1,
    "number of threads used by ceres. requires compiled support for multithreading (default 1)");
DEFINE_string(debug_dir, "", "path to debug output");
DEFINE_double(debug_error_scale, 0, "show scaled reprojection errors");
DEFINE_double(debug_matches_overlap, 1, "show matches if overlap exceeds this fraction");
DEFINE_bool(
    dir_per_frame,
    false,
    "is there a directory per frame?\n"
    "i.e. is an image path of the form: \n"
    "    <frame index>/ ... /<camera id>.<extension>\n"
    "    e.g. 1/cam2.bmp or 000001/isp_out/cam14.png\n"
    "the default is a directory per camera. i.e. an image is of the form:\n"
    "    .../<camera id>/<frame index>.<extension>\n"
    "    e.g. cam2/123.bmp or rgb/cam14/000123.png");
DEFINE_bool(discard_outside_fov, true, "discard matches outside fov");
DEFINE_string(errors_dir, "", "directory where errors will be saved");
DEFINE_int32(experiments, 1, "calibrate multiple times");
DEFINE_bool(force_in_front, true, "no intersections behind camera");
DEFINE_bool(keep_invalid_traces, false, "keep traces with multiple points from the same camera");
DEFINE_bool(lock_distortion, true, "lock the distorion");
DEFINE_bool(lock_focal, false, "lock the focal");
DEFINE_bool(lock_positions, true, "don't calibrate position");
DEFINE_bool(lock_principals, false, "don't calibrate principals");
DEFINE_bool(lock_rotations, false, "don't calibrate rotation");
DEFINE_double(max_error, 0.5, "maximum allowable error for calibration to be valid");
DEFINE_int32(min_traces, 10, "minimum number of traces for camera to be sufficiently constrained");
DEFINE_double(outlier_factor, 5, "reject if error is factor * median");
DEFINE_double(
    outlier_z_threshold,
    3,
    "z score threshold on traces to consider a camera an outlier");
DEFINE_int32(pass_count, 10, "number of passes");
DEFINE_double(perturb_focals, 0, "pertub focals (pixels / radian)");
DEFINE_double(perturb_positions, 0, "perturb positions (m)");
DEFINE_double(perturb_principals, 0, "pertub principals (pixels)");
DEFINE_double(perturb_rotations, 0, "perturb rotations (radians)");
DEFINE_int32(point_count, 10000, "artificial points to generate");
DEFINE_double(point_error_stddev, 0.5, "error added to artificial points");
DEFINE_double(point_min_dist, 1, "minimum distance of artificial points");
DEFINE_string(points_file, "", "path to output calibration points file, default next to output");
DEFINE_string(
    points_file_json,
    "",
    "path to output calibration points file including reference points, default next to output");
DEFINE_string(reference_camera, "", "reference camera to lock if positions are unlocked");
DEFINE_double(
    remove_sparse_overlaps,
    0,
    "reject overlaps with fewer than this fraction of the average match count");
DEFINE_bool(report_per_camera_errors, false, "per camera reprojection error statistics");
DEFINE_bool(robust, true, "use Huber loss function");
DEFINE_int32(seed, -1, "seed for random number generator");
DEFINE_bool(shared_distortion, true, "all cameras in a group share the same distortion");
DEFINE_bool(
    shared_principal_and_focal,
    false,
    "all cameras in a group share the same focal, principal");
DEFINE_bool(
    weight_by_trace_count,
    false,
    "weight the residual error by the number of traces per camera");
DEFINE_bool(weighted_statistics, false, "compute statistics of weighted residuals");

std::unordered_map<std::string, int> cameraIdToIndex;
std::unordered_map<std::string, int> cameraGroupToIndex;

std::string imageIdFormat() {
  return FLAGS_dir_per_frame ? "<frame index>/ ... /<camera id>.<extension>"
                             : ".../<camera id>/<frame index>.<extension>";
}

void buildCameraIndexMaps(const Camera::Rig& rig) {
  for (int i = 0; i < int(rig.size()); ++i) {
    cameraIdToIndex[rig[i].id] = i;
    cameraGroupToIndex[rig[i].group] = i; // last camera in group wins
  }
}

using ImageId = std::string;

std::string getCameraId(const ImageId& image) {
  // image is actually a path
  filesystem::path path = image;
  if (FLAGS_dir_per_frame) {
    return path.stem().native();
  }
  return path.parent_path().filename().native();
}

int getFrameIndex(const ImageId& image) {
  // image is actually a path
  filesystem::path path = image;
  if (FLAGS_dir_per_frame) {
    return std::stoi(path.begin()->native());
  }
  return std::stoi(path.stem().native());
}

bool hasCameraIndex(const ImageId& image) {
  return cameraIdToIndex.count(getCameraId(image));
}

int getCameraIndex(const ImageId& image) {
  return cameraIdToIndex.at(getCameraId(image));
}

// create a string that adheres to the format of an image path
ImageId makeArtificialPath(int frame, const std::string& cameraId) {
  if (FLAGS_dir_per_frame) {
    return std::to_string(frame) + "/" + cameraId;
  }
  return cameraId + "/" + std::to_string(frame);
}

// input path includes basename and extension
cv::Mat_<cv::Vec3w> loadImage(const filesystem::path& path) {
  const filesystem::path colorDir = FLAGS_color;
  return cv_util::loadImage<cv::Vec3w>(colorDir / path);
}

folly::dynamic parseJsonFile(const std::string& path) {
  std::string json;
  folly::readFile(path.c_str(), json);
  CHECK(!json.empty()) << "could not read JSON file: " << path;
  return folly::parseJson(json);
}

// a feature is a point in an image
struct Feature {
  Camera::Vector2 position; // position of the feature in its image, in pixels
  int trace; // index of trace that feature belongs to (or -1 if none)

  Feature(const Camera::Vector2& position) : position(position), trace(-1) {}
};

// a featuremap holds, for each image, a vector of its features
using FeatureMap = std::unordered_map<ImageId, std::vector<Feature>>;

// an overlap is a pair of images and the matches between their features
struct Overlap {
  std::array<ImageId, 2> images;
  std::vector<std::array<size_t, 2>> matches;

  Overlap(const ImageId& image0, const ImageId& image1) {
    images[0] = image0;
    images[1] = image1;
  }

  bool isIntraFrame() const {
    return getFrameIndex(images[0]) == getFrameIndex(images[1]);
  }
};

// a trace is a world coordinate and a list of observations that reference it
struct Trace {
  Camera::Vector3 position;

  std::vector<std::pair<ImageId, int>> references;

  void add(const ImageId& image, const int index) {
    references.emplace_back(image, index);
  }

  // inherit trace's references
  void inherit(Trace& trace, FeatureMap& featureMap, int me) {
    for (const auto& ref : trace.references) {
      featureMap[ref.first][ref.second].trace = me;
    }
    references.insert(references.end(), trace.references.begin(), trace.references.end());
    trace.references.clear();
  }

  void clear(FeatureMap& featureMap) {
    for (const auto& ref : references) {
      featureMap[ref.first][ref.second].trace = -1;
    }
    references.clear();
  }
};

/* Parse features from <parsed> JSON which has structure
    {
        "images": {
            image_name: [{"x": x, "y": y, ...}]
        },
        ...
    }
    where image_name is defined as in imageIdFormat, above.
*/
FeatureMap loadFeatureMap(const folly::dynamic& parsed) {
  FeatureMap result;

  for (const auto& image : parsed["images"].items()) {
    const ImageId path = image.first.getString();
    if (!hasCameraIndex(path)) {
      LOG(INFO) << fmt::format("ignoring image id {}", path);
      continue;
    }
    std::vector<Feature>& features = result[path];
    for (const auto& feature : image.second) {
      features.emplace_back(Camera::Vector2(feature["x"].asDouble(), feature["y"].asDouble()));
    }
  }

  CHECK(!result.empty()) << "verify image id format: " << imageIdFormat();
  LOG(INFO) << fmt::format("{} images loaded", result.size());

  return result;
}

/* Parse matches from <parsed> JSON which has structure
    {
        "all_matches": [{
            "image1": image_name1,
            "image2": image_name1,
            "matches": [{
                "idx1": idx1,
                "idx2": idx2
            }]
        }],
        ...
    }
*/
std::vector<Overlap> loadOverlaps(const folly::dynamic& parsed) {
  std::vector<Overlap> result;

  size_t count = 0;
  for (const auto& overlap : parsed["all_matches"]) {
    ImageId path0 = overlap["image1"].getString();
    ImageId path1 = overlap["image2"].getString();
    if (!hasCameraIndex(path0) || !hasCameraIndex(path1)) {
      continue;
    }
    result.emplace_back(path0, path1);
    for (const auto& match : overlap["matches"]) {
      // A threshold of 0 indicates that score should be ignored
      // Check if score should be ignored before attempting to access
      // match["score"] since it might not be defined in the json
      if (FLAGS_match_score_threshold == 0 ||
          FLAGS_match_score_threshold <= match["score"].asDouble()) {
        result.back().matches.push_back(
            {{size_t(match["idx1"].asInt()), size_t(match["idx2"].asInt())}});
      }
    }
    count += 2 * result.back().matches.size();
  }

  LOG(INFO) << fmt::format("{} feature observations loaded", count);

  return result;
}

Overlap& findOrAddOverlap(std::vector<Overlap>& overlaps, const ImageId& i0, const ImageId& i1) {
  for (Overlap& overlap : overlaps) {
    if (overlap.images[0] == i0 && overlap.images[1] == i1) {
      return overlap;
    }
    // make sure we don't have the image pair (i1, i0) in there
    CHECK(overlap.images[0] != i1 || overlap.images[1] != i0);
  }
  // image pair (i0, i1) not found: add it
  overlaps.emplace_back(i0, i1);
  return overlaps.back();
}

template <typename T>
Camera::Vector2 keypointError(T& gen) {
  std::normal_distribution<> rng(0, FLAGS_point_error_stddev);
  return {rng(gen), rng(gen)};
}

void generateArtificalPoints(
    FeatureMap& featureMap,
    std::vector<Overlap>& overlaps,
    const std::vector<Camera>& cameras) {
  std::mt19937 mt;
  for (int p = 0; p < FLAGS_point_count; ++p) {
    // create a random unit vector
    double longitude = std::uniform_real_distribution<>(-M_PI, +M_PI)(mt);
    double z = std::uniform_real_distribution<>(-1, 1)(mt);
    Camera::Vector3 rig(
        std::sqrt(1 - z * z) * std::cos(longitude), std::sqrt(1 - z * z) * std::sin(longitude), z);
    CHECK_NEAR(rig.squaredNorm(), 1, 0.001);

    // divide unit vector by random disparity
    rig /= std::uniform_real_distribution<>(0, 1 / FLAGS_point_min_dist)(mt);

    // add keypoint to every camera that sees rig
    std::vector<ImageId> images;
    for (const Camera& camera : cameras) {
      if (camera.sees(rig)) {
        ImageId image = makeArtificialPath(0, camera.id);
        featureMap[image].emplace_back(camera.pixel(rig) + keypointError(mt));
        images.push_back(image);
      }
    }

    // add a match for every pair of cameras that see rig
    for (int index1 = 0; index1 < int(images.size()); ++index1) {
      const ImageId& i1 = images[index1];
      for (int index0 = 0; index0 < index1; ++index0) {
        const ImageId& i0 = images[index0];
        Overlap& overlap = findOrAddOverlap(overlaps, i0, i1);
        overlap.matches.push_back({{featureMap[i0].size() - 1, featureMap[i1].size() - 1}});
      }
    }
  }
}

Camera::Vector3 triangulate(const Observations& observations) {
  return triangulateNonlinear(observations, FLAGS_force_in_front);
}

// return reprojection errors for each camera
std::vector<std::vector<Camera::Real>> reprojectionErrors(
    const std::vector<Overlap>& overlaps,
    const FeatureMap& featureMap,
    const std::vector<Trace>& traces,
    const std::vector<Camera>& cameras) {
  std::vector<std::vector<Camera::Real>> errors(ssize(cameras));
  for (const Overlap& overlap : overlaps) {
    if (!overlap.isIntraFrame()) {
      continue;
    }
    const std::reference_wrapper<const ImageId> images[2] = {overlap.images[0], overlap.images[1]};
    const int idxs[2] = {getCameraIndex(images[0]), getCameraIndex(images[1])};
    const std::reference_wrapper<const std::vector<Feature>> features[2] = {
        featureMap.at(images[0]), featureMap.at(images[1])};
    for (const auto& match : overlap.matches) {
      std::array<const Feature, 2> kps = {
          {features[0].get()[match[0]], features[1].get()[match[1]]}};
      CHECK_EQ(kps[0].trace, kps[1].trace) << "matching features belong to different traces";
      Camera::Vector3 rig = kps[0].trace < 0
          ? triangulate({{cameras[idxs[0]], kps[0].position}, {cameras[idxs[1]], kps[1].position}})
          : traces[kps[0].trace].position;
      for (ssize_t i = 0; i < ssize(match); ++i) {
        const Camera::Vector2 pixel = cameras[idxs[i]].pixel(rig);
        errors[idxs[i]].push_back((pixel - kps[i].position).squaredNorm());
      }
    }
  }

  return errors;
}

// report reprojection errors for each camera
void reportReprojectionErrors(
    const std::vector<Overlap>& overlaps,
    const FeatureMap& featureMap,
    const std::vector<Trace>& traces,
    const std::vector<Camera>& cameras) {
  if (!FLAGS_report_per_camera_errors) {
    return;
  }
  std::vector<std::vector<Camera::Real>> errors =
      reprojectionErrors(overlaps, featureMap, traces, cameras);
  for (ssize_t i = 0; i < ssize(cameras); ++i) {
    std::sort(errors[i].begin(), errors[i].end());
    std::ostringstream line;
    for (int percentile : {50, 90, 99}) {
      int index = percentile * (ssize(errors[i]) - 1) / 100.0 + 0.5;
      line << fmt::format("{}%: {:.2f} ", percentile, std::sqrt(errors[i][index]));
    }
    LOG(INFO) << fmt::format(
        "{}: {} reproj. percentile {}", cameras[i].id, ssize(errors[i]), line.str());
  }
}

void removeOutliersFromCameras(
    std::vector<Overlap>& overlaps,
    const FeatureMap& featureMap,
    const std::vector<Trace>& traces,
    const std::vector<Camera>& cameras,
    const Camera::Real outlierFactor) {
  // compute reprojection errors for each camera
  std::vector<std::vector<Camera::Real>> errors =
      reprojectionErrors(overlaps, featureMap, traces, cameras);

  // compute median for each camera
  std::vector<Camera::Real> medians(ssize(errors));
  for (ssize_t i = 0; i < ssize(errors); ++i) {
    medians[i] = calcPercentile(errors[i]);
  }

  std::unordered_map<ImageId, int> outliers;

  // remove matches that neither endpoint wants to keep around
  std::vector<ssize_t> errorIdxs(ssize(errors), 0);
  int total = 0;
  int inlierTotal = 0;
  for (Overlap& overlap : overlaps) {
    if (!overlap.isIntraFrame()) {
      continue;
    }
    const std::reference_wrapper<const ImageId> images[2] = {overlap.images[0], overlap.images[1]};
    const int idxs[2] = {getCameraIndex(images[0]), getCameraIndex(images[1])};

    // move the inliers to front of overlap.matches and resize to fit
    int inliers = 0;
    for (const auto& match : overlap.matches) {
      bool inlier = false;
      for (ssize_t i = 0; i < ssize(match); ++i) {
        int cameraIndex = idxs[i];
        if (errors[cameraIndex][errorIdxs[cameraIndex]++] < medians[cameraIndex] * outlierFactor) {
          inlier = true;
        }
      }
      if (inlier) {
        overlap.matches[inliers++] = match;
      }
    }
    outliers[overlap.images[0]] += ssize(overlap.matches) - inliers;
    outliers[overlap.images[1]] += ssize(overlap.matches) - inliers;
    total += ssize(overlap.matches);
    overlap.matches.resize(inliers);
    inlierTotal += inliers;
  }

  // sanity check that we consumed all the errors
  for (ssize_t i = 0; i < ssize(errors); ++i) {
    CHECK_EQ(errorIdxs[i], ssize(errors[i]));
  }
  if (FLAGS_log_verbose) {
    for (const auto& p : outliers) {
      LOG(INFO) << fmt::format("Removed {} outliers from {}", p.second, p.first);
    }
  }
  LOG(INFO) << fmt::format("{} of {} matches were inliers", inlierTotal, total);
}

void removeInvalidTraces(std::vector<Trace>& traces, FeatureMap& featureMap) {
  int total = 0;
  int removed = 0;
  for (Trace& trace : traces) {
    if (!trace.references.empty()) {
      ++total;
    }
    std::unordered_set<ImageId> uniqueCameras;
    for (const auto& ref : trace.references) {
      if (!uniqueCameras.insert(ref.first).second) {
        // image referenced more than once, remove the trace
        trace.clear(featureMap);
        ++removed;
        break;
      }
    }
  }
  LOG(INFO) << fmt::format("removed {} out of {} traces", removed, total);
}

void triangulateTracesThread(
    std::vector<Trace>& traces,
    const size_t begin,
    const size_t end,
    const FeatureMap& featureMap,
    const std::vector<Camera>& cameras) {
  for (size_t i = begin; i < end; ++i) {
    Trace& trace = traces[i];
    if (!trace.references.empty()) {
      Observations observations;
      for (const auto& ref : trace.references) {
        const Feature& feature = featureMap.at(ref.first)[ref.second];
        const Camera& camera = cameras[getCameraIndex(ref.first)];
        observations.emplace_back(camera, feature.position);
      }
      trace.position = triangulate(observations);
    }
  }
}

void triangulateTraces(
    std::vector<Trace>& traces,
    const FeatureMap& featureMap,
    const std::vector<Camera>& cameras) {
  const int threadCount = ThreadPool::getThreadCountFromFlag(FLAGS_threads);
  std::vector<std::thread> threads;
  for (int thread = 0; thread < threadCount; ++thread) {
    threads.emplace_back(
        triangulateTracesThread,
        std::ref(traces),
        thread * traces.size() / threadCount,
        (thread + 1) * traces.size() / threadCount,
        std::cref(featureMap),
        std::cref(cameras));
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

std::vector<Trace> assembleTraces(FeatureMap& featureMap, const std::vector<Overlap>& overlaps) {
  // mark all features as unreferenced
  for (auto& features : featureMap) {
    for (Feature& feature : features.second) {
      feature.trace = -1;
    }
  }
  std::vector<Trace> traces;
  int nonemptyTraceCount = 0;
  for (const Overlap& overlap : overlaps) {
    const std::reference_wrapper<std::vector<Feature>> features[2] = {
        featureMap[overlap.images[0]], featureMap[overlap.images[1]]};
    for (const auto& match : overlap.matches) {
      const std::reference_wrapper<int> indexes[2] = {
          features[0].get()[match[0]].trace, features[1].get()[match[1]].trace};
      if (indexes[0] < 0 && indexes[1] < 0) { // neither belongs to a trace, start new trace
        traces.emplace_back();
        nonemptyTraceCount++;
        for (ssize_t i = 0; i < ssize(indexes); ++i) {
          indexes[i].get() = traces.size() - 1;
          traces[indexes[i]].add(overlap.images[i], match[i]);
        }
      } else if (indexes[0] < 0) { // 0 does not belong to a trace, add to 1's trace
        indexes[0].get() = indexes[1];
        traces[indexes[0]].add(overlap.images[0], match[0]);
      } else if (indexes[1] < 0) { // 1 does not belong to a trace, add to 0's trace
        indexes[1].get() = indexes[0];
        traces[indexes[1]].add(overlap.images[1], match[1]);
      } else if (indexes[0] != indexes[1]) { // merge two traces, 0 inherits 1's references
        traces[indexes[0]].inherit(traces[indexes[1]], featureMap, indexes[0]);
        --nonemptyTraceCount;
      }
    }
  }

  LOG(INFO) << fmt::format("found {} nonempty traces", nonemptyTraceCount);

  return traces;
}

cv::Mat_<cv::Vec3w> blend(const cv::Mat_<cv::Vec3w>& mat0, const cv::Mat_<cv::Vec3w>& mat1) {
  if (mat0.empty()) {
    return 0.5 * mat1;
  }
  cv::Mat_<cv::Vec3w> result;
  cv::addWeighted(mat0, 0.5, mat1, 0.5, 0, result);
  return result;
}

// draw a line that starts out red, ends up green
void drawRedGreenLine(
    cv::Mat_<cv::Vec3w>& dst,
    const Camera::Vector2& r,
    const Camera::Vector2& g,
    const Camera::Vector2& m) {
  const cv::Scalar red = cv_util::createBGR<cv::Vec3w>(0, 0, 1);
  const cv::Scalar green = cv_util::createBGR<cv::Vec3w>(0, 1, 0);
  cv::line(dst, cv::Point2f(r.x(), r.y()), cv::Point2f(m.x(), m.y()), red, 2);
  cv::line(dst, cv::Point2f(g.x(), g.y()), cv::Point2f(m.x(), m.y()), green, 2);
}

template <typename T>
cv::Mat_<T> projectImageBetweenCamerasNearest(
    const Camera& dst,
    const Camera& src,
    const cv::Mat_<T>& srcImage) {
  cv::Mat_<T> dstImage(cv::Size(dst.resolution.x(), dst.resolution.y()));
  for (int y = 0; y < dstImage.rows; ++y) {
    for (int x = 0; x < dstImage.cols; ++x) {
      Camera::Vector3 rig = dst.rigNearInfinity({x + 0.5, y + 0.5});
      Camera::Vector2 srcPixel;
      if (src.sees(rig, srcPixel)) {
        dstImage(y, x) = srcImage.empty() ? cv_util::createBGR<T>(1, 1, 1)
                                          : srcImage(srcPixel.y(), srcPixel.x());
      } else {
        dstImage(y, x) = cv_util::createBGR<T>(0, 0, 0);
      }
    }
  }
  return dstImage;
}

cv::Mat_<cv::Vec3w> renderOverlap(
    const Overlap& overlap,
    const FeatureMap& featureMap,
    const std::vector<Trace>& traces,
    const std::vector<Camera>& cameras) {
  // transform image 1 into image 0's space and overlay the two
  const ImageId& image0 = overlap.images[0];
  const ImageId& image1 = overlap.images[1];
  const Camera& camera0 = cameras[getCameraIndex(image0)];
  const Camera& camera1 = cameras[getCameraIndex(image1)];
  const std::vector<Feature>& features0 = featureMap.at(image0);
  const std::vector<Feature>& features1 = featureMap.at(image1);
  cv::Mat_<cv::Vec3w> result = blend(
      loadImage(image0), projectImageBetweenCamerasNearest(camera0, camera1, loadImage(image1)));
  for (const auto& match : overlap.matches) {
    Camera::Vector2 p0 = features0[match[0]].position;
    Camera::Vector2 p1 = features1[match[1]].position;
    int trace = features0[match[0]].trace;
    CHECK_EQ(trace, features1[match[1]].trace);
    Camera::Vector3 rig =
        trace < 0 ? triangulate({{camera0, p0}, {camera1, p1}}) : traces[trace].position;
    drawRedGreenLine(
        result,
        p0, // p0 in red
        camera0.pixel(camera1.rigNearInfinity(p1)), // transformed p1 in green
        camera0.pixel(rig)); // via transformed triangulation
  }

  return result;
}

cv::Mat_<cv::Vec3w> renderReprojections(
    const ImageId& image,
    const Camera& camera,
    const std::vector<Feature>& features,
    const std::vector<Trace>& traces,
    const Camera::Real scale) {
  cv::Mat_<cv::Vec3w> result = 0.5f * loadImage(image);
  cv::Mat_<cv::Vec3f> errors = cv::Mat::zeros(result.size(), CV_32FC3);
  const cv::Scalar green(0, 255, 0);
  const cv::Scalar red(0, 0, 255);
  for (const Feature& feature : features) {
    if (feature.trace >= 0) {
      // draw red line from image feature to reprojected world point
      // then continue in green in the same direction but scale x as far
      Camera::Vector2 proj = camera.pixel(traces[feature.trace].position);
      Camera::Vector2 error = proj - feature.position;

      // OpenCV can only save 1, 3, 4 channels so the third is simply an artifact
      errors(cv::Point(feature.position.x(), feature.position.y())) =
          cv::Vec3f(error.x(), error.y(), 0.0f);
      drawRedGreenLine(result, feature.position, proj + scale * error, proj);
    }
  }

  if (FLAGS_errors_dir != "") {
    filesystem::create_directories(FLAGS_errors_dir);
    std::string errorsFile = fmt::format("{}/{}.exr", FLAGS_errors_dir, getCameraId(image));
    cv_util::imwriteExceptionOnFail(errorsFile, errors);
  }

  return result;
}

std::string getReprojectionReport(
    const ceres::Problem& problem,
    const double* parameter = nullptr) {
  std::vector<double> norms =
      getReprojectionErrorNorms(problem, parameter, FLAGS_weighted_statistics);
  double total = 0;
  double totalSq = 0;
  for (double norm : norms) {
    total += norm;
    totalSq += norm * norm;
  }

  std::ostringstream result;
  result << "reprojections " << norms.size() << " " << "RMSE " << std::sqrt(totalSq / norms.size())
         << " " << "average " << total / norms.size() << " " << "median "
         << calcPercentile(norms, 0.5) << " " << "90% " << calcPercentile(norms, 0.9) << " "
         << "99% " << calcPercentile(norms, 0.99) << " ";

  result << "worst 3: ";
  std::sort(norms.begin(), norms.end());
  for (int i = norms.size() - 3; i < int(norms.size()); ++i) {
    result << norms[i] << " ";
  }

  return result.str();
}

double acosClamp(double x) {
  return std::acos(std::min(std::max(-1.0, x), 1.0));
}

std::string getCameraRmseReport(
    const std::vector<Camera>& cameras,
    const std::vector<Camera>& groundTruth) {
  Camera::Real position = 0;
  Camera::Real rotation = 0;
  Camera::Real principal = 0;
  Camera::Real distortion = 0;
  Camera::Real focal = 0;

  for (int i = 0; i < int(cameras.size()); ++i) {
    {
      auto before = groundTruth[i].position;
      auto after = cameras[i].position;
      position += (after - before).squaredNorm();
    }
    for (int v = 0; v < 3; ++v) {
      auto before = groundTruth[i].rotation.row(v);
      auto after = cameras[i].rotation.row(v);
      rotation += (after - before).squaredNorm();
    }
    {
      auto before = groundTruth[i].principal;
      auto after = cameras[i].principal;
      principal += (after - before).squaredNorm();
    }
    {
      auto before = groundTruth[i].getDistortion();
      auto after = cameras[i].getDistortion();
      distortion += (after - before).squaredNorm();
    }
    {
      auto before = groundTruth[i].focal;
      auto after = cameras[i].focal;
      focal += (after - before).squaredNorm();
    }
  }

  Camera::Real angle = 0;
  int angleCount = 0;
  for (int i = 0; i < int(cameras.size()); ++i) {
    for (int j = 0; j < i; ++j) {
      for (int v = 2; v < 3; ++v) {
        // angle between camera i and j
        auto before = acosClamp(groundTruth[i].rotation.row(v).dot(groundTruth[j].rotation.row(v)));
        if (before > 1) {
          continue; // only count angles less than a radian
        }
        auto after = acosClamp(cameras[i].rotation.row(v).dot(cameras[j].rotation.row(v)));
        angle += (after - before) * (after - before);
        ++angleCount;
      }
    }
  }

  // average
  position /= cameras.size();
  rotation /= 3 * cameras.size();
  principal /= cameras.size();
  distortion /= cameras.size();
  focal /= cameras.size();
  angle /= angleCount;

  std::ostringstream result;
  result << "RMSEs: " << "Pos " << std::sqrt(position) << " " << "Rot " << std::sqrt(rotation)
         << " " << "Principal " << std::sqrt(principal) << " " << "Distortion "
         << std::sqrt(distortion) << " " << "Focal " << std::sqrt(focal) << " " << "Angle "
         << std::sqrt(angle) << " ";

  return result.str();
}

template <typename T>
double* parameterBlock(T& t) {
  return t.data();
}

template <>
double* parameterBlock(Camera::Real& t) {
  return &t;
}

template <>
double* parameterBlock(Trace& t) {
  return t.position.data();
}

template <typename T>
void lockParameter(ceres::Problem& problem, T& param, const bool lock = true) {
  if (lock) {
    problem.SetParameterBlockConstant(parameterBlock(param));
  } else {
    problem.SetParameterBlockVariable(parameterBlock(param));
  }
}

template <typename T>
void lockParameters(ceres::Problem& problem, std::vector<T>& params, const bool lock = true) {
  for (T& param : params) {
    lockParameter(problem, param, lock);
  }
}

void reasonableResize(cv::Mat_<cv::Vec3w>& mat) {
  const double kWidth = 1200;
  const double kHeight = 800;
  double factor = std::min(kWidth / mat.cols, kHeight / mat.rows);
  if (factor < 1) {
    cv::resize(mat, mat, {}, factor, factor, cv::INTER_AREA);
  }
}

void showMatches(
    const std::vector<Camera>& cameras,
    const FeatureMap& featureMap,
    const std::vector<Overlap>& overlaps,
    const std::vector<Trace>& traces,
    const int pass) {
  // visualization for debugging
  for (const Overlap& overlap : overlaps) {
    const int idx0 = getCameraIndex(overlap.images[0]);
    const int idx1 = getCameraIndex(overlap.images[1]);
    if (cameras[idx0].overlap(cameras[idx1]) > FLAGS_debug_matches_overlap) {
      cv::Mat_<cv::Vec3w> image = renderOverlap(overlap, featureMap, traces, cameras);
      reasonableResize(image);

      if (!FLAGS_debug_dir.empty()) {
        std::string filename = FLAGS_debug_dir + "/" + "pass" + std::to_string(pass) + "_" +
            getCameraId(overlap.images[0]) + "-" + getCameraId(overlap.images[1]) + ".png";
        imwrite(filename, image);
      } else {
        cv::imshow("overlap", image);
        cv::waitKey();
      }
    }
  }
}

void showReprojections(
    const std::vector<Camera>& cameras,
    const FeatureMap& featureMap,
    const std::vector<Trace>& traces,
    const Camera::Real scale) {
  for (const auto& entry : featureMap) {
    const ImageId& image = entry.first;
    const std::vector<Feature>& features = entry.second;
    const Camera& camera = cameras[getCameraIndex(image)];
    cv::Mat_<cv::Vec3w> render = renderReprojections(image, camera, features, traces, scale);
    reasonableResize(render);
    if (!FLAGS_debug_dir.empty()) {
      std::string filename = FLAGS_debug_dir + "/" + camera.id + ".png";
      imwrite(filename, render);
    } else {
      cv::imshow("reprojections", render);
      cv::waitKey();
    }
  }
}

// returns true with a probability of numerator / denominator
bool randomSample(int numerator, int denominator) {
  static std::default_random_engine e;
  return numerator > std::uniform_int_distribution<>(0, denominator - 1)(e);
}

void solve(ceres::Problem& problem) {
  ceres::Solver::Options options;
  options.use_inner_iterations = true;
  options.max_num_iterations = 500;
  options.minimizer_progress_to_stdout = false;
  options.num_threads = ThreadPool::getThreadCountFromFlag(FLAGS_ceres_threads);
  if (options.num_threads == 0) {
    options.num_threads = 1;
  }
  options.function_tolerance = FLAGS_ceres_function_tolerance;

  ceres::Solver::Summary summary;

  LOG(INFO) << getReprojectionReport(problem);

  int previousFLAGS_v = FLAGS_v; // save FLAGS_v
  if (FLAGS_log_verbose) {
    FLAGS_v = std::max(1, FLAGS_v); // overwrite FLAGS_v
  }
  Solve(options, &problem, &summary);
  FLAGS_v = previousFLAGS_v; // restore FLAGS_v

  LOG(INFO) << summary.BriefReport();
  if (FLAGS_log_verbose) {
    LOG(INFO) << summary.FullReport();
  }

  if (summary.termination_type == ceres::NO_CONVERGENCE) {
    throw std::runtime_error("Failed to converge");
  }

  LOG(INFO) << getReprojectionReport(problem);
}

void validateMatchCount(const std::vector<Camera>& cameras, const std::vector<int>& counts) {
  const double sum = std::accumulate(counts.begin(), counts.end(), 0.0);
  const double mean = sum / counts.size();

  const double sqSum = std::inner_product(counts.begin(), counts.end(), counts.begin(), 0.0);
  const double stdev = std::sqrt(sqSum / counts.size() - mean * mean);

  std::vector<std::string> lowTraceErrors;
  for (int i = 0; i < int(counts.size()); ++i) {
    if (FLAGS_log_verbose) {
      LOG(INFO) << fmt::format("Camera: {} Traces: {}", cameras[i].id, counts[i]);
    }
    const double z = (counts[i] - mean) / stdev;
    if (-z > FLAGS_outlier_z_threshold || counts[i] < FLAGS_min_traces) {
      lowTraceErrors.push_back(
          fmt::format("Too few matches in camera {}: {}", cameras[i].id, counts[i]));
    }
  }

  if (!lowTraceErrors.empty()) {
    std::string errorString = boost::algorithm::join(lowTraceErrors, "\n");
    throw std::runtime_error(errorString);
  }
}

void savePointsFileJson(FeatureMap& featureMap, const std::vector<Trace>& traces) {
  folly::dynamic arrayOfTraces = folly::dynamic::array();
  for (const Trace& trace : traces) {
    if (trace.references.empty()) {
      continue; // don't output zombie traces, a different trace has the references now
    }
    folly::dynamic arrayOfFeatures = folly::dynamic::array();
    for (const auto& ref : trace.references) {
      const ImageId& image = ref.first;
      const Feature& feature = featureMap[image][ref.second];
      folly::dynamic featureSerialized = folly::dynamic::object("y", feature.position.y())(
          "x", feature.position.x())("image_id", ref.first);
      arrayOfFeatures.push_back(featureSerialized);
    }
    folly::dynamic traceSerialized = folly::dynamic::object("features", arrayOfFeatures)(
        "number of references", trace.references.size())("z", trace.position.z())(
        "y", trace.position.y())("x", trace.position.x());
    arrayOfTraces.push_back(traceSerialized);
  }

  folly::dynamic points = folly::dynamic::object("points", arrayOfTraces);
  CHECK(folly::writeFile(folly::toPrettyJson(points), FLAGS_points_file_json.c_str()));
}

void savePointsFile(FeatureMap& featureMap, const std::vector<Trace>& traces) {
  std::ofstream file(FLAGS_points_file);
  for (const Trace& trace : traces) {
    if (trace.references.empty()) {
      continue; // don't output zombie traces, a different trace has the references now
    }
    file << fmt::format("{} {} {} ", trace.position.x(), trace.position.y(), trace.position.z());
    file << fmt::format("1 "); // delimiter
    file << fmt::format("0 0 0"); // RGB value for the point
    file << "\n";
  }
}

std::vector<int> calculateCameraWeights(
    const std::vector<Camera>& cameras,
    const std::vector<Trace>& traces) {
  std::vector<int> weights(cameras.size(), 1);
  if (FLAGS_weight_by_trace_count) {
    for (ssize_t i = 0; i < ssize(cameras); ++i) {
      int cameraTraces = 0;
      for (const Trace& trace : traces) {
        for (const auto& reference : trace.references) {
          if (cameras[i].id == cameras[getCameraIndex(reference.first)].id) {
            cameraTraces++;
            continue;
          }
        }
      }
      weights[i] = cameraTraces;
    }
  }
  return weights;
}

bool positionsUnlocked(int pass) {
  return !FLAGS_lock_positions && pass != 0;
}

double refine(
    std::vector<Camera>& cameras,
    const std::vector<Camera>& groundTruth,
    FeatureMap featureMap,
    std::vector<Overlap> overlaps,
    const int pass) {
  boost::timer::cpu_timer timer;
  // remove outlier matches
  LOG(INFO) << "Removing outlier matches...";
  removeOutliersFromCameras(overlaps, featureMap, {}, cameras, FLAGS_outlier_factor);

  // assemble and remove outlier traces
  LOG(INFO) << "Assembling traces and removing outlier traces...";
  std::vector<Trace> traces = assembleTraces(featureMap, overlaps);
  triangulateTraces(traces, featureMap, cameras);
  removeOutliersFromCameras(overlaps, featureMap, traces, cameras, FLAGS_outlier_factor);

  // final triangulation
  LOG(INFO) << "Reassembling traces with outliers removed and removing invalid traces...";
  traces = assembleTraces(featureMap, overlaps);
  if (!FLAGS_keep_invalid_traces) {
    removeInvalidTraces(traces, featureMap);
  }
  triangulateTraces(traces, featureMap, cameras);

  std::vector<int> weights = calculateCameraWeights(cameras, traces);

  // visualization for debugging
  showMatches(cameras, featureMap, overlaps, traces, pass);

  // read camera parameters from cameras
  std::vector<Camera::Vector3> positions;
  std::vector<Camera::Vector3> rotations;
  std::vector<Camera::Vector2> principals;
  std::vector<Camera::Real> focals;
  std::vector<Camera::Distortion> distortions;
  for (Camera& camera : cameras) {
    positions.push_back(camera.position);
    rotations.push_back(camera.getRotation());
    principals.push_back(camera.principal);
    focals.push_back(camera.getScalarFocal());
    distortions.push_back(camera.getDistortion());
  }

  int referenceCameraIdx = -1;
  int relativeCameraIdx = -1;
  Camera::Real theta;
  Camera::Real phi;
  Camera::Real radius = 0.0; // initialized to silence compiler but this will never get used

  // If positions are unlocked, define a locked reference camera and lock the baseline between
  // the reference camera and relative camera
  if (positionsUnlocked(pass)) {
    if (FLAGS_reference_camera.empty()) {
      referenceCameraIdx = 0;
    } else {
      CHECK(cameraIdToIndex.count(FLAGS_reference_camera))
          << "bad reference_camera: " << FLAGS_reference_camera;
      referenceCameraIdx = cameraIdToIndex[FLAGS_reference_camera];
    }
    relativeCameraIdx = (referenceCameraIdx + 1) % ssize(cameras);

    Camera::Vector3 relativePosition = positions[relativeCameraIdx] - positions[referenceCameraIdx];
    cartesianToSpherical(radius, theta, phi, relativePosition);
  }

  // create the problem: add a residual for each observation
  ceres::Problem problem;
  std::vector<int> counts(cameras.size());
  for (const Trace& trace : traces) {
    if (FLAGS_cap_traces && !randomSample(FLAGS_cap_traces, traces.size())) {
      continue;
    }
    for (const auto& ref : trace.references) {
      const ImageId& image = ref.first;
      const Feature& feature = featureMap[image][ref.second];
      const int camera = getCameraIndex(image);
      ++counts[camera];
      const int group = cameraGroupToIndex[cameras[camera].group];
      if (camera == relativeCameraIdx) {
        SphericalReprojectionFunctor::addResidual(
            problem,
            theta,
            phi,
            rotations[camera],
            principals[FLAGS_shared_principal_and_focal ? group : camera],
            focals[FLAGS_shared_principal_and_focal ? group : camera],
            distortions[FLAGS_shared_distortion ? group : camera],
            traces[feature.trace].position,
            radius,
            cameras[referenceCameraIdx].position,
            cameras[camera],
            feature.position,
            FLAGS_robust,
            weights[camera]);
      } else {
        ReprojectionFunctor::addResidual(
            problem,
            positions[camera],
            rotations[camera],
            principals[FLAGS_shared_principal_and_focal ? group : camera],
            focals[FLAGS_shared_principal_and_focal ? group : camera],
            distortions[FLAGS_shared_distortion ? group : camera],
            traces[feature.trace].position,
            cameras[camera],
            feature.position,
            FLAGS_robust,
            weights[camera]);
      }
    }
  }

  validateMatchCount(cameras, counts);

  // lock focal and distortion
  if (pass == 0 || FLAGS_lock_focal) {
    if (FLAGS_shared_principal_and_focal) {
      for (const auto& mapping : cameraGroupToIndex) {
        lockParameter(problem, focals[mapping.second]);
      }
    } else {
      lockParameters(problem, focals);
    }
  }
  if (pass == 0 || FLAGS_lock_distortion) {
    if (FLAGS_shared_distortion) {
      for (const auto& mapping : cameraGroupToIndex) {
        lockParameter(problem, distortions[mapping.second]);
      }
    } else {
      lockParameters(problem, distortions);
    }
  }
  if (FLAGS_lock_principals) {
    lockParameters(problem, principals);
  }

  // lock position
  LOG(INFO) << fmt::format("Pass: {}", pass);
  // If positions are unlocked, only lock the position and rotation of the reference camera
  if (positionsUnlocked(pass)) {
    problem.SetParameterBlockConstant(positions[referenceCameraIdx].data());
    problem.SetParameterBlockConstant(rotations[referenceCameraIdx].data());
  } else {
    lockParameters(problem, positions);
  }

  if (FLAGS_lock_rotations) {
    lockParameters(problem, rotations);
  }

  if (FLAGS_robust) {
    std::vector<calibration::ReprojectionErrorOutlier> errorsIgnored =
        getReprojectionErrorOutliers(problem);
    LOG(INFO) << fmt::format("Number of down-weighted outliers: {}", errorsIgnored.size());
    std::sort(errorsIgnored.begin(), errorsIgnored.end(), math_util::sortdescPair<double, double>);
    LOG(INFO) << fmt::format(
        "Highest 3 (true/weighted): {}/{}, {}/{}, {}/{}",
        errorsIgnored[2].first,
        errorsIgnored[2].second,
        errorsIgnored[1].first,
        errorsIgnored[1].second,
        errorsIgnored[0].first,
        errorsIgnored[0].second);
  }
  reportReprojectionErrors(overlaps, featureMap, traces, cameras);
  solve(problem);
  if (positionsUnlocked(pass)) {
    positions[relativeCameraIdx] = sphericalToCartesian(radius, theta, phi);
    positions[relativeCameraIdx] += positions[referenceCameraIdx];
  }

  std::vector<double> norms =
      getReprojectionErrorNorms(problem, nullptr, FLAGS_weighted_statistics);
  double median = calcPercentile(norms, 0.5);
  if (pass == FLAGS_pass_count - 1 && median > FLAGS_max_error) {
    LOG(INFO) << fmt::format("Warning: Final pass median error too high: {}", median);
  }

  // write optimized camera parameters back into cameras
  for (int i = 0; i < int(cameras.size()); ++i) {
    const int group = cameraGroupToIndex[cameras[i].group];
    cameras[i] = makeCamera(
        cameras[i],
        positions[i],
        rotations[i],
        principals[FLAGS_shared_principal_and_focal ? group : i],
        focals[FLAGS_shared_principal_and_focal ? group : i],
        distortions[FLAGS_shared_distortion ? group : i]);
  }

  reportReprojectionErrors(overlaps, featureMap, traces, cameras);

  if (FLAGS_points_file != "" && pass == FLAGS_pass_count - 1) {
    savePointsFile(featureMap, traces);
  }
  if (FLAGS_points_file_json != "" && pass == FLAGS_pass_count - 1) {
    savePointsFileJson(featureMap, traces);
  }

  // visualization for debugging
  if (FLAGS_debug_error_scale && pass == FLAGS_pass_count - 1) {
    showReprojections(cameras, featureMap, traces, FLAGS_debug_error_scale);
  }

  if (FLAGS_enable_timing) {
    LOG(INFO) << fmt::format("Pass {} timing :{}", pass, timer.format());
  }
  return median;
}

double geometricCalibration() {
  CHECK_NE(FLAGS_rig_in, "");
  CHECK_NE(FLAGS_rig_out, "");

  if (FLAGS_debug_error_scale || FLAGS_debug_matches_overlap < 1) {
    CHECK_NE(FLAGS_color, "");
  }

  if (!FLAGS_debug_dir.empty()) {
    filesystem::create_directories(FLAGS_debug_dir);
  }

  const Camera::Rig groundTruth = Camera::loadRig(FLAGS_rig_in);
  buildCameraIndexMaps(groundTruth);
  double medianError = 0;

  if (FLAGS_seed != -1) {
    std::srand(FLAGS_seed);
  }

  for (int experiment = 0; experiment < FLAGS_experiments; ++experiment) {
    Camera::Rig cameras = groundTruth;

    Camera::perturbCameras(
        cameras,
        FLAGS_perturb_positions,
        FLAGS_perturb_rotations,
        FLAGS_perturb_principals,
        FLAGS_perturb_focals);

    FeatureMap featureMap;
    std::vector<Overlap> overlaps;

    if (!FLAGS_matches.empty()) {
      folly::dynamic parsed = parseJsonFile(FLAGS_matches);
      featureMap = loadFeatureMap(parsed);
      overlaps = loadOverlaps(parsed);
    } else {
      generateArtificalPoints(featureMap, overlaps, groundTruth);
    }

    LOG(INFO) << getCameraRmseReport(cameras, groundTruth);
    boost::timer::cpu_timer timer;

    for (int pass = 0; pass < FLAGS_pass_count; ++pass) {
      medianError = refine(cameras, groundTruth, featureMap, overlaps, pass);
      std::cout << "pass " << pass << ": " << getCameraRmseReport(cameras, groundTruth)
                << std::endl;
    }
    if (FLAGS_enable_timing) {
      LOG(INFO) << fmt::format("Aggregate timing: {}", timer.format());
    }
    Camera::saveRig(FLAGS_rig_out, cameras);
  }

  return medianError;
}
