/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "source/gpu/GlUtil.h"

// framework for rendering a volume consisting of width x height x depth pixels
struct GpuBuffers : public std::vector<GLuint> {
  using Coor = Eigen::Array2i;

  // format must be en opengl enum e.g. GL_RGBA16F for 4 channels of fp16
  GpuBuffers(GLenum format, const Coor& area, int depth = 1) : area(area) {
    resize(depth);
    for (GLuint& buffer : *this) {
      buffer = createRenderbuffer(area.x(), area.y(), format);
    }
  }

  ~GpuBuffers() {
    for (const GLuint& buffer : *this) {
      glDeleteRenderbuffers(1, &buffer);
    }
  }

  // moveable but not copyable
  GpuBuffers(GpuBuffers&& rhs) : area(rhs.area) {
    std::vector<GLuint>::swap(rhs);
  }

  // maximum number of buffers to render at a time
  static size_t maxDrawBufferCount() {
    // determine maximum number of draw buffers
    GLint result;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &result);
    return result;
  }

  // render all buffers using multiple draw buffers (aka. MRT, Multiple Render
  // Targets) to minimize draw calls
  // sets uniforms bufferBegin, bufferCount, bufferTotal to describe the subset
  // of buffers that the draw buffers represent
  void render(const int program) {
    // go through count buffers at a time
    const size_t count = maxDrawBufferCount();
    for (int bufferBegin = 0; bufferBegin < int(size()); bufferBegin += count) {
      // connect buffer bufferBegin + i to GL_COLOR_ATTACHMENTi
      std::vector<GLenum> drawBuffers(std::min(count, size() - bufferBegin));
      for (int i = 0; i < int(drawBuffers.size()); ++i) {
        drawBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
        glFramebufferRenderbuffer(
            GL_FRAMEBUFFER, drawBuffers[i], GL_RENDERBUFFER, at(bufferBegin + i));
      }
      glDrawBuffers(drawBuffers.size(), drawBuffers.data());

      // tell program which subset of buffers it is drawing, if it cares
      if (glGetUniformLocation(program, "bufferBegin") != -1) {
        setUniform(program, "bufferBegin", bufferBegin);
        setUniform(program, "bufferCount", GLint(drawBuffers.size()));
        setUniform(program, "bufferTotal", GLint(size()));
      }

      // do the actual drawing
      fullscreen(program, "tex");
    }
  }

  // subdivide rectangle until scene.render() returns true
  template <typename T>
  int subdivide(const T& scene) {
    glViewport(0, 0, area.x(), area.y());
    return subdivide(scene, Coor(0, 0), area);
  }

  // format/type must be opengl enums e.g. GL_RGBA/GL_HALF_FLOAT matching T
  template <typename T>
  void read(T* dst, GLenum format, GLenum type, int index = 0) const {
    CHECK_EQ(getChannelCount(format) * getByteCount(type), sizeof(T));
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, at(index));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, area.x(), area.y(), format, type, dst);
  }

  const Coor area;

 private:
  // subdivide rectangle until scene.render() returns true
  template <typename T>
  int subdivide(const T& scene, const Coor& begin, const Coor& size) {
    // setup scissor and attempt to render scene
    glScissor(begin.x(), begin.y(), size.x(), size.y());
    glEnable(GL_SCISSOR_TEST);
    if (scene.render(*this, begin, size)) {
      return 1;
    }
    // unable to render scene: subdivide the rectangle into two pieces by
    // cutting the major axis in half, then recurse to divide and conquer

    // if the major axis is horizontal, this is what that looks like:
    //              ^-- major / 2 -->--------------+
    //   minor =    |               |              |
    // size - major |               |              |
    //              |               |              |
    //              +------------ major ----------->
    CHECK(size.x() > 1 || size.y() > 1) << "unable to subdivide further";
    Coor major = size.x() > size.y() ? Coor(size.x(), 0) : Coor(0, size.y());
    return subdivide(scene, begin, (size - major) + major / 2) +
        subdivide(scene, begin + major / 2, size - major / 2);
  }
};

#if 0

// a functor for subdivide might look like this
struct Scene {
  using Coor = GpuBuffers::Coor;
  bool render(GpuBuffers& dst, const Coor& begin, const Coor& size) const {
    static bool ok = ...; // determine whether you can render this portion
    if (!ok) {
      return false;  // failed to render, request subdivision
    }
    GLuint program = ...; // create program
    // set up textures, uniforms, etc.
    dst.render(program);
    return true;  // success
  }
};

// use like so:
GpuBuffers buffers;
Scene scene;
buffers.subdivide(scene);

#endif
