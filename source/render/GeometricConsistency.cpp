/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cmath>
#include <future>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <opencv2/opencv.hpp>
#include <unsupported/Eigen/CXX11/Tensor>

#include <folly/Format.h>

#include "source/gpu/GlfwUtil.h"
#include "source/gpu/ReprojectionGpuUtil.h"
#include "source/util/Camera.h"
#include "source/util/CvUtil.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;

const std::string kUsageMessage = R"(
- Compute initial depth for every camera
- Repeat pass_count times:
  - Clean away depths that are implausible
  - Recompute depths using clean depths to estimate occlusions

- Example:
    GeometricConsistency \
    --color /path/to/color \
    --output /path/to/output \
    --rig /path/to/rigs/rig.json \
    --first 000000 \
    --last 000000
)";

DEFINE_double(agree_fraction, 0.75, "fraction considered in agreement");
DEFINE_string(color, "", "color directory (required)");
DEFINE_double(disparity_step, 0.5, "pixels per disparity step");
DEFINE_double(downscale, 4, "reduced resolution output");
DEFINE_string(first, "", "first frame to process (lexical)");
DEFINE_bool(keep_clean, false, "only recompute implausible depths");
DEFINE_string(last, "", "last frame to process (lexical)");
DEFINE_int32(median, 0, "radius of median filter applied to input");
DEFINE_string(output, "", "output subdirectory (required)");
DEFINE_int32(pass_count, 2, "how many times to refine depth");
DEFINE_string(rig, "", "path to rig .json file (required)");
DEFINE_string(single, "", "render a single destination camera");

// Image is 16-bit RGBA
using Pixel = cv::Vec4w;
using Image = cv::Mat_<Pixel>;

const int kSignedMax = INT16_MAX;
using SignedPixel = cv::Vec<int16_t, 4>;

void dump(const filesystem::path& path, const cv::Mat_<float>& mat) {
  fb360_dep::cv_util::writeCvMat32FC1ToPFM(path.string() + ".pfm", mat);
  // for convenience, also dump 1.0 m / mat as png
  cv::Mat_<float> disparity = 1.0 / mat;
  cv_util::imwriteExceptionOnFail(
      path.string() + "_disparity.png", cv_util::convertTo<uint16_t>(disparity));
}

// Depth is fp32
using DepthMat = cv::Mat_<float>;

template <typename T>
std::vector<GLuint> createTextures(
    const std::vector<T>& mats,
    const GLenum internalFormat, // e.g. GL_RGB8, GL_SRGB8_ALPHA8, GL_RGBA16F
    const GLenum format, // e.g. GL_RED, GL_RGB, GL_BGRA
    const GLenum type) { // e.g. GL_UNSIGNED_BYTE, GL_FLOAT
  std::vector<GLuint> result;
  for (const T& mat : mats) {
    result.push_back(createTexture(
        mat.cols,
        mat.rows,
        mat.ptr(),
        internalFormat,
        format,
        type,
        true)); // buildMipmaps = true
    setTextureAniso();
    setTextureWrap(GL_CLAMP_TO_BORDER);
  }
  return result;
}

static Camera::Real computeRigRadius(const Camera::Rig& rig) {
  Camera::Real sum = 0;
  for (const Camera& camera : rig) {
    sum += camera.position.norm();
  }
  return sum / rig.size();
}

template <typename T>
T sq(const T& t) {
  return t * t;
}

float sumNorm(const SignedPixel& rgba) {
  float sum = 0;
  for (int c = 0; c < 3; ++c) { // exclude alpha
    sum += float(rgba[c]);
  }
  return sum / kSignedMax;
}

float sumSqNorm(const SignedPixel& rgba) {
  float sum = 0;
  for (int c = 0; c < 3; ++c) { // exclude alpha
    sum += sq(float(rgba[c]));
  }
  return sum / (kSignedMax * kSignedMax);
}

Camera::Real sliceDisparity(const int slice, const int sliceCount) {
  return ReprojectionTable::unnormalizeDisparity((slice + 0.5) / sliceCount);
}

using Volume = Eigen::Tensor<float, 3, Eigen::RowMajor>;

DepthMat winnerTakesAll(const Volume& costs) {
  // find cheapest depth for each x, y in costs (NAN if everything is NAN)
  DepthMat depth(costs.dimension(1), costs.dimension(2), NAN);
  Eigen::MatrixXf best(depth.rows, depth.cols);
  best.setConstant(FLT_MAX);
  for (int d = 0; d < costs.dimension(0); ++d) {
    for (int y = 0; y < costs.dimension(1); ++y) {
      for (int x = 0; x < costs.dimension(2); ++x) {
        if (best(y, x) > costs(d, y, x)) { // note: (x > NAN) is alwyas false
          best(y, x) = costs(d, y, x);
          depth(y, x) = 1 / sliceDisparity(d, costs.dimension(0));
        }
      }
    }
  }

  // NAN out the edges
  for (int y = 0; y < costs.dimension(1); ++y) {
    depth(y, 0) = depth(y, costs.dimension(2) - 1) = NAN;
  }
  for (int x = 0; x < costs.dimension(2); ++x) {
    depth(0, x) = depth(costs.dimension(1) - 1, x) = NAN;
  }
  return depth;
}

