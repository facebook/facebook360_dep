/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/render/CanopyScene.h"

#include "source/util/ThreadPool.h"

namespace fb360_dep {

Canopy::Canopy(const cv::Mat_<cv::Vec4f>& color, const cv::Mat_<cv::Vec3f>& mesh, GLuint program) {
  // tell gl about color
  const bool kBuildMipmaps = true;
  colorTexture = createTexture(
      color.cols, color.rows, color.ptr(), GL_RGBA16, GL_BGRA, GL_FLOAT, kBuildMipmaps);
  setTextureAniso();

  // tell gl about mesh
  vertexArray = createVertexArray();
  positionBuffer = createVertexAttributes(
      program, "position", mesh.ptr<std::array<float, 3>>(), mesh.cols * mesh.rows);
  indexBuffer = createBuffer(GL_ELEMENT_ARRAY_BUFFER, stripify(mesh.cols, mesh.rows));
  modulo = mesh.cols;
  scale = {1.0 / mesh.cols, 1.0 / mesh.rows};
}

void Canopy::destroy() {
  glDeleteBuffers(1, &indexBuffer);
  glDeleteBuffers(1, &positionBuffer);
  glDeleteTextures(1, &colorTexture);
  glDeleteVertexArrays(1, &vertexArray);
}

void Canopy::render(
    GLuint framebuffer,
    const Eigen::Projective3f& transform,
    const GLuint program,
    const float ipd) const {
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

  glClearColor(0.0, 0.4, 0.0, 0.0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL); // use <= so we never see clear depth

  glUseProgram(program);

  // tell vertex shader uniforms
  glUniformMatrix4fv(glGetUniformLocation(program, "transform"), 1, GL_FALSE, transform.data());
  setUniform(program, "modulo", modulo);
  setUniform(program, "scale", scale.x(), scale.y());
  setUniform(program, "ipdm", ipd);

  // tell fragment shader which texture to use
  glBindTexture(GL_TEXTURE_2D, colorTexture);

  // draw stuff
  glBindVertexArray(vertexArray);
  drawElements<GLuint>(GL_TRIANGLE_STRIP);

  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glDisable(GL_DEPTH_TEST);
}

// compute texVar from vertex id
// transform rig space position from vertex array into clip space
std::string canopyVS = R"(
  #version 330 core

  uniform int modulo; // number of vertexes per row
  uniform float ipdm; // positive for left eye, negative for right eye (in meters)
  uniform vec2 scale; // transfrom from 2D index to color texture coordinates
  uniform mat4 transform; // transfrom to clip-space

  in vec3 position;
  out vec2 texVar;

  const float kPi = 3.1415926535897932384626433832795;

  float ipd(const float lat) {
    const float kA = 25;
    const float kB = 0.17;
    return ipdm  * exp(
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
    // compute the color texture coordinates from the vertex id
    texVar = scale * vec2(gl_VertexID % modulo + 0.5, gl_VertexID / modulo + 0.5);

    vec3 pos = position;
    if (ipdm != 0) { // adjust position when rendering stereo
      pos -= eye(pos);
    }

    // apply transform
    gl_Position = transform * vec4(pos, 1);
  }
)";

// read color from sampler
// modulate by how much mesh has been stretched
std::string canopyFS = R"(
  #version 330 core

  uniform sampler2D sampler;

  in vec2 texVar;
  out vec4 color;

  void main() {
    color = texture(sampler, texVar);
    if (color.a == 0) {
      discard;
    }
    // call a = dtexVar/dx and b = dtexVar/dy. a and b describe a parallelogram
    // in the camera image corresponding to your screen space pixel. if you
    // move one pixel in x, you move a in the camera image, and if you move one
    // pixel vertically, you move b in the camera image. if you move unit
    // vectors in other directions, your movement describes an ellipse in the
    // camera image. the minor axis of the ellipse corresponds to the worst
    // direction that you can move in because that is the finest grain
    // information you are looking for in the camera image

    // so, what is the length of the minor axis? well, the squared length of a
    // move in camera space corresponding to [cos(d), sin(d)] - a unit vector
    // move in direction d - in screen space is:
    //    (cos(d) * a + sin(d) * b)^2 =
    //    cos(d)^2 * a^2 + sin(d)^2 * b^2 + 2 cos(d) sin(d) a.b =
    //    (1 + cos(2d))/2 a^2 + (1 - cos(2d))/2 b^2 + sin(2d) a.b =
    //    (a^2 + b^2)/2 + (a^2 - b^2)/2 cos(2d) + a.b sin(2d) =
    //    (a^2 + b^2)/2 + [(a^2 - b^2)/2, a.b].[cos(2d), sin(2d)]

    // it can thus be seen that the squared length varies between:
    //    (a^2 + b^2)/2 -/+ |[(a^2 - b^2)/2, a.b]|
    vec2 a = dFdx(texVar), b = dFdy(texVar);
    float aa = dot(a, a), bb = dot(b, b), ab = dot(a, b);
    float minor = (aa + bb) / 2 - length(vec2((aa - bb) / 2, ab));
    // make contribution proportional to minor axis
    color.a *= minor;

    // alpha is a cone, 1 in the center, epsilon at edges
    const float eps = 1.0f / 255.0f;  // max granularity
    float cone = max(eps, 1 - 2 * length(texVar - 0.5));
    color.a *= cone;
  }
)";

