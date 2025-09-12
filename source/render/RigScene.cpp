/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/render/RigScene.h"

#include <fstream>

#include <fmt/format.h>
#include <glog/logging.h>

#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#if !defined(__has_warning)
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#elif __has_warning("-Wmisleading-indentation")
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif

#if defined(__has_warning)
#if __has_warning("-Wimplicit-fallthrough")
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif // __has_warning("-Wimplicit-fallthrough")
#endif // defined(__has_warning)

#include "source/thirdparty/stb_image.h"
#pragma GCC diagnostic pop

#include <folly/Format.h>

namespace fb360_dep {

const float kUnit = 1.0f; // change this to 1.0e-2f if rig is in cm
const float kWhiteZ = 1.0f; // this distance is mapped to white (meters)
const float kR = 1.0f / kUnit;

// Resolution of direction textures.
// Must match the texture scaling in cameraVS inside createPrograms
// In cameraVS::main and cameraMeshVS::main
// texVarScaled = (0.5 + texVar * (kDirections - 1)) / kDirections;
const int kDirections = 128;

void RigScene::destroyFramebuffers() {
  glDeleteTextures(1, &accumulateTexture);
  glDeleteFramebuffers(1, &accumulateFBO);
  glDeleteRenderbuffers(1, &cameraDepth);
  glDeleteTextures(1, &cameraTexture);
  glDeleteFramebuffers(1, &cameraFBO);
  cameraFBO = 0; // flag as destroyed
}

void RigScene::createFramebuffers(const int w, const int h) {
  // framebuffer used to render each camera
  cameraFBO = createFramebuffer();
  // NB: for peak performance in a headset, this could be GL_SRGB8_ALPHA8
  cameraTexture = createFramebufferTexture(w, h, GL_RGBA16);
  cameraDepth = createFramebufferDepth(w, h);
  // framebuffer used to accumulate all the cameras
  accumulateFBO = createFramebuffer();
  accumulateTexture = createFramebufferTexture(w, h, GL_RGBA32F);
}

void RigScene::createPrograms() {
  // input is depth
  // texVar is computed from the instance id and scale and offset
  // position is computed by a lookup in the direction texture
  const std::string cameraVS = R"(
    #version 330 core

    uniform mat4 transform;
    uniform vec3 camera;
    uniform int modulo;
    uniform vec2 scale;
    uniform sampler2D directions;
    in vec2 offset; // per-vertex offset
    uniform bool isDepth;
    in float depth; // per-instance depth
    out vec2 texVar;

    uniform float kIPD = 0; // positive for left eye, negative for right eye

    const float kPi = 3.1415926535897932384626433832795;

    float ipd(const float lat) {
      const float kA = 25;
      const float kB = 0.17;
      return kIPD * exp(
        -exp(kA * (kB - 0.5 - lat / kPi))
        -exp(kA * (kB - 0.5 + lat / kPi)));
    }

    float sq(const float x) { return x * x; }
    float sq(const vec2 x) { return dot(x, x); }

    float error(const vec2 xy, const float z, const float dEst) {
      // xy^2 = (ipd(atan(z/dEst))/2)^2 + dEst^2 + error <=>
      return sq(xy) - sq(ipd(atan(z / dEst)) / 2) - sq(dEst);
    }

    float solve(const vec3 p) {
      // for initial estimate, assume lat = atan(z/d) ~ atan(z/xy)
      //   p.xy^2 = ipd(atan(z/d)^2 + d^2 ~
      //   p.xy^2 = ipd(atan(z/xy)^2 + d^2 <=>
      float d0 = sqrt(sq(p.xy) - sq(ipd(atan(p.z / length(p.xy)))));
      // refine with a few iterations of newton-raphson
      // two iterations get error below 2.4e-07 radians at 0.2 m
      // one iteration gets the same result at 0.7 m
      // and no iterations are required beyond 6.3 meters
      const int iterations = 2;
      for (int i = 0; i < iterations; ++i) {
        const float kSmidgen = 1e-3;
        float d1 = (1 + kSmidgen) * d0;
        float e0 = error(p.xy, p.z, d0);
        float e1 = error(p.xy, p.z, d1);
        float de = (e1 - e0) / (d1 - d0);
        d0 -= e0 / de;
      }
      return d0;
    }

    vec3 eye(const vec3 p) {
      float dEst = solve(p);
      float ipdEst = ipd(atan(p.z / dEst));
      float eNorm = ipdEst / 2;
      float k = -dEst / eNorm;
      mat2 A = mat2(1.0, k, -k, 1.0); // column major!
      return vec3(inverse(A) * p.xy, 0);
    }

    void main() {
      ivec2 instance = ivec2(gl_InstanceID % modulo, gl_InstanceID / modulo);
      vec2 dirVar = scale * (instance + offset);
      texVar = scale * (instance + (isDepth ? vec2(0.5) : offset));

      // We want the direction texture to align the first and last values to the
      // edge of each row/column, it'll have (kDirections - 1) texels instead of
      // kDirections, so we need to scale by (kDirections - 1) / kDirections
      // Also, kDirections buckets are originally defined at the leftmost edge
      // of pixels, not the center. We make up for this by shifting the input
      // locations by 0.5 / (num texels), where num texels = kDirections - 1
      const float kDirections = 128;
      vec2 texVarScaled = (0.5 + dirVar * (kDirections - 1)) / kDirections;
      vec3 direction = texture(directions, texVarScaled).xyz;

      vec3 rig = camera + depth * direction;
      if (kIPD != 0) { // adjust rig when rendering stereo
        rig -= eye(rig);
      }
      gl_Position = transform * vec4(rig, 1);
    }
  )";

