/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/Format.h>

#include "source/render/BoundingVolumeHierarchy.h"
#include "source/render/PerlinNoise.h"
#include "source/render/RaytracingPrimitives.h"
#include "source/util/Camera.h"
#include "source/util/CvUtil.h"
#include "source/util/MathUtil.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;
using namespace fb360_dep::cv_util;
using namespace fb360_dep::math_util;
using namespace fb360_dep::render;
using namespace fb360_dep::system_util;

const std::string kUsageMessage = R"(
  - Render an artificial scene as seen by the specified rig.

  - Example:
    ./RigSimulator \
    --mode=pinhole_ring \
    --skybox_path=/path/to/skybox.png
)";

DEFINE_int32(
    anti_alias_supersample,
    1,
    "1 = no supersampling, 2 or higher = anti-alias supersampling");
DEFINE_double(ceiling_depth, 0, "depth of ceiling texture (m)");
DEFINE_string(ceiling_path, "", "path to image to use for ceiling");
DEFINE_double(ceiling_position, 0, "how far up the ceiling is (m)");
DEFINE_double(ceiling_width, 0, "width of ceiling texture (m)");
DEFINE_string(
    dest_cam_images,
    "",
    "path to directory to write camera images for multi-camera rigs");
DEFINE_string(dest_left, "", "path to left-eye image");
DEFINE_string(dest_mono, "", "path to mono image");
DEFINE_string(dest_mono_depth, "", "path to mono 1/depthmap (intensity = 1 / depth in meters)");
DEFINE_string(dest_right, "", "path to right-eye image");
DEFINE_string(dest_stereo, "", "path to right-eye image");
DEFINE_int32(eqr_height, 1540, "height of equirect output");
DEFINE_int32(eqr_width, 3080, "width of equirect output");
DEFINE_int32(ftheta_height, 400, "height of ftheta camera output");
DEFINE_double(
    ftheta_image_circle_fov,
    166.667,
    "ftheta FOV, i.e. number of degrees spanned at the image circle");
DEFINE_int32(
    ftheta_image_circle_radius,
    250,
    "image circle radius corresponding to specified ftheta FOV");
DEFINE_int32(ftheta_width, 300, "width of ftheta camera output");
DEFINE_double(
    ground_plane_dist_m,
    1.70,
    "for 'ground_plane' scene, distance from camera to ground");
DEFINE_double(interpupillary_radius, 3.2, "half distance between eyes");
DEFINE_bool(
    marble,
    false,
    "if true, adds a marble (perlin noise) texture to the objects in the scene");
DEFINE_double(marble_scale, 0.1, "scale applied to marble texture");
DEFINE_double(
    max_icosahedron_dist,
    250,
    "maximum distance from origin that a randomly generated icosahedron can spawn");
DEFINE_double(max_icosahedron_radius, 50, "max radius of a randomly generated icosahedron");
DEFINE_double(
    min_icosahedron_dist,
    100,
    "minimum distance from a center of camera to the closest point on a randomly generated icosahedron");
DEFINE_double(min_icosahedron_radius, 20, "min radius of a randomly generated icosahedron");
DEFINE_string(
    mode,
    "",
    "mono_eqr,stereo_eqr,pinhole_ring,ftheta_ring,dodecahedron,icosahedron,rig_from_json (required)");
DEFINE_double(
    noise_amplitude,
    0.0,
    "amount of noise to be added to pixels (to simulate real camera noise). pixel intensities are scaled in 0...255");
DEFINE_int32(num_cams_in_ring, 14, "number of cameras in simulated rings of cameras");
DEFINE_int32(num_random_icosahedrons, 250, "number of icosahedrons to generate");
DEFINE_double(
    pinhole_aspect_ratio,
    1.0,
    "aspect ratio of pinhole lens = horizontal fov / vertical fov");
DEFINE_double(pinhole_fov_horizontal, 77.7, "horizontal FOV of pinhole lens (degrees)");
DEFINE_int32(pinhole_height, 512, "height of pinhole camera output");
DEFINE_int32(pinhole_width, 512, "width of pinhole camera output");
DEFINE_bool(red_triangle, false, "add a red triangle at (0,0)");
DEFINE_string(rig_in, "", "path to read json rig file if mode = rig_from_json");
DEFINE_string(rig_out, "", "path to write json description of multi-camera rig");
DEFINE_double(
    rig_radius,
    0.218,
    "radius of the rig/sphere of cameras (m). distance from center to lens exit pupil.");