cv::Mat_<SignedPixel> computeReference(const Image& image, int w, int h) {
  Image resized = cv_util::resizeImage(image, {w, h});
  cv::Mat_<SignedPixel> reference;
  resized.convertTo(reference, CV_16S, INT16_MAX / double(UINT16_MAX));
  return reference;
}

DepthMat computeDepth(
    const Camera::Rig& rig, // rig determines the resolution of the result
    const int d,
    const std::vector<Image>& images, // full resolution images
    const std::vector<GLuint>& imageTextures,
    const std::vector<DepthMat>& depths = {}, // could be same rez as rig, or not
    const std::vector<GLuint>& depthTextures = {}) {
  const Camera& dst = rig[d];
  LOG(INFO) << folly::sformat("compute depth for {}", dst.id);

  // compute reprojection textures
  std::vector<ReprojectionTexture> reprojections;
  for (const Camera& src : rig) {
    reprojections.emplace_back(dst, src);
  }

  // downsample destination image
  const int w = dst.resolution.x();
  const int h = dst.resolution.y();
  cv::Mat_<SignedPixel> reference = computeReference(images[d], w, h);

  // compute how many slices we need
  const Camera::Real radius = computeRigRadius(rig);
  const Camera::Real minDistance = 1 / ReprojectionTable::maxDisparity();
  const Camera::Real angle = asin(radius / minDistance);
  const Camera::Real focal = dst.focal.norm() * sqrt(0.5);
  const Camera::Real pixels = focal * angle;
  const int sliceCount = std::round(pixels / FLAGS_disparity_step);

  // compute each slice of the cost volume
  Volume costs(sliceCount, h, w);
  for (int slice = 0; slice < sliceCount; ++slice) {
    Camera::Real disparity = sliceDisparity(slice, sliceCount);
    LOG(INFO) << folly::sformat("slice {}/{} ({})", slice, sliceCount, disparity);

    // accumulate each source cost into accum
    cv::Mat_<cv::Vec2f> accum(h, w, cv::Vec2f(0, 0));
    for (int s = 0; s < int(rig.size()); ++s) {
      if (s == d) {
        continue; // don't compare destination to itself
      }

      // compute src color at disparity by reprojection
      cv::Mat_<SignedPixel> image = reproject<SignedPixel>(
          w, h, GL_RGBA16, GL_RGBA, GL_SHORT, reprojections[s], imageTextures[s], disparity);

      // alpha away occluded areas if we have depth information
      if (!depths.empty()) {
        DepthMat depth = reproject<float>(
            w, h, GL_R32F, GL_RED, GL_FLOAT, reprojections[s], depthTextures[s], disparity);
        for (int y = 0; y < h; ++y) {
          for (int x = 0; x < w; ++x) {
            if (!std::isnan(depth(y, x))) {
              const Camera::Vector3 world = dst.rig({x, y}, 1 / disparity);
              const Camera::Real distance = (world - rig[s].position).norm();
              if (depth(y, x) < distance * FLAGS_agree_fraction) {
                image(y, x)[3] = 0; // src is occluded
              }
            }
          }
        }
      }

      // compute average of difference
      cv::Mat_<SignedPixel> diff = image - reference;
      const cv::Size box(3, 3);
      cv::Mat_<SignedPixel> average;
      cv::blur(diff, average, box);

      // compute average of diff^2
      cv::Mat_<SignedPixel> averageOfSq;
      cv::blur(diff.mul(diff, 1.0 / kSignedMax), averageOfSq, box);

      // cost += variance of diff = average of diff^2 - (average of diff)^2
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          if (image(y, x)[3] == kSignedMax && average(y, x)[3] == 0) {
            accum(y, x) += cv::Vec2f(sumNorm(averageOfSq(y, x)) - sumSqNorm(average(y, x)), 1);
          }
        }
      }
    }

    // transfer accumulated fraction to cost
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        costs(slice, y, x) = accum(y, x)[0] / accum(y, x)[1];
      }
    }
  }

  // winner takes all
  return winnerTakesAll(costs);
}

bool isPointBad(
    const Camera::Vector3& world,
    const Camera::Rig& rig,
    const int d,
    const std::vector<DepthMat>& depths) {
  // go through source cameras and see whether any disagree
  for (int s = 0; s < int(rig.size()); ++s) {
    if (s == d) {
      continue; // don't check dst against itself
    }

    // calculate pixel in src camera that sees world point
    const Camera& src = rig[s];
    Camera::Vector2 pixel;
    if (!src.sees(world, pixel)) {
      continue;
    }

    // calculate depth for that pixel in src camera
    const DepthMat& depth = depths[s];
    CHECK_EQ(src.resolution.x(), depth.cols);
    CHECK_EQ(src.resolution.y(), depth.rows);
    const float srcDepth = depth(pixel.y(), pixel.x()); // nearest

    // calculate distance from src camera to world point
    const float proposal = (world - src.position).norm();
    if (proposal < srcDepth * FLAGS_agree_fraction) {
      return true; // proposal is closer than src, reject
    }
  }
  return false;
}