  // an error, e, ortho to the ray will result in an angular error of
  //   x ~ tan x = 1/d * e
  // if the error is parallel to the ray - and the viewer is r away from the
  // ray origin - it will result in an angular error of
  //   x ~ tan x ~ r / d^2 * e

  // we can simply do a mesh simplification with these error metrics
  //   errors in depth are scaled by r / d^2
  //   errors orthogonal to depth are scaled by 1 / d

  // or we can do mesh simplification in an equi-error space
  // this could be optimized using standard mesh simplification

  // the actual mesh consists of points at direction(x, y) * depth(x, y)
  // approximation of equi-error mesh:
  //   (a, b, c) = (x, y, k * r / depth(x, y))
  // real coordinates can be recovered as:
  //   x = a, y = b, depth = k * r / c

  // error introduced in (a, b) will be scaled on the direction sphere by
  //   1 / focal // increasingly inaccurate for large fov lenses
  // and when projected out to depth, (a, b) errors will be scaled by
  //   d / focal

  // error introduced in c will cause errors in depth of
  //   k * r / c^2 = // using 1/c' = -1/c^2
  //   d^2 / (k * r) // using d = k * r / c <=> c = k * r / d

  // from this we can compute k
  // we want the angular error from ortho to be the same as from parallel
  // so we want
  //   1 / d * ortho error = r / d^2 * parallel error <=>
  //   1 / d * d / focal = r / d^2 * d^2 / (k * r) <=>
  //   k = focal

  // input is a, b, c
  // s, t, and depth are recovered from a, b, c
  // position is computed by a lookup in the direction texture
  const std::string cameraMeshVS = R"(
    #version 330 core

    uniform mat4 transform;
    uniform vec3 camera;
    uniform float focalR;
    uniform vec2 scale;
    uniform sampler2D directions;
    uniform bool forceMono;
    in vec3 abc;
    out vec2 texVar;

    void main() {
      // recover (s,t) from (a,b)
      texVar = scale * abc.xy;
      // recover depth from c
      float depth = forceMono ? focalR / 50.0 : focalR / abc.z;
      // scale direction texture coordinates; see cameraVS for discussion
      const float kDirections = 128;
      vec2 texVarScaled = (0.5 + texVar * (kDirections - 1)) / kDirections;
      vec3 direction = texture(directions, texVarScaled).xyz;
      gl_Position = transform * vec4(camera + depth * direction, 1);
    }
  )";

  const std::string fullscreenVS = R"(
    #version 330 core

    in vec2 tex;
    out vec2 texVar;

    void main() {
      gl_Position = vec4(2 * tex - 1, 0, 1);
      texVar = tex;
    }
  )";

  const std::string passthroughFS = R"(
    #version 330 core

    uniform sampler2D sampler;
    in vec2 texVar;
    out vec4 color;

    void main() {
      color = texture(sampler, texVar);
    }
  )";

  const std::string cameraFS = R"(
    #version 330 core

    uniform int debug;
    uniform sampler2D sampler;
    in vec2 texVar;
    out vec4 color;

    void main() {
      color = texture(sampler, texVar);
      // alpha is a cone, 1 in the center, epsilon at edges
      const float eps = 1.0f / 255.0f;  // max granularity
      float cone = max(eps, 1 - 2 * length(texVar - 0.5));
      color.a = cone;
    }
  )";

  const std::string effectFS = R"(
    #version 330 core

    uniform float effect;
    uniform sampler2D sampler;
    in vec2 texVar;
    out vec4 color;

    void main() {
      color = texture(sampler, texVar);
      vec4 cyan = vec4(0.5, 1.0, 1.0, 1.0);
      color += cyan
        * smoothstep(1/(effect - 0.5), 1/effect, gl_FragCoord.w)
        * smoothstep(1/(effect + 0.5), 1/effect, gl_FragCoord.w);
      // alpha is a cone, 1 in the center, 0 at edges
      float cone = max(0, 1 - 2 * length(texVar - 0.5));
      color.a = cone;
    }
  )";

  const std::string exponentialFS = R"(
    #version 330 core

    uniform sampler2D sampler;
    in vec2 texVar;
    out vec4 color;

    void main() {
      color = texture(sampler, texVar);
      color.a = exp(30 * color.a) - 1;
    }
  )";

  const std::string resolveFS = R"(
    #version 330 core

    uniform float fade;
    uniform sampler2D sampler;
    in vec2 texVar;
    out vec4 color;

    void main() {
      vec4 premul = texture(sampler, texVar);
      color.rgb = fade * premul.rgb / premul.a;
      color.a = premul.a;
    }
  )";

  cameraProgram = createProgram(cameraVS, cameraFS);
  cameraMeshProgram = createProgram(cameraMeshVS, cameraFS);
  effectMeshProgram = createProgram(cameraMeshVS, effectFS);
  updateProgram = createProgram(fullscreenVS, exponentialFS);
  resolveProgram = createProgram(fullscreenVS, resolveFS);
}