DEFINE_string(scene, "icosahedron", "scene to draw: 'icosahedron', 'cube', 'ground_plane'");
DEFINE_string(skybox_path, "res/skybox.jpg", "path to image to use as background/skybox");
DEFINE_double(top_cam_vertical_offset, 13.0, "distance from center plane to top camera");

namespace icosahedron_data {
const static float X = 0.525731112119133696;
const static float Z = 0.850650808352039932;
const static float icosahedronVertex[12][3] = {
    {-X, 0.0, Z},
    {X, 0.0, Z},
    {-X, 0.0, -Z},
    {X, 0.0, -Z},
    {0.0, Z, X},
    {0.0, Z, -X},
    {0.0, -Z, X},
    {0.0, -Z, -X},
    {Z, X, 0.0},
    {-Z, X, 0.0},
    {Z, -X, 0.0},
    {-Z, -X, 0.0}};
const static int icosahedronTriangle[20][3] = {
    {1, 4, 0}, {4, 9, 0},  {4, 5, 9},  {8, 5, 4},  {1, 8, 4},  {1, 10, 8}, {10, 3, 8},
    {8, 3, 5}, {3, 2, 5},  {3, 7, 2},  {3, 10, 7}, {10, 6, 7}, {6, 11, 7}, {6, 0, 11},
    {6, 1, 0}, {10, 1, 6}, {11, 0, 9}, {2, 11, 9}, {5, 2, 9},  {11, 2, 7}};
}; // namespace icosahedron_data

void makeIcosahedron(
    std::vector<Triangle>& triangles,
    const cv::Vec3f& center,
    const float& radius) {
  using namespace icosahedron_data;

  const cv::Vec3f color =
      center[2] > 0 ? cv::Vec3f(0, 1, 0) : cv::Vec3f(randf0to1(), randf0to1(), randf0to1());

  for (int i = 0; i < 20; ++i) {
    const int v1i = icosahedronTriangle[i][0];
    const int v2i = icosahedronTriangle[i][1];
    const int v3i = icosahedronTriangle[i][2];
    const cv::Vec3f v1(
        icosahedronVertex[v1i][0], icosahedronVertex[v1i][1], icosahedronVertex[v1i][2]);
    const cv::Vec3f v2(
        icosahedronVertex[v2i][0], icosahedronVertex[v2i][1], icosahedronVertex[v2i][2]);
    const cv::Vec3f v3(
        icosahedronVertex[v3i][0], icosahedronVertex[v3i][1], icosahedronVertex[v3i][2]);

    triangles.push_back(
        Triangle(v1 * radius + center, v2 * radius + center, v3 * radius + center, color));
  }
}

RayIntersectionResult raytraceBVH(const Ray& ray, const BoundingVolumeHierarchy& bvh) {
  RayIntersectionResult closestIntersection(false, FLT_MAX, -1);

  // if the ray misses the bvh, it misses everything
  if (!rayIntersectSphereYesNo(ray, bvh.sphere)) {
    return closestIntersection;
  }

  if (bvh.isLeaf) { // if its a leaf, do intersection with triangles in the leaf
    for (int i = 0; i < int(bvh.leafTriangles.size()); ++i) {
      RayIntersectionResult triResult = rayIntersectTriangle(ray, bvh.leafTriangles[i]);
      if (triResult.hit && triResult.dist < closestIntersection.dist) {
        closestIntersection = triResult;
      }
    }
  } else { // if not a leaf, do intersection with child bvh's
    for (int i = 0; i < int(bvh.children.size()); ++i) {
      RayIntersectionResult childResult = raytraceBVH(ray, bvh.children[i]);
      if (childResult.hit && childResult.dist < closestIntersection.dist) {
        closestIntersection = childResult;
      }
    }
  }
  return closestIntersection;
}