std::string canopyFS_SVD = R"(
  #version 330 core

  uniform sampler2D sampler;

  in vec2 texVar;
  out vec4 color;

  void main() {
    color = texture(sampler, texVar);
    if (color.a == 0) {
      discard;
    }
    // call a = dtexVar/dx and b = dtexVar/dy. a and b describe a parallelogram
    // in the camera image corresponding to your screen space pixel as before but a more complete
    // method is to compute an area invariant version of this using the ratio of the singular values
    // of the matrix that takes a square pixel into the parallelogram.
    vec2 v1 = dFdx(texVar), v2 = dFdy(texVar);
    float a = v1.x;
    float b = v1.y;
    float c = v2.x;
    float d = v2.y;
    float s1 = a*a + b*b + c*c + d*d;
    float sb = a*a + b*b - c*c - d*d;
    float sc = a*c + b*d;
    float s2 =  sqrt(sb*sb + 4*sc*sc);
    float sigma1 = sqrt((s1 + s2) / 2);
    float sigma2 = sqrt((s1 - s2) / 2);
    color.a *= sigma2 / sigma1;

    // alpha is a cone, 1 in the center, epsilon at edges
    const float eps = 1.0f / 255.0f;  // max granularity
    float cone = max(eps, 1 - 2 * length(texVar - 0.5));
    color.a *= cone;
  }
)";

// read color from sampler and apply soft max
std::string accumulateFS = R"(
  #version 330 core

  uniform sampler2D sampler;
  uniform bool alphaBlend;

  in vec2 texVar;
  out vec4 color;

  void main() {
    color = texture(sampler, texVar);

    if (alphaBlend) {
      // we want weight, w = k^a - 1 = e^(log(k) * a) - 1
      const float kLogK = 30;
      color.a = exp(kLogK * color.a) - 1;
    }
  }
)";

// read color from sampler, converting from pre-multiplied alpha
std::string unpremulFS = R"(
  #version 330 core

  uniform sampler2D sampler;

  in vec2 texVar;
  out vec4 color;

  void main() {
    color = texture(sampler, texVar);
    color /= color.a;
  }
)";

// merge a texture into the accumulate buffer
static void
accumulate(GLuint framebuffer, GLuint texture, const GLuint program, const bool alphaBlend) {
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

  // set up blend equations to accumulate premultiplied alpha
  //    dst.rgb += src.a * src.rgb <=> dst.rgb = src.a * src.rgb + 1 * dst.rgb
  //    dst.a += src.a <=> dst.a = 1 * src.a + 1 * dst.a
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);

  glUseProgram(program);
  setUniform(program, "alphaBlend", alphaBlend);
  glBindTexture(GL_TEXTURE_2D, texture);
  fullscreen(program, "tex");

  glDisable(GL_BLEND);
}

void CanopyScene::render(
    const GLuint framebuffer,
    const Eigen::Projective3f& transform,
    const float ipd,
    const bool alphaBlend) const {
  // framebuffer used to accumulate all the cameras
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  GLuint accumulateBuffer = createFramebuffer();
  GLuint accumulateTexture = createFramebufferTexture(viewport[2], viewport[3], GL_RGBA32F);
  glClearColor(0.0, 0.0, 0.0, 0.0);
  glClear(GL_COLOR_BUFFER_BIT);

  // framebuffer used to render a single canopy
  GLuint canopyBuffer = createFramebuffer();
  GLuint canopyTexture = createFramebufferTexture(viewport[2], viewport[3], GL_RGBA32F);
  GLuint canopyDepth = createFramebufferDepth(viewport[2], viewport[3]);

  // accumulate all the canopies into the accumulateBuffer
  for (auto& canopy : canopies) {
    canopy.render(canopyBuffer, transform, canopyProgram, ipd);
    accumulate(accumulateBuffer, canopyTexture, accumulateProgram, alphaBlend);
  }

  // clean up canopy framebuffer
  glDeleteRenderbuffers(1, &canopyDepth);
  glDeleteTextures(1, &canopyTexture);
  glDeleteFramebuffers(1, &canopyBuffer);

  // un-premultiply out of the accumulation buffer into framebuffer
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glUseProgram(unpremulProgram);
  glBindTexture(GL_TEXTURE_2D, accumulateTexture);
  fullscreen(unpremulProgram, "tex");

  // clean up
  glDeleteTextures(1, &accumulateTexture);
  glDeleteFramebuffers(1, &accumulateBuffer);
}