void RigScene::destroyPrograms() {
  glDeleteProgram(resolveProgram);
  glDeleteProgram(updateProgram);
  glDeleteProgram(effectMeshProgram);
  glDeleteProgram(cameraMeshProgram);
  glDeleteProgram(cameraProgram);
}

static MatrixDepth loadPfm(const std::string& filename) {
  std::ifstream file(filename, std::ios::binary);
  CHECK(file) << "can't open " << filename;
  // first line is identifier
  std::string identifier;
  file >> identifier;
  CHECK_EQ(identifier, "Pf") << "expected grayscale PFM file";
  // second line is width and height
  int width, height;
  file >> width >> height;
  // third line is scale with endianness encoded in the sign
  double scale;
  file >> scale;
  CHECK_LT(scale, 0) << "expected little endian PFM file";
  // skip 1 whitespace character
  file.ignore();
  // get the rest
  MatrixDepth result(height, width);
  file.read((char*)result.data(), result.size() * sizeof(float));
  return result;
}

static MatrixDepth fakePfm(const int width, const int height, const float depth) {
  return MatrixDepth::Constant(height, width, depth);
}

// glDeleteTextures appears to be slow. recycle
static bool kRecycle = true;
static std::vector<GLuint> recycledTextures;

void recycleTexture(const GLuint texture) {
  if (texture != 0) { // glDeleteTexture silently ignores 0
    if (kRecycle) {
      recycledTextures.push_back(texture);
    } else {
      glDeleteTextures(1, &texture);
    }
  }
}

GLuint bindRecycledTexture() {
  if (recycledTextures.empty()) {
    return createTexture();
  }
  GLuint result = recycledTextures.back();
  recycledTextures.pop_back();
  glBindTexture(GL_TEXTURE_2D, result);
  return result;
}

void emptyRecycling() {
  while (!recycledTextures.empty()) {
    glDeleteTextures(1, &recycledTextures.back());
    recycledTextures.pop_back();
  }
}

static GLuint linearTexture2D(
    const int width,
    const int height,
    const GLenum dstformat, // GL_RGB32F, for example
    const GLenum srcformat, // GL_RGBA, for example
    const GLenum srctype, // GL_UNSIGNED_BYTE, for example
    const GLvoid* data) {
  GLuint result = bindRecycledTexture();
  glTexImage2D(
      GL_TEXTURE_2D,
      0, // level
      dstformat,
      width,
      height,
      0, // border
      srcformat,
      srctype,
      data);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  return result;
}

static GLuint linearCompressedTexture2D(
    const int width,
    const int height,
    const GLenum format, // GL_COMPRESSED_RGBA_BPTC_UNORM, for example
    const GLvoid* data,
    const size_t size) {
  GLuint result = bindRecycledTexture();
  glCompressedTexImage2D(
      GL_TEXTURE_2D,
      0, // level
      format,
      width,
      height,
      0, // border
      GLsizei(size),
      data);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  return result;
}