// returns BGR-D (D=depth)
cv::Vec4f traceRayToGetColor(
    const Ray& ray,
    const std::vector<Triangle>& triangles,
    const BoundingVolumeHierarchy& bvh,
    const cv::Mat_<cv::Vec3b>& skybox) {
  // intersect with geometry in bvh
  RayIntersectionResult intersectionResult = raytraceBVH(ray, bvh);

  // intersect with a textured rectangle above the rig
  if (!FLAGS_ceiling_path.empty()) {
    // solve r(depth).z = ceiling_position <=>
    const float depth = (FLAGS_ceiling_position - ray.origin[2]) / ray.dir[2];
    if (0 < depth && depth < intersectionResult.dist) {
      cv::Vec3f p = ray.origin + depth * ray.dir;
      float s = p[0] / FLAGS_ceiling_width + 0.5;
      float t = p[1] / FLAGS_ceiling_depth + 0.5;
      if (0 <= s && s < 1 && 0 <= t && t < 1) {
        // ceiling is hit, return the color
        static cv::Mat_<cv::Vec3b> ceiling =
            cv_util::imreadExceptionOnFail(FLAGS_ceiling_path, cv::IMREAD_COLOR);
        cv::Vec3f color = ceiling(t * ceiling.rows, s * ceiling.cols);
        return cv::Vec4f(color[0] / 255, color[1] / 255, color[2] / 255, depth);
      }
    }
  }

  // if nothing else was hit, intersect with sky equirect
  if (!intersectionResult.hit) {
    const float phi = acos(math_util::clamp(
        ray.dir[2], -1.0f, 1.0f)); // the min here is to avoid a nan.. other numerics can result in
                                   // a value that is epsilon > 1.0 being passed here
    const float theta = M_PI + atan2(ray.dir[1], ray.dir[0]);
    const float sampleX = (theta / (2.0 * M_PI)) * skybox.cols;
    const float sampleY = (phi / M_PI) * skybox.rows;
    const cv::Vec3b skyColor =
        skybox(std::min(int(sampleY), skybox.rows - 1), int(sampleX) % skybox.cols);
    return cv::Vec4f(
        skyColor[0] / 255.0f,
        skyColor[1] / 255.0f,
        skyColor[2] / 255.0f,
        std::numeric_limits<float>::max());
  }

  assert(
      intersectionResult.hitObjectIdx >=
      0); // if this fails, we probably forgot to bind the triangle indices in Triangle::selfIdx
  cv::Vec3f baseColor = triangles[intersectionResult.hitObjectIdx].color;
  const cv::Vec3f& normal = triangles[intersectionResult.hitObjectIdx].normal;
  const cv::Vec3f intersectionPoint = ray.origin + intersectionResult.dist * ray.dir;

  if (FLAGS_marble) {
    baseColor *= 0.7f +
        0.3f *
            std::fabs(perlin_noise::pnoise(
                FLAGS_marble_scale * intersectionPoint[0],
                FLAGS_marble_scale * intersectionPoint[1],
                FLAGS_marble_scale * intersectionPoint[2]));
  }

  const static cv::Vec3f kLightPos(2.0f, 1.0f, 5.2f);
  cv::Vec3f lightDir = kLightPos - intersectionPoint;
  lightDir /= norm(lightDir);
  const float lightCoef = .25f + .75f * std::max(0.0f, normal.dot(lightDir));

  const cv::Vec3f shadedColor = baseColor * lightCoef;
  return cv::Vec4f(shadedColor[0], shadedColor[1], shadedColor[2], intersectionResult.dist);
}

void makeIcosahedronScene(std::vector<Triangle>& triangles) {
  for (int i = 0; i < FLAGS_num_random_icosahedrons; ++i) {
    const float minAllowedCenterDist = FLAGS_min_icosahedron_dist + FLAGS_max_icosahedron_radius;

    cv::Vec3f center;
    do {
      center = cv::Vec3f(
          2.0f * (randf0to1() - 0.5) * FLAGS_max_icosahedron_dist,
          2.0f * (randf0to1() - 0.5) * FLAGS_max_icosahedron_dist,
          2.0f * (randf0to1() - 0.5) * FLAGS_max_icosahedron_dist);
    } while (norm(center) < minAllowedCenterDist);
    const float radiusRange = FLAGS_max_icosahedron_radius - FLAGS_min_icosahedron_radius;
    const float icosahedronRadius = FLAGS_min_icosahedron_radius + randf0to1() * radiusRange;
    makeIcosahedron(triangles, center, icosahedronRadius);
  }

  if (FLAGS_red_triangle) {
    float kDepth = FLAGS_min_icosahedron_dist;
    float kSide = 0.1f * kDepth;
    triangles.push_back(Triangle(
        cv::Vec3f(kDepth, 0, 0),
        cv::Vec3f(kDepth, 0, kSide),
        cv::Vec3f(kDepth, kSide, 0),
        cv::Vec3f(0, 0, 1)));
  }
}

