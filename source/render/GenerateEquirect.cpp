/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <math.h>
#include <algorithm>

#include <fmt/format.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <opencv2/opencv.hpp>

#include <folly/Format.h>

#include "source/rig/RigTransform.h"
#include "source/util/Camera.h"
#include "source/util/ImageUtil.h"
#include "source/util/ThreadPool.h"

using namespace fb360_dep;
using namespace fb360_dep::image_util;

const std::string kUsageMessage = R"(
  - Generates an equirect from a set of color images at a uniformly spaced range of depths.

  - Example:
    ./GenerateEquirect \
    --color=/path/to/video/color \
    --output=/path/to/output \
    --rig=/path/to/rigs/rig.json \
    --frame=000000 \
    --depth_min=1.0 \
    --depth_max=1000.0 \
    --num_depths=50
  )";

DEFINE_bool(black_bg, false, "set the background to be optionally black (red by default)");
DEFINE_string(camera_id, "", "id of camera selected to be centered");
DEFINE_string(cameras, "", "cameras to render (comma-separated)");
DEFINE_string(color, "", "path to input color images (required)");
DEFINE_bool(crop_equirect, false, "crop the equirect to only include visible images");
DEFINE_double(depth_max, 10.0, "max depth in m");
DEFINE_double(depth_min, 1.0, "min depth in m");
DEFINE_string(frame, "000000", "frame to process (lexical)");
DEFINE_uint64(height, 512, "equirect height in pixels");
DEFINE_uint64(num_depths, 50, "num depths");
DEFINE_string(output, "", "path to output directory (required)");
DEFINE_string(rig, "", "path to camera rig .json (required)");
DEFINE_double(scale, 1, "image scale factor");
DEFINE_int32(threads, -1, "number of threads (-1 = max allowed, 0 = no threading)");

using Image = cv::Mat_<cv::Vec4f>;

void saveImage(const cv::Mat_<cv::Vec4f> eqr, const double depth) {
  size_t height = eqr.rows;
  size_t width = eqr.cols;

  // Add text to image showing current depth
  const std::string depthStr = fmt::format("{:.2f}", depth);
  const cv::Point2f textPos((85.0f / 100.0f) * width, (6.0f / 100.0f) * height);
  const int textFont = cv::FONT_HERSHEY_PLAIN;
  const double textScale = 2;
  const cv::Scalar textColor(0, 1, 0, 1); // green
  cv::putText(eqr, depthStr + " m", textPos, textFont, textScale, textColor);

  const filesystem::path equirectDir = filesystem::path(FLAGS_output) / "equirect";
  filesystem::create_directories(equirectDir);

  // Pad filename with zeros so they are saved in lexicographical order
  const std::string filename =
      fmt::format("{}/{:05}_cm.png", equirectDir.string(), int(depth * 100));
  cv_util::imwriteExceptionOnFail(filename, 255.0f * eqr);
}

cv::Vec4f getPixelColor(
    const Camera::Vector3 currentPoint,
    const Camera::Rig& rig,
    const std::vector<Image>& images) {
  cv::Vec4f accColor(0, 0, 0, 0);
  int accImages = 0;
  for (int currentCamera = 0; currentCamera < int(rig.size()); ++currentCamera) {
    // If pixel in bounds
    Camera::Vector2 currentPixel;
    if (rig[currentCamera].sees(currentPoint, currentPixel)) {
      accColor += images[currentCamera](currentPixel.y(), currentPixel.x());
      accImages++;
    }
  }
  cv::Vec4f pixelColor;
  if (accImages > 0) {
    pixelColor = accColor / accImages;
  } else {
    pixelColor = FLAGS_black_bg ? cv::Vec4f(0, 0, 0, 1) : cv::Vec4f(0, 0, 1, 1);
  }
  return pixelColor;
}

Camera::Vector3 getEquirectPoint(
    const double x,
    const double y,
    const double depth,
    const double width,
    const double height) {
  double theta = -1 * ((x + 0.5) / width * 2 * M_PI);
  double phi = (y + 0.5) / height * M_PI;

  double cartX = depth * sin(phi) * cos(theta);
  double cartY = depth * sin(phi) * sin(theta);
  double cartZ = depth * cos(phi);
  return Camera::Vector3(cartX, cartY, cartZ);
}

Image createEquirect(
    const Camera::Rig& rig,
    const std::vector<Image>& images,
    const size_t height,
    const size_t width,
    const float depth) {
  Image equirectImage(height, width);
  for (double x = 0; x < width; x++) {
    for (double y = 0; y < height; y++) {
      Camera::Vector3 currentPoint = getEquirectPoint(x, y, depth, width, height);
      equirectImage(y, x) = getPixelColor(currentPoint, rig, images);
    }
  }
  return equirectImage;
}