// create rainbow texture with white a distance of kWhite from camera
static GLuint fakeTexture(const MatrixDepth& depth) {
  static const std::vector<Eigen::Vector3f> colors = {
      {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {1, 1, 0}, {1, 1, 1}};

  std::vector<uint8_t> fake(depth.size() * 4);
  for (int y = 0; y < depth.rows(); ++y) {
    for (int x = 0; x < depth.cols(); ++x) {
      float value = kWhiteZ / (kUnit * depth(y, x));
      Eigen::Vector3f color;
      if (value < 0) {
        color = colors.front();
      } else if (value < 1) {
        float v = value * (colors.size() - 1);
        int i = static_cast<int>(v);
        float f = v - i;
        color = (1 - f) * colors[i] + f * colors[i + 1];
      } else {
        color = colors.back();
      }
      uint8_t* p = &fake[(depth.cols() * y + x) * 4];
      for (int i = 0; i < 3; ++i) {
        p[i] = static_cast<uint8_t>(color[i] * 255 + 0.5f);
      }
      p[3] = 255;
    }
  }
  return linearTexture2D(
      static_cast<int>(depth.cols()),
      static_cast<int>(depth.rows()),
      GL_SRGB8_ALPHA8,
      GL_RGBA,
      GL_UNSIGNED_BYTE,
      fake.data());
}

// create grayscale texture with white a distance of kWhite from rig
template <typename T>
static GLuint fakeTexture(const MatrixDepth& depth, const Camera& camera) {
  std::vector<T> fake(depth.size() * 4);
  for (int y = 0; y < depth.rows(); ++y) {
    for (int x = 0; x < depth.cols(); ++x) {
      const Camera::Vector2 normalized((x + 0.5) / depth.cols(), (y + 0.5) / depth.rows());
      const Camera::Vector2 pixel = normalized.cwiseProduct(camera.resolution);
      const Camera::Vector3 rig = camera.rig(pixel).pointAt(depth(y, x));
      const float value = std::min(1.0f, kWhiteZ / (kUnit * static_cast<float>(rig.norm())));
      T* p = &fake[(depth.cols() * y + x) * 4];
      for (int i = 0; i < 3; ++i) {
        p[i] = static_cast<T>(value * std::numeric_limits<T>::max() + 0.5f);
      }
      p[3] = std::numeric_limits<T>::max();
    }
  }
  return linearTexture2D(
      static_cast<int>(depth.cols()),
      static_cast<int>(depth.rows()),
      RigScene::getInternalRGBAFormat(T()),
      GL_RGBA,
      getType(T()),
      fake.data());
}

template <typename T>
static void debugSaveBinary(const std::string& filename, const T& data) {
  const bool kSaveBinaries = false;
  if (kSaveBinaries) {
    std::ofstream file(filename, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(data[0]));
  }
}

static GLuint loadImageTexture(const std::string& filename) {
  // load color from an image file as 4 channels (rgba) per pixel
  const int kDstChannels = 4;
  int width, height, channels;
  uint8_t* data = stbi_load(filename.c_str(), &width, &height, &channels, kDstChannels);
  if (!data) {
    return 0;
  }

  using VectorXb = Eigen::Matrix<uint8_t, Eigen::Dynamic, 1>;
  debugSaveBinary(
      boost::filesystem::path(filename).replace_extension(".rgba").string(),
      Eigen::Map<VectorXb>(data, width * height * kDstChannels));
  // hand it to opengl
  GLuint result = linearTexture2D(width, height, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, data);
  // clean up and return
  stbi_image_free(data);
  return result;
}

static bool isBC7Supported() {
  static bool cacheValid = false;
  static bool cache = false;
  if (cacheValid) {
    return cache;
  }
  GLint count;
  glGetIntegerv(GL_NUM_EXTENSIONS, &count);
  for (GLint i = 0; i < count; ++i) {
    const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
    if (strstr(ext, "texture_compression_bptc")) {
      cache = true;
      break;
    }
  }
  if (cacheValid) {
    return cache;
  }
  cacheValid = true; // atomic would avoid harmless possible overreporting
  if (cache) {
    return cache;
  }
  // BC7 not supported, report what is
  LOG(INFO) << "BC7 (BPTC) not supported:";
  for (GLint i = 0; i < count; ++i) {
    const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
    if (strstr(ext, "texture_compression_")) {
      LOG(INFO) << fmt::format("- supported: {}", ext);
    }
  }
  return cache;
}

static void read32(uint32_t& dst, std::ifstream& file) {
  file.read((char*)&dst, sizeof(uint32_t));
}

static GLuint loadDdsTexture(const std::string& filename) {
  std::ifstream file(filename, std::ios::binary);
  if (!file) {
    return 0;
  }
  // first there's the signature
  uint32_t signature;
  read32(signature, file);
  CHECK_EQ(signature, 'D' << 0 | 'D' << 8 | 'S' << 16 | ' ' << 24);
  /* then a DDS_HEADER
  typedef struct {
    DWORD           dwSize;
    DWORD           dwFlags;
    DWORD           dwHeight;
    DWORD           dwWidth;
    DWORD           dwPitchOrLinearSize;
    DWORD           dwDepth;
    DWORD           dwMipMapCount;
    DWORD           dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    DWORD           dwCaps;
    DWORD           dwCaps2;
    DWORD           dwCaps3;
    DWORD           dwCaps4;
    DWORD           dwReserved2;
  } DDS_HEADER;
  */
  std::vector<uint32_t> ddsHeader(1);
  read32(ddsHeader[0], file);
  CHECK_EQ(ddsHeader[0] % sizeof(uint32_t), 0);
  ddsHeader.resize(ddsHeader[0] / sizeof(uint32_t));
  file.read((char*)&ddsHeader[1], ddsHeader[0] - sizeof(uint32_t));
  uint32_t width = ddsHeader[3]; // dwWidth
  uint32_t height = ddsHeader[2]; // dwHeight
  uint32_t size = ddsHeader[4]; // dwPitchOrLinearSize
  uint32_t format = ddsHeader[20]; // dwFourCC inside ddspf
  if (format == ('D' << 0 | 'X' << 8 | '1' << 16 | '0' << 24)) {
    /* "DX10" indicates the presence of a DDS_HEADER_DXT10
    typedef struct {
      DXGI_FORMAT              dxgiFormat;
      D3D10_RESOURCE_DIMENSION resourceDimension;
      UINT                     miscFlag;
      UINT                     arraySize;
      UINT                     miscFlags2;
    } DDS_HEADER_DXT10;
    */
    uint32_t ddsHeaderDxt10[5];
    for (uint32_t& v : ddsHeaderDxt10) {
      read32(v, file);
    }
    format = ddsHeaderDxt10[0]; // dxgiFormat
  }
  GLenum glFormat;
  if (format == 99) { // BC7_UNORM_SRGB
    if (!isBC7Supported()) {
      return 0;
    }
#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM 0x8E8D
#endif
    glFormat = GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM;
  } else {
    CHECK(0) << ".dds file is not BC7_UNORM_SRGB (99) format";
  }
  // read the bytes
  std::vector<uint8_t> compressed(size);
  file.read((char*)compressed.data(), compressed.size());
  debugSaveBinary(boost::filesystem::path(filename).replace_extension(".bc7").string(), compressed);
  // hand it to opengl
  return linearCompressedTexture2D(width, height, glFormat, compressed.data(), compressed.size());
}

static GLuint loadTexture(const std::string& filename) {
  GLuint result = loadDdsTexture(filename + ".dds");
  if (!result) {
    result = loadImageTexture(filename + ".png");
  }
  if (!result) {
    result = loadImageTexture(filename + ".jpg");
  }
  CHECK(result != 0) << "can't load image " << filename;
  return result;
}

static MatrixDepth downscale(const MatrixDepth& hires, const int factor) {
  // point-sample
  int offset = factor / 2;
  MatrixDepth result(hires.rows() / factor, hires.cols() / factor);
  for (int y = 0; y < result.rows(); ++y) {
    for (int x = 0; x < result.cols(); ++x) {
      result(y, x) = hires(y * factor + offset, x * factor + offset);
    }
  }
  return result;
}

static RigScene::Subframe createMeshSubframe(
    const std::string& imagePrefix,
    const std::string& depthPrefix,
    const GLuint program) {
  RigScene::Subframe subframe;
  subframe.vertexArray = createVertexArray();
  // read .obj file
  std::vector<Eigen::Vector3f> vertexes;
  std::vector<Eigen::Vector3i> faces;
  std::string depth = depthPrefix + ".obj";
  std::ifstream file(depth);
  CHECK(file) << "can't open " << depth;
  while (file) {
    std::string first;
    file >> first;
    if (first == "v") {
      Eigen::Vector3f v;
      file >> v.x() >> v.y() >> v.z();
      vertexes.push_back(v);
    } else if (first == "f") {
      Eigen::Vector3i f;
      file >> f.x() >> f.y() >> f.z();
      f -= f.Constant(1); // first vertex in an .obj file is 1
      faces.push_back(f);
    }
    std::string skip;
    getline(file, skip); // skip rest of line
  }
  debugSaveBinary(imagePrefix + ".vtx", vertexes);
  debugSaveBinary(imagePrefix + ".idx", faces);
  // pass vertexes and faces to opengl
  GLuint meshVBO = createVertexAttributes(getAttribLocation(program, "abc"), vertexes);
  GLuint meshIBO = createBuffer(GL_ELEMENT_ARRAY_BUFFER, faces);
  subframe.indexCount = 3 * static_cast<GLsizei>(faces.size());
  Eigen::Vector3f maximum(0, 0, 0);
  for (const Eigen::Vector3f& v : vertexes) {
    maximum = maximum.cwiseMax(v);
  }
  subframe.size = {maximum.x() + 0.5f, maximum.y() + 0.5f};
  LOG(INFO) << fmt::format(
      "loaded {}x{} mesh, {} vertexes, {} faces",
      subframe.size.x(),
      subframe.size.y(),
      vertexes.size(),
      faces.size());
  // load color
  subframe.colorTexture = loadTexture(imagePrefix);
  // clean up buffers
  glBindVertexArray(0);
  glDeleteBuffers(1, &meshIBO);
  glDeleteBuffers(1, &meshVBO);

  return subframe;
}

static RigScene::Subframe createPointCloudSubframeFromMemory(
    const GLuint texture,
    MatrixDepth& depthMap,
    const GLuint program) {
  RigScene::Subframe subframe;
  subframe.colorTexture = texture;

  // create vertex buffer for per-instance depth
  subframe.vertexArray = createVertexArray();
  using Vector1f = Eigen::Matrix<float, 1, 1>;
  GLuint depthVBO = createVertexAttributes(
      getAttribLocation(program, "depth"),
      Eigen::Map<Eigen::Matrix<Vector1f, Eigen::Dynamic, 1>>(
          reinterpret_cast<Vector1f*>(depthMap.data()), depthMap.size()));
  glVertexAttribDivisor(getAttribLocation(program, "depth"), 1);
  subframe.size = {depthMap.cols(), depthMap.rows()};

  // create vertex buffer for vertex offsets
  const float kRadius = 1.0;
  std::vector<Eigen::Vector2f> offsets = {
      {0.5f - kRadius, 0.5f - kRadius},
      {0.5f + kRadius, 0.5f - kRadius},
      {0.5f - kRadius, 0.5f + kRadius},
      {0.5f + kRadius, 0.5f + kRadius},
  };
  GLuint offsetVBO = createVertexAttributes(getAttribLocation(program, "offset"), offsets);

  // clean up buffers
  glBindVertexArray(0);
  glDeleteBuffers(1, &offsetVBO);
  glDeleteBuffers(1, &depthVBO);

  return subframe;
}

static RigScene::Subframe createPointCloudSubframe(
    const std::string& imagePrefix,
    const std::string& depthPrefix,
    const GLuint program) {
  // load depth map from file
  MatrixDepth depthMap = depthPrefix.empty()
      ? fakePfm(kDirections, kDirections, static_cast<float>(Camera::kNearInfinity))
      : loadPfm(depthPrefix + ".pfm");
  const int kDownscaleFactor = 1;
  if (kDownscaleFactor != 1) {
    depthMap = downscale(depthMap, kDownscaleFactor);
  }

  const GLuint texture = imagePrefix.empty() ? fakeTexture(depthMap) : loadTexture(imagePrefix);

  return createPointCloudSubframeFromMemory(texture, depthMap, program);
}

RigScene::Subframe RigScene::createSubframe(
    const Camera& camera,
    const GLuint buffer,
    const uint64_t offset,
    const folly::dynamic& layout) const {
  Subframe subframe;
  subframe.vertexArray = createVertexArray();
  const int w(static_cast<int>(camera.resolution.x()));
  const int h(static_cast<int>(camera.resolution.y()));
  // PBO for color
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
  if (isBC7Supported()) {
    subframe.colorTexture = linearCompressedTexture2D(
        w,
        h,
        GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM,
        reinterpret_cast<GLvoid*>( // NOLINT(performance-no-int-to-ptr): OpenGL API requires void*
                                   // for buffer offsets
            layout[".bc7"]["offset"].getInt() - offset),
        w * h); // bc7 is 1 byte/pixel
  } else {
    subframe.colorTexture = linearTexture2D(
        w,
        h,
        GL_SRGB8_ALPHA8,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        reinterpret_cast<GLvoid*>( // NOLINT(performance-no-int-to-ptr): OpenGL API requires void*
                                   // for buffer offsets
            layout[".rgba"]["offset"].getInt() - offset));
  }
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  // VBO for vertexes
  glBindBuffer(GL_ARRAY_BUFFER, buffer);
  GLint location = getAttribLocation(cameraMeshProgram, "abc");
  glVertexAttribPointer(
      location,
      3,
      GL_FLOAT,
      GL_TRUE,
      0,
      reinterpret_cast<GLvoid*>( // NOLINT(performance-no-int-to-ptr): OpenGL API requires void* for
                                 // buffer offsets
          layout[".vtx"]["offset"].getInt() - offset));
  glEnableVertexAttribArray(location);
  // IBO for indexes
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
  subframe.indexCount = static_cast<GLsizei>(layout[".idx"]["size"].getInt() / sizeof(uint32_t));
  subframe.indexOffset = reinterpret_cast<GLvoid*>( // NOLINT(performance-no-int-to-ptr): OpenGL API
                                                    // requires void* for buffer offsets
      layout[".idx"]["offset"].getInt() - offset);
  subframe.size = {w, h};
  // unbind vertex array before deleting buffer so vertex array keeps it alive
  glBindVertexArray(0);
  glDeleteBuffers(1, &buffer);
  return subframe;
}

RigScene::Subframe RigScene::createSubframe(
    const std::string& id,
    const std::string& images,
    const std::string& depths) const {
  const std::string image = images.empty() ? "" : images + '/' + id;
  const std::string depth = depths.empty() ? "" : depths + '/' + id + "_depth";
  return useMesh ? createMeshSubframe(image, depth, cameraMeshProgram)
                 : createPointCloudSubframe(image, depth, cameraProgram);
}

std::vector<RigScene::Subframe> RigScene::createFrame(
    const std::string& images,
    const std::string& depths) const {
  std::vector<RigScene::Subframe> subframes(rig.size());
  for (int i = 0; i < int(subframes.size()); ++i) {
    LOG(INFO) << fmt::format("load subframe for {}", rig[i].id);
    subframes[i] = createSubframe(rig[i].id, images, depths);
  }
  return subframes;
}

RigScene::Subframe RigScene::createPointCloudSubframeFromData(
    const uint8_t* colorData,
    uint16_t* depthData,
    const int colorWidth,
    const int colorHeight,
    const int depthWidth,
    const int depthHeight,
    const float depthScale = 1.0f) const {
  // Color
  GLuint colorTexture = linearTexture2D(
      colorWidth, colorHeight, GL_SRGB8_ALPHA8, GL_BGRA, GL_UNSIGNED_BYTE, colorData);

  // Depth
  using MatrixDepth16 = Eigen::Matrix<uint16_t, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  MatrixDepth16 depthMatrix16 = Eigen::Map<MatrixDepth16>(depthData, depthHeight, depthWidth);
  MatrixDepth depthMatrix = depthMatrix16.cast<float>();
  depthMatrix *= depthScale;

  return createPointCloudSubframeFromMemory(colorTexture, depthMatrix, cameraProgram);
}

void RigScene::destroyFrame(std::vector<Subframe>& subframes) {
  for (Subframe& subframe : subframes) {
    recycleTexture(subframe.colorTexture);
    glDeleteVertexArrays(1, &subframe.vertexArray);
  }
  subframes.clear();
}

GLuint RigScene::createDirection(const Camera& camera) {
  // create direction texture that tabelizes the rig() function
  std::vector<Eigen::Vector3f> directions;
  for (int y = 0; y < kDirections; ++y) {
    for (int x = 0; x < kDirections; ++x) {
      Camera::Vector2 c(x, y);
      Camera::Vector2 pixel = c.cwiseProduct(camera.resolution) / (kDirections - 1);
      Camera::Vector3 direction = camera.rig(pixel).direction();
      if (isDepthZCoord) {
        Camera::Real factor = -camera.pixelToCamera(pixel).z();
        direction /= factor;
      }
      directions.push_back(direction.cast<float>());
    }
  }
  return linearTexture2D(kDirections, kDirections, GL_RGB32F, GL_RGB, GL_FLOAT, directions.data());
}

// render a full-screen triangle with the given program and texture
void RigScene::fullscreen(GLuint program, GLuint texture, GLuint target) {
  glUseProgram(program);

  static const int kUnit = 0;
  connectUnitWithTextureAndUniform(kUnit, target, texture, program, "sampler");

  GLuint vertexArray = createVertexArray();
  const std::vector<Eigen::Vector2f> tex{
      {0, 0},
      {0, 2},
      {2, 0},
  };
  GLuint buffer = createVertexAttributes(program, "tex", tex);
  glDrawArrays(GL_TRIANGLES, 0, 3); // draw the triangle
  glDeleteBuffers(1, &buffer);
  glDeleteVertexArrays(1, &vertexArray);
}

void RigScene::clearSubframe() const {
  glBindFramebuffer(GL_FRAMEBUFFER, cameraFBO);
  CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

GLuint RigScene::getProgram() const {
  if (useMesh) {
    return effect != 0 ? effectMeshProgram : cameraMeshProgram;
  }
  return cameraProgram;
}

void RigScene::renderSubframe(const int subframeIndex, const bool wireframe) const {
  CHECK(subframeIndex < int(subframes.size()));
  const RigScene::Subframe& subframe = subframes[subframeIndex];
  const Camera& camera = rig[subframeIndex];
  const GLuint directionTexture = directionTextures[subframeIndex];

  GLuint program = getProgram();
  glUseProgram(program);
  // send camera position to gl
  Eigen::Vector3f position = camera.position.cast<float>();
  glUniform3fv(getUniformLocation(program, "camera"), 1, position.data());
  setUniform(program, "scale", 1.0f / subframe.size.x(), 1.0f / subframe.size.y());
  // activate direction texture for this camera
  const int kDirectionUnit = 0;
  connectUnitWith2DTextureAndUniform(kDirectionUnit, directionTexture, program, "directions");
  // debug flags, if used
  if (glGetUniformLocation(program, "debug") != -1) {
    setUniform(program, "debug", debug);
  }
  // activate color texture for this camera
  const int kColorUnit = 1;
  connectUnitWith2DTextureAndUniform(kColorUnit, subframe.colorTexture, program, "sampler");
  // activate vertex array for this camera
  glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
  glBindVertexArray(subframe.vertexArray);
  glEnable(GL_DEPTH_TEST);
  if (useMesh) {
    if (effect != 0) {
      setUniform(program, "effect", effect);
    }
    float focalR = static_cast<float>(camera.getScalarFocal() * kR);
    setUniform(program, "focalR", focalR);
    setUniform(program, "forceMono", forceMono);
    glDrawElements(GL_TRIANGLES, subframe.indexCount, GL_UNSIGNED_INT, subframe.indexOffset);

    if (!backgroundSubframes.empty() && renderBackground) {
      const RigScene::Subframe& backgroundSubframe = backgroundSubframes[subframeIndex];
      const int kBackgroundColorUnit = 2;
      connectUnitWith2DTextureAndUniform(
          kBackgroundColorUnit, backgroundSubframe.colorTexture, program, "sampler");
      glBindVertexArray(backgroundSubframe.vertexArray);
      glDrawElements(
          GL_TRIANGLES,
          backgroundSubframe.indexCount,
          GL_UNSIGNED_INT,
          backgroundSubframe.indexOffset);
    }
  } else {
    setUniform(program, "modulo", subframe.size.x());
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, subframe.size.prod());
    setUniform(program, "isDepth", isDepth);
  }
  glDisable(GL_DEPTH_TEST);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

GLint RigScene::clearAccumulation() {
  // save the currently bound framebuffer
  GLint result;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &result);
  // destroy existing framebuffers if they're the wrong size
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  const int w = viewport[2];
  const int h = viewport[3];
  if (cameraFBO != 0) {
    glBindTexture(GL_TEXTURE_2D, cameraTexture);
    int cw;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cw);
    int ch;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &ch);
    if (cw != w || ch != h) {
      destroyFramebuffers();
    }
  }
  if (cameraFBO == 0) {
    createFramebuffers(w, h);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, accumulateFBO);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_FRAMEBUFFER_SRGB);
  if (/* DISABLE CODE */ (false)) {
    glCullFace(GL_FRONT);
    glEnable(GL_CULL_FACE);
  }
  return result;
}