void makeCubesScene(std::vector<Triangle>& triangles) {
  static const std::vector<cv::Vec3f> kCubeVertices = {
      {0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {0, 1, 1}, {1, 0, 0}, {1, 0, 1}, {1, 1, 0}, {1, 1, 1}};

  static const std::vector<cv::Vec3i> kCubeTriangleIndices = {
      {2, 0, 1},
      {1, 3, 2},
      {6, 2, 0},
      {0, 4, 6},
      {4, 0, 1},
      {1, 5, 4},
      {3, 1, 5},
      {5, 7, 3},
      {7, 3, 2},
      {2, 6, 7},
      {5, 4, 6},
      {6, 7, 5}};

  for (int triangle = 0; triangle < int(kCubeTriangleIndices.size()); ++triangle) {
    cv::Vec3f vertices[3];
    for (int vertex = 0; vertex < 3; ++vertex) {
      vertices[vertex] = kCubeVertices[kCubeTriangleIndices[triangle][vertex]];
    }

    // Scales and offsets are chosen to place one cube straight ahead, and one
    // smaller cube offset from centered.
    static const std::vector<float> kScales = {2, 1};
    static const std::vector<cv::Vec3f> kOffsets = {{0, 0, -25}, {5, 2, -20}};
    static const cv::Vec3f kCenterShift(-0.5, -0.5, -0.5);
    static const std::vector<std::vector<cv::Vec3f>> kColors = {
        // Red       Green      Yellow     Blue       Magenta    Cyan
        {{0, 0, 1}, {0, 1, 0}, {0, 1, 1}, {1, 0, 0}, {1, 0, 1}, {1, 1, 0}},
        // Teal        Purple       White      Orange       Salmon
        {{0.5, 1, 0},
         {1, 0, 0.5},
         {1, 1, 1},
         {0, 0.5, 1},
         {0.5, 0.5, 1},
         // Black
         {0, 0, 0}}};

    const int numCubes = int(kScales.size());
    CHECK_EQ(numCubes, int(kOffsets.size()));
    CHECK_EQ(numCubes, int(kColors.size()));
    for (int cube = 0; cube < numCubes; ++cube) {
      triangles.push_back(Triangle(
          kScales[cube] * (vertices[0] + kCenterShift) + kOffsets[cube],
          kScales[cube] * (vertices[1] + kCenterShift) + kOffsets[cube],
          kScales[cube] * (vertices[2] + kCenterShift) + kOffsets[cube],
          kColors[cube][triangle / 2]));
    }
  }
}

void makeGroundPlaneScene(std::vector<Triangle>& triangles) {
  static const float kR = 100.0f; // 100 meters
  const float z = -FLAGS_ground_plane_dist_m;
  static const std::vector<cv::Vec3f> vertices = {
      {-kR, -kR, z},
      {+kR, -kR, z},
      {+kR, +kR, z},
      {-kR, +kR, z},
  };

  const cv::Vec3f red(0, 0, 1);
  triangles.push_back(Triangle(vertices[0], vertices[1], vertices[2], red));
  triangles.push_back(Triangle(vertices[3], vertices[0], vertices[2], red));
}

std::vector<Camera> ringOfClones(const Camera& camera, int count, double radius) {
  std::vector<Camera> result(count, camera);
  for (int i = 0; i < count; ++i) {
    // the negative sign on theta here is so the camera array goes clockwise
    const double theta = -2.0 * M_PI * double(i) / double(count);
    Camera& clone = result[i];
    clone.setRotation(Camera::Vector3(cos(theta), sin(theta), 0), Camera::Vector3::UnitZ());
    clone.position = radius * clone.forward();
    clone.id = std::to_string(i);
    clone.group = "side camera";
  }
  return result;
}

std::vector<Camera> makeHorizontalRingOfPinholeCameras(
    const int numCameras,
    const float cameraArrayRadius,
    const int pixelWidth,
    const int pixelHeight,
    const float fovHorizontalDegrees,
    const float aspectRatioWoverH) {
  const float tanHalfFov = std::tan(toRadians(fovHorizontalDegrees) / 2);
  const Camera::Vector2 focal(
      (pixelWidth / 2.0) / tanHalfFov, (pixelHeight / 2.0) / (tanHalfFov / aspectRatioWoverH));
  const Camera generic(Camera::Type::RECTILINEAR, Camera::Vector2(pixelWidth, pixelHeight), focal);

  return ringOfClones(generic, numCameras, cameraArrayRadius);
}

Camera makeGenericFTheta(
    const int pixelWidth,
    const int pixelHeight,
    const int imageCircleRadius,
    const float circleFov) {
  return Camera(
      Camera::Type::FTHETA,
      Camera::Vector2(pixelWidth, pixelHeight),
      2 * imageCircleRadius / toRadians(circleFov) * Camera::Vector2(1, 1));
}

void addTopCamera(
    Camera::Rig& rig,
    const int pixelWidth,
    const int pixelHeight,
    const int imageCircleRadius,
    const float circleFov) {
  Camera top = makeGenericFTheta(pixelWidth, pixelHeight, imageCircleRadius, circleFov);
  top.position = Camera::Vector3(0, 0, FLAGS_top_cam_vertical_offset);
  top.setRotation(Camera::Vector3(0, 0, 1), Camera::Vector3(1, 0, 0));
  top.id = std::to_string(rig.size());
  rig.push_back(top);
}

std::vector<Camera> makeHorizontalRingOfFThetaCameras(
    const int numCameras,
    const float cameraArrayRadius,
    const int pixelWidth,
    const int pixelHeight,
    const int imageCircleRadius,
    const float circleFov) {
  Camera generic = makeGenericFTheta(pixelWidth, pixelHeight, imageCircleRadius, circleFov);
  return ringOfClones(generic, numCameras, cameraArrayRadius);
}

// place an ftheta camera on a sphere of some specified radius and pointing
// forward in the direction of the sphere normal.
Camera makeFThetaCameraOnSphere(
    const float /*sphereRadius*/,
    const Camera::Vector3& normal,
    const int pixelWidth,
    const int pixelHeight,
    const int imageCircleRadius,
    const float circleFov,
    const std::string& id) {
  const Camera::Vector3 worldUp(0, 0, 1);

  Camera camera = makeGenericFTheta(pixelWidth, pixelHeight, imageCircleRadius, circleFov);

  camera.position = imageCircleRadius * normal;
  Camera::Vector3 right = normal.cross(worldUp).normalized();
  camera.setRotation(normal, normal.cross(-right));
  camera.id = id;

  return camera;
}

Camera::Vector3 icosaVert(const int index) {
  const float(&v)[3] = icosahedron_data::icosahedronVertex[index];
  return Eigen::Map<const Eigen::Vector3f>(v).cast<Camera::Real>();
}

std::vector<Camera> makeDodecahedronOfFThetaCameras(
    const float cameraSphereRadius,
    const int pixelWidth,
    const int pixelHeight,
    const int imageCircleRadius,
    const float circleFov) {
  std::vector<Camera> cameras;
  for (int i = 0; i < int(ARRAY_SIZE(icosahedron_data::icosahedronVertex)); ++i) {
    cameras.emplace_back(makeFThetaCameraOnSphere(
        cameraSphereRadius,
        icosaVert(i),
        pixelWidth,
        pixelHeight,
        imageCircleRadius,
        circleFov,
        std::to_string(cameras.size())));
  }
  return cameras;
}

std::vector<Camera> makeIcosahedronOfFThetaCameras(
    const float cameraSphereRadius,
    const int pixelWidth,
    const int pixelHeight,
    const int imageCircleRadius,
    const float circleFov) {
  std::vector<Camera> cameras;
  for (const auto& indexes : icosahedron_data::icosahedronTriangle) {
    Camera::Vector3 midpoint =
        (icosaVert(indexes[0]) + icosaVert(indexes[1]) + icosaVert(indexes[2])).normalized();
    cameras.emplace_back(makeFThetaCameraOnSphere(
        cameraSphereRadius,
        midpoint,
        pixelWidth,
        pixelHeight,
        imageCircleRadius,
        circleFov,
        std::to_string(cameras.size())));
  }
  return cameras;
}

// assumes a cv::Mat of Vec3f
void corruptImageWithNoise(cv::Mat_<cv::Vec3f>& image) {
  const float a = FLAGS_noise_amplitude;
  if (a == 0.0f) {
    return;
  }
  for (int y = 0; y < image.rows; ++y) {
    for (int x = 0; x < image.cols; ++x) {
      const cv::Vec3f origColor = image(y, x);
      image(y, x) = cv::Vec3f(
          math_util::clamp<float>(origColor[0] + 2.0f * a * (randf0to1() - 0.5f), 0, 255.0f),
          math_util::clamp<float>(origColor[1] + 2.0f * a * (randf0to1() - 0.5f), 0, 255.0f),
          math_util::clamp<float>(origColor[2] + 2.0f * a * (randf0to1() - 0.5f), 0, 255.0f));
    }
  }
}

template <typename T>
cv::Mat_<T> downscale(const cv::Mat_<T>& src, int factor) {
  CHECK_EQ(0, src.cols % factor) << src.cols << factor;
  CHECK_EQ(0, src.rows % factor) << src.rows << factor;
  cv::Mat_<T> dst;
  resize(src, dst, cv::Size(src.cols / factor, src.rows / factor), 0, 0, cv::INTER_AREA);
  return dst;
}

// return (rgb, 1/depth) as Mat
std::pair<cv::Mat_<cv::Vec3f>, cv::Mat_<float>> renderMonoEquirect(
    const std::vector<Triangle>& triangles,
    const BoundingVolumeHierarchy& bvh,
    const int w,
    const int h,
    const cv::Mat_<cv::Vec3b>& skybox) {
  const int aas = FLAGS_anti_alias_supersample;
  cv::Mat_<cv::Vec3f> eqrImage(cv::Size(w * aas, h * aas));
  cv::Mat_<float> eqrInvDepth(cv::Size(w * aas, h * aas));
  for (int y = 0; y < eqrImage.rows; ++y) {
    if (y % 100 == 0) {
      LOG(INFO) << y;
    }
    for (int x = 0; x < eqrImage.cols; ++x) {
      // theta increases counterclockwise, but x increases clockwise, hence the
      // (1 - x/w) term in the equation for theta below.
      const float theta = 2.0f * M_PI * (1.0f - (x + 0.5f) / float(eqrImage.cols));
      const float phi = M_PI * (y + 0.5f) / float(eqrImage.rows);
      const cv::Vec3f rayOrigin = cv::Vec3f(0.0, 0.0, 0.0);
      const cv::Vec3f rayDir = cv::Vec3f(sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi));
      const Ray ray(rayOrigin, rayDir);
      const cv::Vec4f rgbd = traceRayToGetColor(ray, triangles, bvh, skybox);
      eqrImage(y, x) = 255.0f * head3(rgbd);
      eqrInvDepth(y, x) = math_util::clamp(1.0f / rgbd[3], 0.0f, 1.0f);
    }
  }
  return std::make_pair(downscale(eqrImage, aas), downscale(eqrInvDepth, aas));
}