template <typename T>
GLuint createCubemapTexture(
    const T& scene,
    const int edge,
    const Eigen::Vector3f& position,
    const float ipd,
    const bool alphaBlend) {
  // create cubemap framebuffer
  GLuint framebuffer = createFramebuffer();
  GLuint cubemap = createFramebufferCubemapTexture(edge, edge, GL_RGBA32F);
  glViewport(0, 0, edge, edge);

  // 90 degree frustum
  const float kNearZ = 0.1f; // meters
  Eigen::Projective3f projection = frustum(-kNearZ, kNearZ, -kNearZ, kNearZ, kNearZ);

  // render each cube face
  const int kFaceCount = 6;
  for (int face = 0; face < kFaceCount; ++face) {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(
        GL_DRAW_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
        cubemap,
        0);
    CHECK_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

    // from https://www.khronos.org/registry/OpenGL/extensions/EXT/EXT_texture_cube_map.txt
    // major axis
    // direction     target                             sc     tc    ma
    // ----------    -------------------------------    ---    ---   ---
    //  +rx          TEXTURE_CUBE_MAP_POSITIVE_X_EXT    -rz    -ry   rx
    //  -rx          TEXTURE_CUBE_MAP_NEGATIVE_X_EXT    +rz    -ry   rx
    //  +ry          TEXTURE_CUBE_MAP_POSITIVE_Y_EXT    +rx    +rz   ry
    //  -ry          TEXTURE_CUBE_MAP_NEGATIVE_Y_EXT    +rx    -rz   ry
    //  +rz          TEXTURE_CUBE_MAP_POSITIVE_Z_EXT    +rx    -ry   rz
    //  -rz          TEXTURE_CUBE_MAP_NEGATIVE_Z_EXT    -rx    -ry   rz
    static const Eigen::Vector3f table[kFaceCount][3] = {
        {Eigen::Vector3f::UnitX(), -Eigen::Vector3f::UnitZ(), -Eigen::Vector3f::UnitY()},
        {-Eigen::Vector3f::UnitX(), Eigen::Vector3f::UnitZ(), -Eigen::Vector3f::UnitY()},
        {Eigen::Vector3f::UnitY(), Eigen::Vector3f::UnitX(), Eigen::Vector3f::UnitZ()},
        {-Eigen::Vector3f::UnitY(), Eigen::Vector3f::UnitX(), -Eigen::Vector3f::UnitZ()},
        {Eigen::Vector3f::UnitZ(), Eigen::Vector3f::UnitX(), -Eigen::Vector3f::UnitY()},
        {-Eigen::Vector3f::UnitZ(), -Eigen::Vector3f::UnitX(), -Eigen::Vector3f::UnitY()}};
    Eigen::Affine3f transform;
    transform.linear().row(0) = table[face][1]; // sc from table
    transform.linear().row(1) = table[face][2]; // tc from table
    transform.linear().row(2) = -table[face][0]; // -major axis direction from table
    transform.translation().setZero();
    transform.translate(-position);
    scene.render(framebuffer, projection * transform, ipd, alphaBlend);
  }

  // clean up
  glDeleteFramebuffers(1, &framebuffer);

  // bind and return the cubemap
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap);
  return cubemap;
}

// render scene from position as a cubemap with edge by edge pixel faces, stacked vertically
cv::Mat_<cv::Vec4f> CanopyScene::cubemap(
    int edge,
    const Eigen::Vector3f& position,
    const float ipd,
    const bool alphaBlend) const {
  GLuint cubemap = createCubemapTexture(*this, edge, position, ipd, alphaBlend);

  // opengl's origin is bottom-left whereas opencv uses top-left
  // so stick faces into result from bottom to top, then flip the whole thing upside-down
  const int kFaceCount = 6;
  cv::Mat_<cv::Vec4f> result(kFaceCount * edge, edge);
  for (int face = 0; face < kFaceCount; ++face) {
    cv::Vec4f* dst = &result((kFaceCount - 1 - face) * edge, 0);
    glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_BGRA, GL_FLOAT, dst);
  }
  const int kUpsideDownCode = 0;
  cv::flip(result, result, kUpsideDownCode);

  // clean up and return
  glDeleteTextures(1, &cubemap);
  return result;
}

