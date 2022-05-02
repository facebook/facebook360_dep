/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#ifdef WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// includes below need to be in the following order:
#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
// windows.h
#include <windows.h>
#ifdef FB360_DEP_USE_OVR_CAPI_GLE
#include <GL/CAPI_GLE.h>
#else
#include <GL/glew.h>
#endif
#define GL_HALF_FLOAT 0x140B
#define GL_RG 0x8227

#include <GL/gl.h>
#endif

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#include <OpenGL/glext.h>
#endif

#ifdef __linux__
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

#define GL_GLES_PROTOTYPES 1
#include <GLES3/gl3.h>
#endif

#define __gl_h_ // block older GL

#include <glog/logging.h>
#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <array>
#include <fstream>
#include <string>

#include <folly/Format.h>

inline Eigen::Projective3f
frustum(float minX, float maxX, float minY, float maxY, float minZ, float maxZ = INFINITY) {
  // see glFrustum: http://goo.gl/fsgoMf
  float A = (maxX + minX) / (maxX - minX);
  float B = (maxY + minY) / (maxY - minY);
  float C = -(maxZ + minZ) / (maxZ - minZ);
  float D = -2 * maxZ * minZ / (maxZ - minZ);

  // fix special case maxZ == inf
  if (std::isinf(maxZ)) {
    C = -1;
    D = -2 * minZ;
  }

  Eigen::Matrix4f m;
  m << 2 * minZ / (maxX - minX), 0, A, 0, 0, 2 * minZ / (maxY - minY), B, 0, 0, 0, C, D, 0, 0, -1,
      0;

  return Eigen::Projective3f(m);
}

// produce a triangle strip covering a width x height grid
inline std::vector<GLuint> stripify(int width, int height, int skip = 1) {
  std::vector<GLuint> result;
  for (int y = 0; y + skip < height; y += skip) {
    result.push_back(y * width); // double-hit the first index
    for (int x = 0; x < width; x += skip) {
      result.push_back(y * width + x);
      result.push_back((y + skip) * width + x);
    }
    result.push_back(result.back()); // double-hit the last index
  }
  return result;
}

inline void attachShader(GLuint program, const GLint type, const std::string& source) {
  GLuint shader = glCreateShader(type);
  const char* p = source.c_str();
  glShaderSource(shader, 1, &p, NULL);
  glCompileShader(shader);
  GLint status;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (!status) {
    GLint length;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::vector<GLchar> log(length);
    glGetShaderInfoLog(shader, length, &length, &log[0]);
    LOG(FATAL) << folly::sformat("{}\nsource:\n{}", log.data(), source);
  }
  glAttachShader(program, shader);
  glDeleteShader(shader); // ok: won't actually be deleted until detached
}

inline GLuint createProgram(const std::string& vs, const std::string& fs) {
  GLuint program = glCreateProgram();
  attachShader(program, GL_VERTEX_SHADER, vs);
  attachShader(program, GL_FRAGMENT_SHADER, fs);
  glLinkProgram(program);
  GLint status;
  glGetProgramiv(program, GL_LINK_STATUS, &status);
  if (!status) {
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::vector<GLchar> log(length);
    glGetProgramInfoLog(program, length, &length, &log[0]);
    LOG(FATAL) << folly::sformat("{}\nvs:\n{}\nfs\n{}", log.data(), vs, fs);
  }
  glUseProgram(program);

  return program;
}

inline GLint getUniformLocation(GLuint program, const char* name) {
  GLint result = glGetUniformLocation(program, name);
  CHECK_NE(result, -1) << "can't find uniform '" << name << "'";
  return result;
}

inline void setUniform(GLuint program, const char* name, const GLint& value) {
  glUniform1i(getUniformLocation(program, name), value);
}

inline void setUniform(GLuint program, const char* name, const float& value) {
  glUniform1f(getUniformLocation(program, name), value);
}

inline void setUniform(GLuint program, const char* name, const float& x, const float& y) {
  glUniform2f(getUniformLocation(program, name), x, y);
}

// connect texture unit with target/texture and program/uniform[index]
inline void connectUnitWithTextureAndUniform(
    const GLuint unit,
    const GLenum target,
    const GLuint texture,
    const GLuint program,
    const char* uniform,
    int index = 0) {
  glActiveTexture(GL_TEXTURE0 + unit);
  glBindTexture(target, texture);
  glUniform1i(getUniformLocation(program, uniform) + index, unit);
}

inline void connectUnitWith2DTextureAndUniform(
    const GLuint unit,
    const GLuint texture,
    const GLuint program,
    const char* uniform,
    int index = 0) {
  connectUnitWithTextureAndUniform(unit, GL_TEXTURE_2D, texture, program, uniform, index);
}

inline GLint getAttribLocation(GLuint program, const char* name) {
  GLint result = glGetAttribLocation(program, name);
  CHECK_NE(result, -1) << "can't find attribute '" << name << "'";
  return result;
}