std::pair<cv::Mat_<cv::Vec3f>, cv::Mat_<cv::Vec3f>> renderStereoEquirect(
    const std::vector<Triangle>& triangles,
    const BoundingVolumeHierarchy& bvh,
    const int w,
    const int h,
    const cv::Mat_<cv::Vec3b>& skybox) {
  const int aas = FLAGS_anti_alias_supersample;
  cv::Mat_<cv::Vec3f> eqrImageLeft(cv::Size(w * aas, h * aas));
  cv::Mat_<cv::Vec3f> eqrImageRight(cv::Size(w * aas, h * aas));
  for (int y = 0; y < eqrImageLeft.rows; ++y) {
    if (y % 100 == 0) {
      LOG(INFO) << y;
    }
    for (int x = 0; x < eqrImageLeft.cols; ++x) {
      // theta increases counterclockwise, but x increases clockwise, hence the
      // (1 - x/w) term in the equation for theta below.
      const float theta = 2.0f * M_PI * (1.0f - (x + 0.5f) / float(eqrImageLeft.cols));
      const float phi = M_PI * (y + 0.5f) / float(eqrImageLeft.rows);

      const cv::Vec3f rayOriginLeft =
          cv::Vec3f(cos(theta + M_PI / 2.0f), sin(theta + M_PI / 2.0f), 0.0f) *
          FLAGS_interpupillary_radius;

      const cv::Vec3f rayOriginRight =
          cv::Vec3f(cos(theta - M_PI / 2.0f), sin(theta - M_PI / 2.0f), 0.0f) *
          FLAGS_interpupillary_radius;

      const cv::Vec3f rayDir = cv::Vec3f(sin(phi) * cos(theta), sin(phi) * sin(theta), cos(phi));

      const cv::Vec3f rayColorLeft =
          head3(traceRayToGetColor(Ray(rayOriginLeft, rayDir), triangles, bvh, skybox));
      const cv::Vec3f rayColorRight =
          head3(traceRayToGetColor(Ray(rayOriginRight, rayDir), triangles, bvh, skybox));

      eqrImageLeft(y, x) = rayColorLeft * 255.0f;
      eqrImageRight(y, x) = rayColorRight * 255.0f;
    }
  }

  return std::make_pair(downscale(eqrImageLeft, aas), downscale(eqrImageRight, aas));
}

