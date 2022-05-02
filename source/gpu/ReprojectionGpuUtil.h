/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/gpu/GpuBuffers.h"
#include "source/render/ReprojectionTexture.h"

namespace fb360_dep {

// reproject a source camera into a dst camera at constant disparity

// example usage (see reproject convenience function below):
//  GpuBuffers buffers(GL_RGBA16, area);
//  ReprojectionTexture reprojection(dst, src);
//  ReprojectionScene scene(reprojection, srcTexture, disparity);
//  buffers.subdivide(scene);
//  cv::Mat_<cv::Vec<uint16_t, 4>> mat(area.y(), area.x());
//  ... ideally let the GPU finish before reading back ...
//  buffers.read(&mat(0, 0), GL_RGBA, GL_UNSIGNED_SHORT);
struct ReprojectionScene {
  ReprojectionScene(
      const ReprojectionTexture& reprojection,
      const GLuint srcTexture,
      const float disparity)
      : program(createProgram(reprojection)) {
    connectUnitWithTextureAndUniform(
        0, GL_TEXTURE_3D, reprojection.texture, program, "reprojectionTexture");
    connectUnitWithTextureAndUniform(1, GL_TEXTURE_2D, srcTexture, program, "srcTexture");
    setDisparity(disparity);
  }

  ~ReprojectionScene() {
    glDeleteProgram(program);
  }

  using Coor = GpuBuffers::Coor;

  bool render(GpuBuffers& dst, const Coor& begin, const Coor& size) const {
    dst.render(program);
    return true; // success
  }

  const GLuint program;

  void setDisparity(const float disparity) {
    setUniform(program, "disparity", disparity);
  }

 private:
  static GLuint createProgram(const ReprojectionTexture& reprojection) {
    const std::string fragmentShader = R"(
      #version 330 core

      uniform vec3 reprojectionScale;
      uniform vec3 reprojectionOffset;

      uniform sampler3D reprojectionTexture;
      uniform sampler2D srcTexture;

      uniform float disparity;

      in vec2 dstCoor;
      out vec4 result;

      vec2 reproject(vec2 dst) {
        return texture(
          reprojectionTexture,
          vec3(dst, disparity) * reprojectionScale + reprojectionOffset).xy;
      }

      void main() {
        vec2 srcCoor = reproject(dstCoor);
        result = texture(srcTexture, srcCoor);
      }
    )";
    GLuint program = ::createProgram(fullscreenVertexShader("tex", "dstCoor"), fragmentShader);
    glUniform3fv(getUniformLocation(program, "reprojectionScale"), 1, reprojection.scale.data());
    glUniform3fv(getUniformLocation(program, "reprojectionOffset"), 1, reprojection.offset.data());
    return program;
  }
};

// This is convenient, but note: It blocks until GPU is done
template <typename T>
cv::Mat_<T> reproject(
    const int width,
    const int height,
    const GLenum internalFormat, // e.g. GL_RGB8, GL_SRGB8_ALPHA8, GL_RGBA16F
    const GLenum format, // e.g. GL_RED, GL_RGB, GL_BGRA
    const GLenum type, // e.g. GL_UNSIGNED_BYTE, GL_FLOAT
    const ReprojectionTexture& reprojection,
    const GLuint srcTexture,
    const float disparity) {
  GpuBuffers buffers(internalFormat, {width, height});
  ReprojectionScene scene(reprojection, srcTexture, disparity);
  buffers.subdivide(scene);
  cv::Mat_<T> result(height, width);
  // this blocks until the GPU is done
  buffers.read(&result(0, 0), format, type);
  return result;
}

} // namespace fb360_dep