DepthMat cleanDepth(const Camera::Rig& rig, const int d, const std::vector<DepthMat>& depths) {
  const Camera& dst = rig[d];
  LOG(INFO) << folly::sformat("cleaning {}", dst.id);

  // NaN out depths that other cameras disagree with
  DepthMat depth = depths[d].clone();
  CHECK_EQ(dst.resolution.x(), depth.cols);
  CHECK_EQ(dst.resolution.y(), depth.rows);
  for (int y = 0; y < depth.rows; ++y) {
    for (int x = 0; x < depth.cols; ++x) {
      Camera::Vector3 world = dst.rig({x + 0.5, y + 0.5}, depth(y, x));
      if (isPointBad(world, rig, d, depths)) {
        depth(y, x) = NAN;
      }
    }
  }
  return depth;
}

// copy every non-nan depth from cleanDepth into depth
void restoreCleanDepth(DepthMat& depth, const DepthMat& cleanDepth) {
  for (int y = 0; y < depth.rows; ++y) {
    for (int x = 0; x < depth.cols; ++x) {
      float value = cleanDepth(y, x);
      if (!std::isnan(value)) {
        depth(y, x) = value;
      }
    }
  }
}

Camera::Rig downscale(const Camera::Rig& rig) {
  Camera::Rig result;
  for (const Camera& camera : rig) {
    Camera::Vector2 resolution = camera.resolution / FLAGS_downscale;
    result.push_back(camera.rescale(resolution.array().round()));
  }
  return result;
}

void processFrame(const std::string& frameName, const Camera::Rig& rig) {
  const filesystem::path path = filesystem::path(FLAGS_output) / frameName;
  filesystem::create_directory(path);

  // load images
  const std::vector<Image> images = image_util::loadImages<Pixel>(FLAGS_color, rig, frameName);
  const std::vector<GLuint> imageTextures =
      createTextures(images, GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT);

  // compute initial depth estimate
  Camera::Rig small = downscale(rig);
  std::vector<DepthMat> depths(small.size());
  for (int d = 0; d < int(small.size()); ++d) {
    depths[d] = computeDepth(small, d, images, imageTextures);
    dump(path / folly::sformat("{}_iffy", small[d].id), depths[d]);
  }

  // refine depth estimate
  for (int pass = 0; pass < FLAGS_pass_count; ++pass) {
    // compute clean depths by getting rid of improbable ones
    std::vector<DepthMat> cleanDepths;
    for (int d = 0; d < int(small.size()); ++d) {
      cleanDepths.push_back(cleanDepth(small, d, depths));
      dump(path / folly::sformat("{}_{}_clean", small[d].id, pass), cleanDepths[d]);
    }
    const std::vector<GLuint> cleanDepthTextures =
        createTextures(cleanDepths, GL_R32F, GL_RED, GL_FLOAT);

    // recompute depth using cleaned depths
    for (int d = 0; d < int(small.size()); ++d) {
      depths[d] = computeDepth(small, d, images, imageTextures, cleanDepths, cleanDepthTextures);
      dump(path / folly::sformat("{}_{}", small[d].id, pass), depths[d]);
    }

    // restore clean depths
    if (FLAGS_keep_clean) {
      for (int d = 0; d < int(small.size()); ++d) {
        restoreCleanDepth(depths[d], cleanDepths[d]);
      }
    }

    // free clean depth textures
    for (const GLuint& texture : cleanDepthTextures) {
      glDeleteTextures(1, &texture);
    }
  }

  // free image textures
  for (const GLuint& texture : imageTextures) {
    glDeleteTextures(1, &texture);
  }
}

class OffscreenWindow : public GlWindow {
 protected:
  Camera::Rig& rig;

 public:
  OffscreenWindow(Camera::Rig& rig) : GlWindow::GlWindow(), rig(rig) {
    filesystem::create_directory(FLAGS_output);
  }

  void display() override {
    const int numFrames = std::stoi(FLAGS_last) - std::stoi(FLAGS_first) + 1;
    for (int iFrame = 0; iFrame < numFrames; ++iFrame) {
      const std::string frameName =
          image_util::intToStringZeroPad(iFrame + std::stoi(FLAGS_first), 6);
      LOG(INFO) << folly::sformat("Processing frame {}", frameName);

      processFrame(frameName, rig);
    }
  }
};

int main(int argc, char* argv[]) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_rig, "");
  CHECK_NE(FLAGS_color, "");
  CHECK_NE(FLAGS_output, "");

  // prepare for offscreen rendering
  Camera::Rig rig = Camera::loadRig(FLAGS_rig);
  OffscreenWindow offscreenWindow(rig);

  // Render frames
  offscreenWindow.display();

  return EXIT_SUCCESS;
}