void renderCamera(
    const Camera& cam,
    const std::vector<Triangle>& triangles,
    const BoundingVolumeHierarchy& bvh,
    const cv::Mat_<cv::Vec3b>& skybox,
    cv::Mat_<cv::Vec3f>& destImage,
    cv::Mat_<float>& destDepthMap) {
  const int aas = FLAGS_anti_alias_supersample;
  cv::Mat_<cv::Vec3f> image(cv::Size(cam.resolution.x() * aas, cam.resolution.y() * aas));

  cv::Mat_<float> depthMap(image.size());
  for (int y = 0; y < image.rows; ++y) {
    if (y % 100 == 0) {
      LOG(INFO) << y;
    }
    for (int x = 0; x < image.cols; ++x) {
      const Camera::Vector2 pixel((x + 0.5f) / aas, (y + 0.5f) / aas);
      cv::Vec4f colorAndDepth;
      if (cam.isOutsideImageCircle(pixel)) {
        colorAndDepth = {0, 0, 0, FLT_MAX};
      } else {
        const Camera::Ray rig = cam.rig(pixel);
        const Ray rigCv(
            cv::Vec3f(rig.origin().x(), rig.origin().y(), rig.origin().z()),
            cv::Vec3f(rig.direction().x(), rig.direction().y(), rig.direction().z()));
        colorAndDepth = traceRayToGetColor(rigCv, triangles, bvh, skybox);
      }
      image(y, x) = 255.0f * head3(colorAndDepth);
      depthMap(y, x) = colorAndDepth[3];
    }
  }
  destImage = downscale(image, aas);
  destDepthMap = downscale(depthMap, aas);
  corruptImageWithNoise(destImage);
}