void RigScene::updateAccumulation() const {
  // add camera buffer to accumulation buffer
  glBindFramebuffer(GL_FRAMEBUFFER, accumulateFBO);
  CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
  // set up blend equations to accumulate premultiplied alpha
  //    dst.rgb = src.a * src.rgb + 1 * dst.rgb
  //    dst.a = 1 * src.a + 1 * dst.a
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);
  fullscreen(updateProgram, cameraTexture);
  glDisable(GL_BLEND);
}

void RigScene::resolveAccumulation(GLint fbo, float fade) const {
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glUseProgram(resolveProgram);
  setUniform(resolveProgram, "fade", fade);
  fullscreen(resolveProgram, accumulateTexture);
}

static Eigen::Matrix4f computeTransform(const Eigen::Matrix4f& view) {
  // rig is specified in cm using z-is-up convention
  // view is specified in m using y-is-up convention
  Eigen::Matrix4f model;
  model << kUnit, 0, 0, 0, 0, 0, kUnit, 0, 0, -kUnit, 0, 0, 0, 0, 0, 1;
  return view * model;
}

void RigScene::updateTransform(const Eigen::Matrix4f& transform) const {
  GLuint program = getProgram();
  glUseProgram(program);
  glUniformMatrix4fv(getUniformLocation(program, "transform"), 1, GL_FALSE, transform.data());
}

