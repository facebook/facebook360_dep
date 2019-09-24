/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <opencv2/core/core.hpp>

#include "source/gpu/GlUtil.h"
#include "source/util/Camera.h"

namespace fb360_dep {

// a canopy is the bumpy half-dome described by a camera's disparity and color images
struct Canopy {
  Canopy(const cv::Mat_<cv::Vec4f>& color, const cv::Mat_<cv::Vec3f>& mesh, GLuint program);
  void destroy();

  void render(
      GLuint framebuffer,
      const Eigen::Projective3f& transform,
      const GLuint program,
      const float ipd = 0.0f) const;

 private:
  int modulo;
  Eigen::Vector2f scale;
  GLuint vertexArray;
  GLuint colorTexture;
  GLuint positionBuffer;
  GLuint indexBuffer;
};

struct CanopyScene {
  CanopyScene(
      const Camera::Rig& cameras,
      const std::vector<cv::Mat_<float>>& disparities,
      const std::vector<cv::Mat_<cv::Vec4f>>& colors,
      const bool onScreen = true);
  ~CanopyScene();

  // render scene to the specified opengl framebuffer
  void render(
      const GLuint framebuffer,
      const Eigen::Projective3f& transform,
      const float ipd = 0.0f,
      const bool alphaBlend = true) const;

  // render scene from position as a cubemap with edge x edge pixel faces, stacked vertically
  cv::Mat_<cv::Vec4f> cubemap(
      int edge,
      const Eigen::Vector3f& position = {0, 0, 0},
      const float ipd = 0.0f,
      const bool alphaBlend = true) const;

  // render scene from position as an equirect that is height pixels tall and twice as wide
  cv::Mat_<cv::Vec4f> equirect(
      int height,
      const Eigen::Vector3f& position = {0, 0, 0},
      const float ipd = 0.0f,
      const bool alphaBlend = true) const;

 private:
  std::vector<Canopy> canopies;

  // programs
  GLuint canopyProgram;
  GLuint accumulateProgram;
  GLuint unpremulProgram;
};

} // namespace fb360_dep