template <typename T>
GLuint createBuffer(GLenum target, const T* p, size_t count) {
  GLuint buffer;
  glGenBuffers(1, &buffer);
  glBindBuffer(target, buffer);
  glBufferData(target, count * sizeof(T), p, GL_STREAM_DRAW);
  return buffer;
}

// should work for any v that is contiguous, has size() and operator[]
template <typename T>
GLuint createBuffer(GLenum target, const T& v) {
  return createBuffer(target, &v[0], v.size());
}

inline GLenum getType(const uint8_t&) {
  return GL_UNSIGNED_BYTE;
}
inline GLenum getType(const GLushort&) {
  return GL_UNSIGNED_SHORT;
}
inline GLenum getType(const GLuint&) {
  return GL_UNSIGNED_INT;
}
inline GLenum getType(const float&) {
  return GL_FLOAT;
}

inline int getByteCount(GLenum type) {
  switch (type) {
    case GL_UNSIGNED_BYTE:
    case GL_BYTE:
      return 1;
    case GL_UNSIGNED_SHORT:
    case GL_SHORT:
    case GL_HALF_FLOAT:
      return 2;
    case GL_UNSIGNED_INT:
    case GL_INT:
    case GL_FLOAT:
      return 4;
  }
  CHECK(false) << "unexpected type " << type;
}

inline int getChannelCount(GLenum format) {
  switch (format) {
    case GL_RED:
    case GL_GREEN:
    case GL_BLUE:
    case GL_ALPHA:
      return 1;
    case GL_RG:
      return 2;
    case GL_RGB:
      return 3;
    case GL_RGBA:
      return 4;
  }
  CHECK(false) << "unexpected format " << format;
}

// should work for any T which has size() and operator[]
template <typename T>
GLuint createVertexAttributes(GLuint location, const T* p, size_t count) {
  GLuint buffer = createBuffer(GL_ARRAY_BUFFER, p, count);
  glVertexAttribPointer(
      location,
      (GLint)(*p).size(), // dimension
      getType((*p)[0]), // type
      GL_TRUE, // normalized
      0, // stride (0 means inferred)
      (GLvoid*)0); // offset
  glEnableVertexAttribArray(location);
  return buffer;
}

template <typename T>
GLuint createVertexAttributes(GLuint location, const T& v) {
  return createVertexAttributes(location, &v[0], v.size());
}

template <typename T>
GLuint createVertexAttributes(GLuint program, const char* name, const T& v) {
  return createVertexAttributes(getAttribLocation(program, name), v);
}

template <typename T>
GLuint createVertexAttributes(GLuint program, const char* name, const T* p, size_t count) {
  return createVertexAttributes(getAttribLocation(program, name), p, count);
}

template <typename T>
void drawElements(GLenum mode, uint32_t count) {
  glDrawElements(mode, count, getType(T()), 0);
}

template <typename T>
void drawElements(GLenum mode) {
  GLint bytes;
  glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &bytes);
  glDrawElements(mode, bytes / sizeof(T), getType(T()), 0);
}

// should work for any v which can be used in createBuffer()
template <typename T>
void drawElements(GLenum mode, const T& v) {
  static_assert(std::is_same<decltype(v[0]), const GLuint&>::value, "TODO");
  GLuint buffer = createBuffer(GL_ELEMENT_ARRAY_BUFFER, v);
  glDrawElements(mode, v.size(), GL_UNSIGNED_INT, 0);
  glDeleteBuffers(1, &buffer);
}

inline GLuint createVertexArray() {
  GLuint vertexArray;
  glGenVertexArrays(1, &vertexArray);
  glBindVertexArray(vertexArray);

  return vertexArray;
}

inline GLuint createFramebuffer(GLenum target = GL_FRAMEBUFFER) {
  GLuint framebuffer;
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(target, framebuffer);

  return framebuffer;
}

inline GLuint createTexture(GLenum target = GL_TEXTURE_2D) {
  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(target, texture);

  return texture;
}

