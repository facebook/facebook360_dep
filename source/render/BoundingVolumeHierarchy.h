/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <set>
#include <vector>

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include "source/render/RaytracingPrimitives.h"

namespace fb360_dep {
namespace render {

// a randomized sphere-tree bounding volume hierarchy for accelerating
// ray-triangle tests
struct BoundingVolumeHierarchy {
  bool isLeaf;
  Sphere sphere;
  std::vector<BoundingVolumeHierarchy> children;
  std::vector<Triangle> leafTriangles;

  static BoundingVolumeHierarchy makeBVH(
      const std::vector<Triangle>& triangles,
      const int thresholdNumTrisForLeaf,
      const int splitK,
      const int currDepth,
      const int maxDepth) {
    BoundingVolumeHierarchy bvh;

    // construct a sphere that contains all of the triangles we have. first,
    // find the center of mass of their vertices. then find a sufficient radius
    cv::Vec3f cm(0.0f, 0.0f, 0.0f);
    for (const Triangle& t : triangles) {
      cm += t.v0 + t.v1 + t.v2;
    }
    cm /= float(triangles.size() * 3);
    bvh.sphere.center = cm;

    bvh.sphere.radius = 0.0f;
    for (const Triangle& t : triangles) {
      bvh.sphere.radius = std::max(bvh.sphere.radius, float(norm(cm - t.v0)));
      bvh.sphere.radius = std::max(bvh.sphere.radius, float(norm(cm - t.v1)));
      bvh.sphere.radius = std::max(bvh.sphere.radius, float(norm(cm - t.v2)));
    }

    // if we hit one of the termination criteria, stop:
    // 1. reached max depth
    // 2. not enough triangles to go further
    if (currDepth >= maxDepth || int(triangles.size()) < splitK ||
        int(triangles.size()) < thresholdNumTrisForLeaf) {
      bvh.isLeaf = true;
      bvh.leafTriangles = triangles;
      return bvh;
    }

    // at this point we know it's not going to be a leaf. we will pick splitK
    // triangles at random to be "cluster centers", assign each triangle to
    // its closest cluster center, and make a child volume around each center.
    // this is similar to doing 1 iteration of k-means clustering with random
    // initialization.
    bvh.isLeaf = false;

    // pick splitK unique triangles
    std::vector<int> centerTriangleIndices;
    std::set<int> centerTriangleSet;
    while (int(centerTriangleIndices.size()) < splitK) {
      int r = rand() % triangles.size();
      if (centerTriangleSet.count(r) == 0) {
        centerTriangleIndices.push_back(r);
        centerTriangleSet.insert(r);
      }
    }

    // for each triangle, assign it to its closest cluster center.
    // closeness will be determined by distance from the first vertex.
    // this could work badly if there are really unevenly sized triangles.
    std::vector<int> clusterAssignment(triangles.size());
    for (int i = 0; i < int(triangles.size()); ++i) {
      float minDist = FLT_MAX;
      for (int j = 0; j < splitK; ++j) {
        const cv::Vec3f clusterJCenter = triangles[centerTriangleIndices[j]].v0;
        const cv::Vec3f diff = triangles[i].v0 - clusterJCenter;
        const float dist2 = diff[0] * diff[0] + diff[1] * diff[1] + diff[2] * diff[2];
        if (dist2 < minDist) {
          minDist = dist2;
          clusterAssignment[i] = j;
        }
      }
    }

    // for each cluster, recursively make a child bvh containing the triangles
    // that were assigned to that cluster.
    std::vector<std::vector<Triangle>> trianglesInCluster(splitK);
    for (int i = 0; i < int(triangles.size()); ++i) {
      trianglesInCluster[clusterAssignment[i]].push_back(triangles[i]);
    }

    for (int j = 0; j < splitK; ++j) {
      bvh.children.push_back(
          makeBVH(trianglesInCluster[j], thresholdNumTrisForLeaf, splitK, currDepth + 1, maxDepth));
    }

    return bvh;
  }
};

}; // end namespace render
}; // end namespace fb360_dep