void renderCamerasThreaded(
    const cv::Mat_<cv::Vec3b>& skybox,
    const std::vector<Triangle>& triangles,
    const BoundingVolumeHierarchy& bvh,
    const std::vector<Camera>& cameras,
    const std::string destDir) {
  std::vector<std::thread> renderThreads;
  std::vector<cv::Mat_<cv::Vec3f>> images(cameras.size(), cv::Mat_<cv::Vec3f>());
  std::vector<cv::Mat_<float>> depthMaps(cameras.size(), cv::Mat_<float>());
  for (int i = 0; i < int(cameras.size()); ++i) {
    LOG(INFO) << folly::sformat("------ rendering camera {}", i);
    renderThreads.emplace_back(
        renderCamera,
        std::ref(cameras[i]),
        std::ref(triangles),
        std::ref(bvh),
        std::ref(skybox),
        std::ref(images[i]),
        std::ref(depthMaps[i]));
  }

  for (auto& renderThread : renderThreads) {
    renderThread.join();
  }

  for (int i = 0; i < int(images.size()); ++i) {
    imwriteExceptionOnFail(destDir + "/" + cameras[i].id + ".png", images[i]);
    imwriteExceptionOnFail(destDir + "/" + cameras[i].id + "_depth.png", depthMaps[i]);
    writeCvMat32FC1ToPFM(destDir + "/" + cameras[i].id + "_depth.pfm", depthMaps[i]);
  }
}

