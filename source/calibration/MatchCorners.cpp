/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/calibration/MatchCorners.h"

#include <fmt/format.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/Format.h>

#include "source/calibration/Calibration.h"
#include "source/calibration/FeatureDetector.h"
#include "source/calibration/FeatureMatcher.h"
#include "source/util/FilesystemUtil.h"
#include "source/util/ImageUtil.h"
#include "source/util/ThreadPool.h"

using namespace fb360_dep;
using namespace fb360_dep::calibration;
using namespace fb360_dep::image_util;

DEFINE_int32(
    camera_count,
    0,
    "Total number of cameras to match. Default value of 0 will match all the cameras in the json");
DEFINE_string(
    color_channel,
    "grayscale",
    "color channel. supported channels: grayscale, red, green, blue");
DEFINE_int32(min_features, 1500, "minimum number of features to consider calibration valid");
DEFINE_int32(octave_count, 4, "number of resolutions to use when looking for features");
DEFINE_bool(same_scale, false, "match at same scale where feature was found");
DEFINE_double(scale, 1, "scale at which to perform matching");
DEFINE_bool(use_nearest, false, "use nearest neighbor during corner matching, default is bilinear");

using Image = cv::Mat_<uint8_t>;
using ImageId = std::string;

namespace fb360_dep::calibration {

Image extractSingleChannelImage(const cv::Mat_<cv::Vec3b>& image) {
  std::vector<Image> splitImage;
  cv::split(image, splitImage);

  const int channels = 3;
  CHECK_EQ(splitImage.size(), channels) << "error loading separate color channels";
  LOG(INFO) << fmt::format("Loading single channge images {}", FLAGS_color_channel);
  Image singleChannelImage;
  if (FLAGS_color_channel == "grayscale") {
    cv::cvtColor(image, singleChannelImage, cv::COLOR_BGR2GRAY);
  } else if (FLAGS_color_channel == "blue") {
    singleChannelImage = splitImage[0];
  } else if (FLAGS_color_channel == "green") {
    singleChannelImage = splitImage[1];
  } else if (FLAGS_color_channel == "red") {
    singleChannelImage = splitImage[2];
  } else {
    LOG(FATAL) << fmt::format("Unknown color channel selected: {}", FLAGS_color_channel);
  }
  return singleChannelImage;
}

std::vector<Image> loadSingleChannelImages(const filesystem::path& dir, const Camera::Rig& rig) {
  // Load color images without alpha in bgr format
  std::vector<cv::Mat_<cv::Vec3b>> images =
      loadImages<cv::Vec3b>(dir, rig, FLAGS_frame, FLAGS_threads);

  ThreadPool threadPool(FLAGS_threads);

  std::vector<Image> singleChannelImages;
  for (auto& image : images) {
    singleChannelImages.emplace_back(extractSingleChannelImage(image));
  }

  return singleChannelImages;
}

static void saveMatches(
    const filesystem::path& filename,
    const std::map<ImageId, std::vector<Keypoint>>& allCorners,
    const std::vector<Overlap>& overlaps) {
  const filesystem::path colorDir = FLAGS_color;
  CHECK(!allCorners.empty());
  const std::string imageExt = filesystem::getFirstExtension(colorDir / allCorners.begin()->first);

  folly::dynamic allCornersData = Keypoint::serializeRig(allCorners, FLAGS_frame, imageExt);

  folly::dynamic allMatches = folly::dynamic::array;
  for (const auto& overlap : overlaps) {
    allMatches.push_back(overlap.serialize(FLAGS_frame, imageExt));
  }

  folly::dynamic matchesData =
      folly::dynamic::object("all_matches", allMatches)("images", allCornersData);
  LOG(INFO) << fmt::format("Saving matches to file: {}", filename.string());
  if (filename.has_parent_path()) {
    filesystem::create_directories(filename.parent_path());
  }
  CHECK(folly::writeFile(folly::toPrettyJson(matchesData), filename.string().data()));
}

template <typename T>
static void downscale(cv::Mat_<T>& image) {
  if (FLAGS_scale != 1.0) {
    cv::resize(image, image, {}, FLAGS_scale, FLAGS_scale, cv::INTER_AREA);
  }
}

std::vector<Image> loadChannels(const Camera::Rig& rig) {
  std::vector<Image> images;
  LOG(INFO) << "Loading images... ";
  const filesystem::path colorDir = FLAGS_color;
  if (FLAGS_color_channel == "grayscale") {
    images = loadImages<uint8_t>(colorDir, rig, FLAGS_frame, FLAGS_threads);
  } else if (
      FLAGS_color_channel == "red" || FLAGS_color_channel == "green" ||
      FLAGS_color_channel == "blue") {
    images = loadSingleChannelImages(colorDir, rig);

  } else {
    LOG(FATAL) << fmt::format("Unknown color channel selected: {}", FLAGS_color_channel);
  }
  LOG(INFO) << "Images loaded";

  // scale according to FLAGS_scale and verify that resolutions match

  for (Image& image : images) {
    downscale(image);
  }
  // check that camera and image aspect ratios match within 1%
  CHECK_EQ(ssize(rig), ssize(images));
  for (ssize_t i = 0; i < ssize(rig); ++i) {
    const Camera::Vector2 res = rig[i].resolution;
    const double ratio = images[i].cols / double(images[i].rows);
    CHECK_LT(ratio - 0.01, res.x() / res.y()) << rig[i].id << " image and camera shape mismatch";
    CHECK_LT(res.x() / res.y(), ratio + 0.01) << rig[i].id << " image and camera shape mismatch";
  }

  return images;
}

static Camera::Rig loadRig() {
  Camera::Rig rig = Camera::loadRig(FLAGS_rig_in);
  while (0 < FLAGS_camera_count && FLAGS_camera_count < ssize(rig)) {
    rig.pop_back(); // we will only match a subset of the cameras in the rig
  }
  CHECK_GT(ssize(rig), 0);
  return rig;
}

// rescale rig to match image resolution
static Camera::Rig rescale(const Camera::Rig& rigFull, const std::vector<Image>& images) {
  Camera::Rig rig;
  CHECK_EQ(ssize(rigFull), ssize(images));
  for (ssize_t i = 0; i < ssize(rigFull); ++i) {
    rig.push_back(rigFull[i].rescale({images[i].cols, images[i].rows}));
  }
  CHECK_EQ(ssize(rig), ssize(rigFull));
  return rig;
}

// upscale corners from image to rig resolution
static void upscale(
    std::map<ImageId, std::vector<Keypoint>>& corners,
    const Camera::Rig& rig,
    const std::vector<Image>& images) {
  for (auto& pair : corners) {
    // compute index of camera id in pair.first
    ssize_t index = -1;
    for (ssize_t i = 0; i < ssize(rig); ++i) {
      if (rig[i].id == pair.first) {
        index = i;
      }
    }
    CHECK_NE(index, -1) << "no camera named " << pair.first << " found in rig";

    // go through keypoints scaling from image to camera resolution
    for (Keypoint& keypoint : pair.second) {
      keypoint.coords.x() *= rig[index].resolution.x() / images[index].cols;
      keypoint.coords.y() *= rig[index].resolution.y() / images[index].rows;
    }
  }
}

void processScale(
    const float scale,
    const Camera::Rig& rigFull,
    const std::vector<Image>& images,
    std::map<ImageId, std::vector<Keypoint>>& allCorners,
    std::vector<Overlap>& overlaps) {
  LOG(INFO) << fmt::format("Processing scale: {}", scale);

  std::vector<Image> scaledImages;
  for (const Image& image : images) {
    Image scaledImage;
    cv::resize(image, scaledImage, {}, scale, scale, cv::INTER_AREA);
    scaledImages.push_back(scaledImage);
  }

  // rescale rig to match images
  CHECK(!scaledImages.empty());
  const Camera::Rig rig = rescale(rigFull, scaledImages);

  std::map<ImageId, std::vector<Keypoint>> newCorners =
      findAllCorners(rig, scaledImages, FLAGS_use_nearest);
  std::vector<Overlap> newOverlaps = findAllMatches(rig, scaledImages, newCorners);
  upscale(newCorners, rigFull, scaledImages);

  // matches refer to corners by index, so we need to offset these by the total number
  for (Overlap& newOverlap : newOverlaps) {
    const ImageId& image0 = newOverlap.images[0];
    const ImageId& image1 = newOverlap.images[1];
    for (auto& match : newOverlap.matches) {
      match.corners[0] += allCorners[image0].size();
      match.corners[1] += allCorners[image1].size();
    }
  }

  // add newOverlaps to overlaps
  if (overlaps.empty()) {
    overlaps = newOverlaps;
  } else {
    CHECK_EQ(ssize(overlaps), ssize(newOverlaps));
    for (ssize_t i = 0; i < ssize(overlaps); ++i) {
      overlaps[i].matches.insert(
          overlaps[i].matches.end(), newOverlaps[i].matches.begin(), newOverlaps[i].matches.end());
    }
  }

  // add newCorners to corners
  for (const auto& entry : newCorners) {
    const ImageId& image = entry.first;
    allCorners[image].insert(
        allCorners[image].end(), newCorners[image].begin(), newCorners[image].end());
  }
}

void processOctaves(
    const Camera::Rig& rigFull,
    const std::vector<Image>& images,
    std::map<ImageId, std::vector<Keypoint>>& allCorners,
    std::vector<Overlap>& overlaps) {
  int octaveCount = FLAGS_same_scale ? FLAGS_octave_count : 1;
  for (int octave = 0; octave < octaveCount; octave++) {
    float scale = std::pow(0.5, octave);
    processScale(scale, rigFull, images, allCorners, overlaps);
  }
}

} // namespace fb360_dep::calibration

int matchCorners() {
  CHECK_NE(FLAGS_color, "");
  CHECK_NE(FLAGS_rig_in, "");
  CHECK_NE(FLAGS_matches, "");

  // Load camera rig
  const Camera::Rig& rigFull = loadRig();

  const int validFrame = getSingleFrame(FLAGS_color, rigFull, FLAGS_frame);
  FLAGS_frame = image_util::intToStringZeroPad(validFrame);

  // Load input grayscale images
  const std::vector<Image>& images = loadChannels(rigFull);

  std::map<ImageId, std::vector<Keypoint>> allCorners;
  std::vector<Overlap> overlaps;

  processOctaves(rigFull, images, allCorners, overlaps);

  for (const auto& entry : allCorners) {
    if (ssize(entry.second) < FLAGS_min_features) {
      throw std::runtime_error(
          fmt::format("Too few features found in camera {}: {}", entry.first, ssize(entry.second)));
    }
  }
  saveMatches(FLAGS_matches, allCorners, overlaps);

  return EXIT_SUCCESS;
}