Image createCroppedEquirect(
    const Camera::Rig& rig,
    const std::vector<Image>& images,
    const size_t height,
    const size_t width,
    const float depth) {
  double minX = width;
  double maxX = 0;
  double minY = height;
  double maxY = 0;
  for (double x = 0; x < width; x++) {
    for (double y = 0; y < height; y++) {
      Camera::Vector3 currentPoint = getEquirectPoint(x, y, depth, width, height);
      for (int currentCamera = 0; currentCamera < int(rig.size()); currentCamera++) {
        Camera::Vector2 currentPixel;
        if (rig[currentCamera].sees(currentPoint, currentPixel)) {
          minX = floor(std::min(minX, x));
          maxX = ceil(std::max(maxX, x));
          minY = floor(std::min(minY, y));
          maxY = ceil(std::max(maxY, y));
        }
      }
    }
  }

  const size_t newHeight = FLAGS_height;
  const size_t newWidth = FLAGS_height / (maxY - minY) * (maxX - minX);
  Image equirectImage(newHeight, newWidth);

  for (double x = 0; x < newWidth; x++) {
    for (double y = 0; y < newHeight; y++) {
      Camera::Vector3 currentPoint = getEquirectPoint(
          x * (maxX - minX) / newWidth + minX,
          y * (maxY - minY) / newHeight + minY,
          depth,
          width,
          height);

      equirectImage(y, x) = getPixelColor(currentPoint, rig, images);
    }
  }
  return equirectImage;
}

// Returns angle between two 3-vectors
double getRotationAngle(Camera::Vector3 v1, Camera::Vector3 v2, int signFactor) {
  double dotprod = v1.transpose() * v2;
  double magnitude = v1.norm() * v2.norm();

  return magnitude == 0 ? 0 : signFactor * acos(dotprod / magnitude);
}

// Rotate rig so that selected camera is facing towards centered in the equirect
void centerRig(Camera::Rig& rig, std::string camera_id) {
  const Camera::Vector3 centerOfEquirect(-1, 0, 0);
  const Camera::Vector3 upwards(0, 0, 1); // desired final upward orientation for camera
  const Camera::Vector3 translation(0, 0, 0);
  const Eigen::UniformScaling<double> scale(1);

  Camera::Real theta = 0; // angle around x axis
  Camera::Real psi = 0; // angle around y axis
  Camera::Real phi = 0; // angle around z axis

  // Yaw (rotation around z axis)
  const Camera selectedCamera = Camera::findCameraById(camera_id, rig);

  Camera::Vector3 projectedOnXY =
      Camera::Vector3(selectedCamera.forward().x(), selectedCamera.forward().y(), 0);

  int phiFactor = selectedCamera.forward().y() > 0 ? 1 : -1;
  phi = getRotationAngle(centerOfEquirect, projectedOnXY, phiFactor);

  const Camera::Vector3 rotation1(0, 0, phi);
  rig = transformRig(rig, rotation1, translation, scale);

  // Pitch (rotation around y axis)
  const Camera selectedCamera2 = Camera::findCameraById(camera_id, rig);

  Camera::Vector3 projectedOnXZ =
      Camera::Vector3(selectedCamera2.forward().x(), 0, selectedCamera2.forward().z());

  int psiFactor = selectedCamera2.forward().z() > 0 ? -1 : 1;
  psi = getRotationAngle(centerOfEquirect, projectedOnXZ, psiFactor);

  const Camera::Vector3 rotation2(0, psi, 0);
  rig = transformRig(rig, rotation2, translation, scale);

  // Roll (rotation around x axis)
  const Camera selectedCamera3 = Camera::findCameraById(camera_id, rig);

  Camera::Vector3 projectedOnYZ =
      Camera::Vector3(0, selectedCamera3.up().y(), selectedCamera3.up().z());

  int thetaFactor = selectedCamera3.up().y() > 0 ? 1 : -1;
  theta = getRotationAngle(upwards, projectedOnYZ, thetaFactor);

  const Camera::Vector3 rotation3(theta, 0, 0);
  rig = transformRig(rig, rotation3, translation, scale);
}

int main(int argc, char* argv[]) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_color, "");
  CHECK_NE(FLAGS_rig, "");
  CHECK_NE(FLAGS_output, "");

  // Load camera rig. This allows us to use all the goodies in
  // calibration/Camera.h and util/ImageUtil.h
  Camera::Rig rig = filterDestinations(Camera::loadRig(FLAGS_rig), FLAGS_cameras);

  if (!FLAGS_camera_id.empty()) {
    centerRig(rig, FLAGS_camera_id);
  }

  LOG(INFO) << "Loading images...";

  // Load input color images
  const std::vector<Image> images =
      loadScaledImages<cv::Vec4f>(FLAGS_color, rig, FLAGS_frame, FLAGS_scale);
  CHECK_GT(images.size(), 0) << "no images loaded!";
  CHECK_EQ(images.size(), rig.size());

  // Rescale cameras
  for (Camera& src : rig) {
    src = src.rescale(src.resolution * FLAGS_scale);
  }

  size_t height = FLAGS_height;
  size_t width = 2 * height;

  const float dispMin = 1.0f / FLAGS_depth_max;
  const float dispMax = 1.0f / FLAGS_depth_min;
  ThreadPool threadPool(FLAGS_threads);
  for (int i = FLAGS_num_depths - 1; i >= 0; --i) {
    threadPool.spawn([&, i] {
      const float fraction = float(i) / float(FLAGS_num_depths - 1);
      const float disp =
          FLAGS_num_depths == 1 ? dispMin : fraction * dispMin + (1 - fraction) * dispMax;
      const float depth = 1.0f / disp;
      LOG(INFO) << fmt::format("Depth {} of {}...", (FLAGS_num_depths - i), FLAGS_num_depths);

      Image equirectImage;
      if (FLAGS_crop_equirect) {
        equirectImage = createCroppedEquirect(rig, images, height, width, depth);
      } else {
        equirectImage = createEquirect(rig, images, height, width, depth);
      }
      saveImage(equirectImage, depth);
    });
  }
  threadPool.join();

  return EXIT_SUCCESS;
}