static bool
isVisible(const Camera& camera, const Camera::Vector2& frac, const Eigen::Matrix4f& transform) {
  Camera::Vector2 pixel = frac.cwiseProduct(camera.resolution);
  Eigen::Vector3f rig = camera.rigNearInfinity(pixel).cast<float>();
  Eigen::Vector4f clip = transform * Eigen::Vector4f(rig.x(), rig.y(), rig.z(), 1);
  return clip.w() > 0 && -clip.w() < clip.x() && clip.x() < clip.w() && -clip.w() < clip.y() &&
      clip.y() < clip.w();
}

static bool isVisible(const Camera& camera, const Eigen::Matrix4f& transform) {
  const int kIntervalsPerEdge = 3;
  for (int y = 0; y <= kIntervalsPerEdge; ++y) {
    for (int x = 0; x <= kIntervalsPerEdge; ++x) {
      if (y == 0 || y == kIntervalsPerEdge) {
        if (x == 0 || x == kIntervalsPerEdge) {
          continue; // don't check the corners
        }
      }
      Camera::Vector2 frac(
          x / Camera::Real(kIntervalsPerEdge), y / Camera::Real(kIntervalsPerEdge));
      if (isVisible(camera, frac, transform)) {
        return true;
      }
    }
  }
  return false;
}