// render equirect from cubemap
std::string equirectFS = R"(
  #version 330 core

  uniform samplerCube sampler;

  in vec2 texVar;
  out vec4 color;

  void main() {
    // remap texVar to equirect direction with z up, BUT
    // - flip texVar.x because latitude grows in the negative direction of the z-axis
    // - flip texVar.y because glReadPixels reads the image from the bottom up
    const float PI = 3.1415926535897932384626433832795;
    float lon = (1 - texVar.x) * 2.0 * PI; // 360 .. 0 degrees
    float lat = -(texVar.y - 0.5) * PI; // 180 .. -180 degrees
    vec3 direction = vec3(
        cos(lat) * cos(lon),
        cos(lat) * sin(lon),
        sin(lat));
    color = texture(sampler, direction);
  }
)";

// render scene from position as an equirect that is height pixels tall and twice as wide
cv::Mat_<cv::Vec4f> CanopyScene::equirect(
    int height,
    const Eigen::Vector3f& position,
    const float ipd,
    const bool alphaBlend) const {
  // use the equirect height for the cube edge to provide plenty of resolution
  GLuint cubemap = createCubemapTexture(*this, height, position, ipd, alphaBlend);

  // create the framebuffer
  int width = 2 * height;
  GLuint fbo = createFramebuffer();
  GLuint color = createFramebufferColor(width, height, GL_RGBA32F);
  glViewport(0, 0, width, height);

  // set up the program
  GLuint program = createProgram(fullscreenVertexShader(), equirectFS);
#ifdef GL_TEXTURE_CUBE_MAP_SEAMLESS
  glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
#endif
  setTextureWrap<GL_TEXTURE_CUBE_MAP>(GL_CLAMP_TO_EDGE);
  setLinearFiltering<GL_TEXTURE_CUBE_MAP>();
  setTextureAniso<GL_TEXTURE_CUBE_MAP>();

  // render and read the result
  fullscreen(program);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  cv::Mat_<cv::Vec4f> equirect(height, width);
  glReadPixels(0, 0, equirect.cols, equirect.rows, GL_BGRA, GL_FLOAT, equirect.ptr());

  // clean up and return
  glDeleteProgram(program);
  glDeleteRenderbuffers(1, &color);
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &cubemap);
  return equirect;
}

static cv::Mat_<cv::Vec3f> disparityMesh(const cv::Mat_<float>& disparity, Camera camera) {
  // use camera disparity to compute a world coordinate mesh
  camera = camera.rescale({disparity.cols, disparity.rows});
  cv::Mat_<cv::Vec3f> mesh(disparity.rows, disparity.cols);
  for (int y = 0; y < disparity.rows; ++y) {
    for (int x = 0; x < disparity.cols; ++x) {
      float distance = 1.0f / disparity(y, x);
      Camera::Vector3 rig = camera.rig({x + 0.5, y + 0.5}, distance);
      mesh(y, x) = cv::Vec3f(rig[0], rig[1], rig[2]);
    }
  }
  return mesh;
}

cv::Mat_<cv::Vec4f> alphaFov(const cv::Mat_<cv::Vec4f>& color, Camera camera) {
  // knock out pixels outside fov
  cv::Mat_<cv::Vec4f> result(color.rows, color.cols);
  camera = camera.rescale({result.cols, result.rows});
  for (int y = 0; y < result.rows; ++y) {
    for (int x = 0; x < result.cols; ++x) {
      result(y, x) = color(y, x);
      result(y, x)[3] = camera.isOutsideImageCircle({x + 0.5, y + 0.5}) ? 0 : 1;
    }
  }
  return result;
}

CanopyScene::CanopyScene(
    const Camera::Rig& cameras,
    const std::vector<cv::Mat_<float>>& disparities,
    const std::vector<cv::Mat_<cv::Vec4f>>& colors,
    const bool onScreen) {
  // create the programs
  canopyProgram = createProgram(canopyVS, onScreen ? canopyFS : canopyFS_SVD);
  accumulateProgram = createProgram(fullscreenVertexShader(), accumulateFS);
  unpremulProgram = createProgram(fullscreenVertexShader(), unpremulFS);

  // prepare images and meshes for canopies in parallel
  std::vector<cv::Mat_<cv::Vec4f>> images(ssize(cameras));
  std::vector<cv::Mat_<cv::Vec3f>> meshes(ssize(cameras));
  ThreadPool threads;
  for (ssize_t i = 0; i < ssize(cameras); ++i) {
    threads.spawn([&, i] {
      images[i] = alphaFov(colors[i], cameras[i]);
      meshes[i] = disparityMesh(disparities[i], cameras[i]);
    });
  }
  threads.join();

  // create the canopies
  for (ssize_t i = 0; i < ssize(images); ++i) {
    canopies.emplace_back(images[i], meshes[i], canopyProgram);
  }
}

CanopyScene::~CanopyScene() {
  for (Canopy& canopy : canopies) {
    canopy.destroy();
  }
  glDeleteProgram(unpremulProgram);
  glDeleteProgram(accumulateProgram);
  glDeleteProgram(canopyProgram);
}

} // namespace fb360_dep
