/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include "source/util/CvUtil.h"

namespace fb360_dep {
namespace render {

struct Ray {
  cv::Vec3f origin;
  cv::Vec3f dir;
  Ray(const cv::Vec3f& origin, const cv::Vec3f& dir) : origin(origin), dir(dir) {}
};

struct RayIntersectionResult {
  bool hit;
  float dist;
  int hitObjectIdx;
  RayIntersectionResult(const bool hit, const float dist, const int hitObjectIdx)
      : hit(hit), dist(dist), hitObjectIdx(hitObjectIdx) {}
  static RayIntersectionResult miss() {
    return RayIntersectionResult(false, 0, -1);
  }
};

struct Triangle {
  cv::Vec3f v0;
  cv::Vec3f v1;
  cv::Vec3f v2;
  cv::Vec3f e1;
  cv::Vec3f e2;
  cv::Vec3f normal; // precompute edges and normals
  cv::Vec3f color;
  int selfIdx; // used to keep track of where this triangle lives in an array

  Triangle(const cv::Vec3f& v0, const cv::Vec3f& v1, const cv::Vec3f& v2, const cv::Vec3f& color)
      : v0(v0), v1(v1), v2(v2), e1(v1 - v0), e2(v2 - v0), color(color), selfIdx(-1) {
    normal = e1.cross(e2);
    normal /= norm(normal);
  }
};

struct Sphere {
  cv::Vec3f center;
  float radius;
  cv::Vec3f color;
};

// based on http://graphicscodex.com/Sample2-RayTriangleIntersection.pdf
RayIntersectionResult rayIntersectTriangle(const Ray& ray, const Triangle& tri) {
  const cv::Vec3f q = ray.dir.cross(tri.e2);
  const float a = tri.e1.dot(q);

  // avoid near-parralel result (for numerical reasons)
  if (a * a < 0.0001f) {
    return RayIntersectionResult::miss();
  }

  const cv::Vec3f s = (ray.origin - tri.v0) / a;
  const cv::Vec3f r = s.cross(tri.e1);

  // barycentric coordinates
  const float b0 = s.dot(q);
  const float b1 = r.dot(ray.dir);
  const float b2 = 1.0f - b0 - b1;

  if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f) {
    return RayIntersectionResult::miss();
  }

  const float intersectionDist = tri.e2.dot(r);
  if (intersectionDist < 0.0f) {
    return RayIntersectionResult::miss();
  }

  return RayIntersectionResult(true, intersectionDist, tri.selfIdx);
}

// if we don't care about the intersection distance, the intersection can be
// faster. this is useful for shadows and bounding volume hierarchies.
bool rayIntersectSphereYesNo(const Ray& ray, const Sphere& sphere) {
  const cv::Vec3f rayToSphereCenter = sphere.center - ray.origin;
  const float lengthRTSC2 = rayToSphereCenter[0] * rayToSphereCenter[0] +
      rayToSphereCenter[1] * rayToSphereCenter[1] + rayToSphereCenter[2] * rayToSphereCenter[2];

  // the ray starts inside the sphere->hit
  if (lengthRTSC2 < sphere.radius * sphere.radius) {
    return true;
  }

  // check if the intersection is behind the ray
  const float closestApproach = rayToSphereCenter.dot(ray.dir);
  if (closestApproach < 0.0f) {
    return false;
  }

  const float halfCord2 =
      sphere.radius * sphere.radius + closestApproach * closestApproach - lengthRTSC2;

  if (halfCord2 < 0.0f) {
    return false;
  }

  return true;
}

}; // end namespace render
}; // end namespace fb360_dep