void RigScene::render(
    const Eigen::Matrix4f& projview,
    const float displacementMeters,
    const bool doCulling,
    const bool wireframe) {
  Eigen::Matrix4f transform = computeTransform(projview);
  updateTransform(transform);
  GLint fbo = clearAccumulation();
  culled.resize(rig.size());
  for (int i = 0; i < int(rig.size()); ++i) {
    culled[i] = doCulling && !isVisible(rig[i], transform);

    if (i < int(subframes.size()) && subframes[i].isValid() && !culled[i]) {
      clearSubframe();
      renderSubframe(i, wireframe);
      updateAccumulation();
    }
  }
  const float kBeginFade = 0.5f;
  const float kEndFade = 0.75f;
  const float kMinimumFade = 0.05f;
  // 0 until kBeginFade, then ramp down do kMinimumFade at kEndFade
  const float fade = kMinimumFade +
      (1.0f - kMinimumFade) *
          std::max(0.0f, std::min(1.0f, (displacementMeters - kEndFade) / (kBeginFade - kEndFade)));
  resolveAccumulation(fbo, fade * fade); // square to die off faster
  CHECK_EQ(glGetError(), GL_NO_ERROR);
}

RigScene::~RigScene() {
  if (cameraFBO) { // framebuffers are created lazily
    destroyFramebuffers();
  }
  destroyFrame(subframes);
  for (const GLuint& texture : directionTextures) {
    recycleTexture(texture);
  }
  destroyPrograms();
  emptyRecycling();
}

