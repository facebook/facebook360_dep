/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include <boost/filesystem.hpp>

#include "source/gpu/GlUtil.h"
#include "source/render/AsyncLoader.h"
#include "source/util/Camera.h"

namespace fb360_dep {

using MatrixDepth = Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

struct RigScene {
  explicit RigScene(
      const Camera::Rig& rig,
      const bool useMesh = true,
      const bool isDepthZCoord = false);
  explicit RigScene(
      const std::string& rigPath,
      const bool useMesh = true,
      const bool isDepthZCoord = false);
  RigScene(
      const std::string& rigPath,
      const std::string& imageDir,
      const std::string& depthDir,
      const bool useMesh = true,
      const bool isDepthZCoord = false);

  // construct a RigScene using depth and image data that is already in-memory.
  // assumes 'images' is a vector of flattened image data, T[4], RGBA order.
  // depthMaps may be a different resolution (lower) than images. this will be
  // rendered as a point cloud (not as a mesh).
  template <typename T>
  RigScene(
      const Camera::Rig& rig,
      std::vector<MatrixDepth>& depthMaps,
      const std::vector<std::vector<T>>& images,
      const std::vector<int>& imageWidths,
      const std::vector<int>& imageHeights);

  ~RigScene();

  RigScene(const RigScene&) = delete;
  RigScene& operator=(const RigScene&) = delete;
  RigScene(RigScene&&) = delete;
  RigScene& operator=(RigScene&&) = delete;

  const bool useMesh;
  const bool isDepthZCoord;

  GLuint cameraProgram;
  GLuint cameraMeshProgram;
  GLuint effectMeshProgram;
  GLuint updateProgram;
  GLuint resolveProgram;

  GLuint cameraFBO;
  GLuint cameraTexture;
  GLuint cameraDepth;

  GLuint accumulateFBO;
  GLuint accumulateTexture;

  bool forceMono = false; // used for demos to illustrate difference with 6dof
  bool renderBackground = true; // render separate background if available
  GLint debug = 0; // flags for debugging
  float effect = 0; // effect parameters
  bool isDepth;

  Camera::Rig rig;

  // a frame consists of a subframe from each camera
  struct Subframe {
    Subframe() : colorTexture(0) {}

    bool isValid() const {
      return colorTexture != 0;
    }

    GLuint vertexArray;
    GLsizei indexCount;
    GLvoid* indexOffset = nullptr;
    GLuint colorTexture;
    Eigen::Vector2i size;
  };
  std::vector<Subframe> subframes;
  std::vector<Subframe> backgroundSubframes;

  std::vector<GLuint> directionTextures;

  void createFramebuffers(const int w, const int h);
  void destroyFramebuffers();

  void createPrograms();
  void destroyPrograms();

  GLuint createDirection(const Camera& camera);

  Subframe createSubframe(
      const Camera& camera,
      const GLuint buffer,
      const uint64_t offset,
      const folly::dynamic& layout) const;
  Subframe createSubframe(
      const std::string& id,
      const std::string& imageDir,
      const std::string& depthDir) const;
  Subframe createPointCloudSubframeFromData(
      const uint8_t* colorData,
      uint16_t* depthData,
      const int colorWidth,
      const int colorHeight,
      const int depthWidth,
      const int depthHeight,
      const float depthScale) const;
  std::vector<Subframe> createFrame(const std::string& imageDir, const std::string& depthDir) const;
  static void destroyFrame(std::vector<Subframe>& subframes);

  // render a fullscreen triangle
  static void fullscreen(GLuint program, GLuint texture, GLuint target = GL_TEXTURE_2D);

  void clearSubframe() const;
  GLuint getProgram() const;
  GLint clearAccumulation();
  void updateAccumulation() const;
  void resolveAccumulation(GLint fbo, float fade = 1.0f) const;
  void updateTransform(const Eigen::Matrix4f& transform) const;

  mutable std::vector<bool> culled;

  void render(
      const Eigen::Matrix4f& projview,
      const float displacementMeters = 0,
      const bool doCulling = true,
      const bool wireframe = false);

  static GLenum getInternalRGBAFormat(const uint8_t& /*unused*/) {
    return GL_SRGB8_ALPHA8;
  }
  static GLenum getInternalRGBAFormat(const uint16_t& /*unused*/) {
    return GL_RGBA16;
  }
  static GLenum getInternalRGBAFormat(const float& /*unused*/) {
    return GL_RGBA32F;
  }

 private:
  template <typename T>
  void createSubframes(
      const Camera::Rig& rig,
      std::vector<MatrixDepth>& depthMaps,
      const std::vector<std::vector<T>>& images,
      const std::vector<int>& imageWidths,
      const std::vector<int>& imageHeights);
  void renderSubframe(const int subframeIndex, const bool wireframe = false) const;
};

} // namespace fb360_dep