template <GLenum target = GL_TEXTURE_2D>
inline void setLinearFiltering(bool buildMipmaps = false) {
  // Don't build mip maps unless asked
  if (buildMipmaps) {
    glGenerateMipmap(target);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  } else {
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  }
  // this is the default already, but ...
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

template <GLenum target = GL_TEXTURE_2D>
inline void setTextureWrap(GLenum mode) {
  glTexParameteri(target, GL_TEXTURE_WRAP_S, mode);
  glTexParameteri(target, GL_TEXTURE_WRAP_T, mode);
}

// turn on anisotropic filtering, if available (0 means maximum supported)
template <GLenum target = GL_TEXTURE_2D>
inline void setTextureAniso(GLint aniso = 0) {
#ifdef GL_TEXTURE_MAX_ANISOTROPY_EXT
  if (aniso == 0) { // query the maximum value and set that
    glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &aniso);
  }
  glTexParameteri(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
#endif
}

inline GLuint createTexture(
    const int width,
    const int height,
    const void* data,
    const GLenum internalFormat, // e.g. GL_RGB8, GL_SRGB8_ALPHA8, GL_RGBA16F
    const GLenum format, // e.g. GL_RED, GL_RGB, GL_BGRA
    const GLenum type, // e.g. GL_UNSIGNED_BYTE, GL_FLOAT
    const bool buildMipmaps = false) {
  GLuint texId = createTexture();
  glTexImage2D(
      GL_TEXTURE_2D,
      0, // level
      internalFormat,
      width,
      height,
      0, // border
      format,
      type,
      data);
  setLinearFiltering(buildMipmaps);
  return texId;
}

inline GLuint createRenderbuffer(GLenum target = GL_RENDERBUFFER) {
  GLuint renderbuffer;
  glGenRenderbuffers(1, &renderbuffer);
  glBindRenderbuffer(target, renderbuffer);

  return renderbuffer;
}

inline GLuint
createRenderbuffer(GLint width, GLint height, GLenum format, GLenum target = GL_RENDERBUFFER) {
  GLuint renderbuffer = createRenderbuffer(target);
  glRenderbufferStorage(target, format, width, height);

  return renderbuffer;
}

inline GLuint createFramebufferTexture(int width, int height, GLenum format) {
  GLuint texture = createTexture();
  glTexImage2D(
      GL_TEXTURE_2D,
      0, // level
      format,
      width,
      height,
      0, // border must be 0
      GL_RGBA,
      GL_BYTE,
      NULL); // no pixel data
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

  return texture;
}

inline GLuint createFramebufferCubemapTexture(int width, int height, GLenum format) {
  CHECK_EQ(width, height) << "cube faces must be square";
  GLuint texture = createTexture(GL_TEXTURE_CUBE_MAP);
  for (int face = 0; face < 6; ++face) {
    glTexImage2D(
        GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
        0, // level
        format,
        width,
        height,
        0, // border must be 0
        GL_RGBA,
        GL_BYTE,
        NULL); // no pixel data
  }
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  return texture;
}

inline GLuint createFramebufferDepthTexture(int width, int height) {
  GLuint depth = createTexture();
  glTexImage2D(
      GL_TEXTURE_2D,
      0, // level
      GL_DEPTH_COMPONENT,
      width,
      height,
      0, // border must be 0
      GL_DEPTH_COMPONENT,
      GL_FLOAT,
      NULL); // no pixel data
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);

  return depth;
}

inline GLuint createFramebufferColor(int width, int height, GLenum format = GL_RGB) {
  GLuint color = createRenderbuffer();
  glRenderbufferStorage(GL_RENDERBUFFER, format, width, height);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);

  return color;
}

inline GLuint createFramebufferDepth(int width, int height) {
  GLuint depth = createRenderbuffer();
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, width, height);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);

  return depth;
}

// useful when patching shaders: replace all occurrences of needle in haystack
inline void
replaceAll(std::string& haystack, const std::string& needle, const std::string& replacement) {
  for (size_t pos; (pos = haystack.find(needle)) != std::string::npos;) {
    haystack.replace(pos, needle.size(), replacement);
  }
}

// a vertex shader suitable for use with fullscreen
inline std::string fullscreenVertexShader(
    const char* attribute = "tex",
    const char* varying = "texVar") {
  std::string result = R"(
    #version 330 core

    in vec2 $ATTRIBUTE$;
    out vec2 $VARYING$;

    void main() {
      gl_Position = vec4(2 * $ATTRIBUTE$ - 1, 0, 1);
      $VARYING$ = $ATTRIBUTE$;
    }
  )";
  replaceAll(result, "$ATTRIBUTE$", attribute);
  replaceAll(result, "$VARYING$", varying);
  return result;
}

// draw a fullscreen triangle passing (0, 0)..(1, 1) into 'attribute'
inline void fullscreen(const GLuint program, const char* attribute = "tex") {
  GLuint vertexArray = createVertexArray();
  std::array<std::array<float, 2>, 3> data{{{{0, 0}}, {{2, 0}}, {{0, 2}}}};
  GLuint positions = createVertexAttributes(program, attribute, data);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glDeleteBuffers(1, &positions);
  glDeleteVertexArrays(1, &vertexArray);
}

inline std::vector<uint8_t> readPpmFile(int& width, int& height, const std::string& path) {
  // read the metadata
  std::ifstream file(path);
  std::string magic;
  file >> magic;
  CHECK_EQ(magic, "P6");
  file >> width;
  file >> height;
  int maxval;
  file >> maxval;
  CHECK_EQ(maxval, 255);
  file.ignore();
  // read the data
  static const int kNumChannels = 3;
  std::vector<uint8_t> rgb(width * height * kNumChannels);
  file.read((char*)rgb.data(), rgb.size());
  return rgb;
}

// Load a texture from a  ppm file
inline GLuint loadTexture(const std::string& path, const bool buildMipmaps = false) {
  int width;
  int height;
  std::vector<uint8_t> rgb = readPpmFile(width, height, path);
  GLuint tex =
      createTexture(width, height, rgb.data(), GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, buildMipmaps);
  setTextureWrap(GL_CLAMP_TO_EDGE);
  return tex;
}