RigScene::RigScene(const Camera::Rig& rig, const bool useMesh, const bool isDepthZCoord)
    : useMesh(useMesh), isDepthZCoord(isDepthZCoord), rig(rig) {
  createPrograms();
  for (const Camera& camera : rig) {
    directionTextures.push_back(createDirection(camera));
  }
  cameraFBO = 0; // mark cameraFBO as uninitialized
}

RigScene::RigScene(const std::string& rigPath, const bool useMesh, const bool isDepthZCoord)
    : RigScene(Camera::loadRig(rigPath), useMesh, isDepthZCoord) {}

RigScene::RigScene(
    const std::string& rigPath,
    const std::string& imageDir,
    const std::string& depthDir,
    const bool useMesh,
    const bool isDepthZCoord)
    : RigScene(rigPath, useMesh, isDepthZCoord) {
  subframes = createFrame(imageDir, depthDir);
}

template <typename T>
void RigScene::createSubframes(
    const Camera::Rig& rig,
    std::vector<MatrixDepth>& depthMaps,
    const std::vector<std::vector<T>>& images,
    const std::vector<int>& imageWidths,
    const std::vector<int>& imageHeights) {
  const size_t numCameras = rig.size();
  CHECK_GT(numCameras, 0);
  CHECK_EQ(numCameras, depthMaps.size());
  CHECK_EQ(numCameras, images.size());
  subframes.resize(numCameras);
  isDepth = images[0].empty();
  for (size_t i = 0; i < numCameras; ++i) {
    GLuint texture;
    if (isDepth) {
      texture = fakeTexture<T>(depthMaps[i], rig[i]);
    } else {
      CHECK_EQ(images[i].size(), imageWidths[i] * imageHeights[i] * 4);
      texture = linearTexture2D(
          imageWidths[i],
          imageHeights[i],
          getInternalRGBAFormat(T()),
          GL_RGBA,
          getType(T()),
          images[i].data());
    }
    subframes[i] = createPointCloudSubframeFromMemory(texture, depthMaps[i], cameraProgram);
  }
}

template <>
RigScene::RigScene(
    const Camera::Rig& rig,
    std::vector<MatrixDepth>& depthMaps,
    const std::vector<std::vector<uint8_t>>& images,
    const std::vector<int>& imageWidths,
    const std::vector<int>& imageHeights)
    : RigScene(rig, false) {
  createSubframes(rig, depthMaps, images, imageWidths, imageHeights);
}

template <>
RigScene::RigScene(
    const Camera::Rig& rig,
    std::vector<MatrixDepth>& depthMaps,
    const std::vector<std::vector<uint16_t>>& images,
    const std::vector<int>& imageWidths,
    const std::vector<int>& imageHeights)
    : RigScene(rig, false) {
  createSubframes(rig, depthMaps, images, imageWidths, imageHeights);
}

} // namespace fb360_dep