int main(int argc, char** argv) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_mode, "");
  CHECK_NE(FLAGS_skybox_path, "");

  // load skybox
  cv::Mat_<cv::Vec3b> skybox = imreadExceptionOnFail(FLAGS_skybox_path, cv::IMREAD_COLOR);

  // construct scene of triangles
  std::vector<Triangle> triangles;
  if (FLAGS_scene == "icosahedron") {
    makeIcosahedronScene(triangles);
  } else if (FLAGS_scene == "cube") {
    makeCubesScene(triangles);
  } else if (FLAGS_scene == "ground_plane") {
    makeGroundPlaneScene(triangles);
  } else {
    CHECK(false) << "unexpected scene: " << FLAGS_scene;
  }

  // bind indices of triangles. we need these to know how to color them after
  // finding the ray-bvh intersection.
  for (int i = 0; i < int(triangles.size()); ++i) {
    triangles[i].selfIdx = i;
  }

  // build bounding volume hierarchy
  LOG(INFO) << "building BVH";
  const static int kBVHStopNumTrianglesInLeaf = 20;
  const static int kBVHSplitK = 5;
  const static int kBVHMaxDepth = 50;
  BoundingVolumeHierarchy bvh = BoundingVolumeHierarchy::makeBVH(
      triangles,
      kBVHStopNumTrianglesInLeaf,
      kBVHSplitK,
      0, // start at depth = 0
      kBVHMaxDepth);

  if (FLAGS_mode == "mono_eqr") {
    CHECK_NE(FLAGS_dest_mono, "");
    CHECK_NE(FLAGS_dest_mono_depth, "");

    cv::Mat_<cv::Vec3f> monoEquirect;
    cv::Mat_<float> monoEquirectInvDepth;
    std::tie(monoEquirect, monoEquirectInvDepth) =
        renderMonoEquirect(triangles, bvh, FLAGS_eqr_width, FLAGS_eqr_height, skybox);
    imwriteExceptionOnFail(FLAGS_dest_mono, monoEquirect);
    imwriteExceptionOnFail(FLAGS_dest_mono_depth, monoEquirectInvDepth * 255.0);

  } else if (FLAGS_mode == "stereo_eqr") {
    CHECK_NE(FLAGS_dest_left, "");
    CHECK_NE(FLAGS_dest_right, "");
    CHECK_NE(FLAGS_dest_stereo, "");

    std::pair<cv::Mat_<cv::Vec3f>, cv::Mat_<cv::Vec3f>> eqrLeftRight =
        renderStereoEquirect(triangles, bvh, FLAGS_eqr_width, FLAGS_eqr_height, skybox);
    cv::Mat_<cv::Vec3f> stereoPair;
    vconcat(eqrLeftRight.first, eqrLeftRight.second, stereoPair);
    imwriteExceptionOnFail(FLAGS_dest_left, eqrLeftRight.first);
    imwriteExceptionOnFail(FLAGS_dest_right, eqrLeftRight.second);
    imwriteExceptionOnFail(FLAGS_dest_stereo, stereoPair);

  } else {
    std::vector<Camera> cameras;
    if (FLAGS_mode == "pinhole_ring") {
      cameras = makeHorizontalRingOfPinholeCameras(
          FLAGS_num_cams_in_ring,
          FLAGS_rig_radius,
          FLAGS_pinhole_width,
          FLAGS_pinhole_height,
          FLAGS_pinhole_fov_horizontal,
          FLAGS_pinhole_aspect_ratio);
    } else if (FLAGS_mode == "ftheta_ring") {
      cameras = makeHorizontalRingOfFThetaCameras(
          FLAGS_num_cams_in_ring,
          FLAGS_rig_radius,
          FLAGS_ftheta_width,
          FLAGS_ftheta_height,
          FLAGS_ftheta_image_circle_radius,
          FLAGS_ftheta_image_circle_fov);
      // add top camera, too
      addTopCamera(
          cameras,
          FLAGS_ftheta_width,
          FLAGS_ftheta_height,
          FLAGS_ftheta_image_circle_radius,
          FLAGS_ftheta_image_circle_fov);
    } else if (FLAGS_mode == "dodecahedron") {
      cameras = makeDodecahedronOfFThetaCameras(
          FLAGS_rig_radius,
          FLAGS_ftheta_width,
          FLAGS_ftheta_height,
          FLAGS_ftheta_image_circle_radius,
          FLAGS_ftheta_image_circle_fov);
    } else if (FLAGS_mode == "icosahedron") {
      cameras = makeIcosahedronOfFThetaCameras(
          FLAGS_rig_radius,
          FLAGS_ftheta_width,
          FLAGS_ftheta_height,
          FLAGS_ftheta_image_circle_radius,
          FLAGS_ftheta_image_circle_fov);
    } else if (FLAGS_mode == "rig_from_json") {
      CHECK_NE(FLAGS_rig_in, "");
      cameras = Camera::loadRig(FLAGS_rig_in);
    } else {
      CHECK(false) << "unexpected mode: " << FLAGS_mode;
    }
    if (!FLAGS_rig_out.empty()) {
      const std::vector<std::string> comments = {};
      const int doubleNumDigits = 10;
      Camera::saveRig(FLAGS_rig_out, cameras, comments, doubleNumDigits);
    }
    if (!FLAGS_dest_cam_images.empty()) {
      renderCamerasThreaded(skybox, triangles, bvh, cameras, FLAGS_dest_cam_images);
    }
  }

  return EXIT_SUCCESS;
}
