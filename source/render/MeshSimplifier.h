/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <Eigen/Geometry>

#include "source/util/CvUtil.h"

#ifndef NUM_VERTEXES_FACE
#define NUM_VERTEXES_FACE 3
#endif

namespace fb360_dep::render {

// Based on paper "Surface Simplification Using Quadric Error Metrics", by
// M. Garland and P. Heckbert
class MeshSimplifier final {
 public:
  MeshSimplifier(
      const Eigen::MatrixXd& vertexesIn,
      const Eigen::MatrixXi& facesIn,
      const bool isEquiError,
      const int nThreads);
  void
  simplify(const int numFacesOut, const float strictness, const bool removeBoundaryEdges = false);
  Eigen::MatrixXd getVertexes();
  Eigen::MatrixXi getFaces();

 private:
  struct Vertex {
    std::vector<int> facesIdx;
    Eigen::Vector3d coord;
    Eigen::Matrix4d q;
    bool isBoundary;
    bool isDeleted;
    Vertex() {
      coord = Eigen::Vector3d::Zero();
      q = Eigen::Matrix4d::Zero();
      isBoundary = false;
      isDeleted = false;
    }
  };
  struct Face {
    int vertexesIdx[NUM_VERTEXES_FACE];
    Eigen::Matrix4d q;
    Eigen::Vector3d normal;
    double cost[NUM_VERTEXES_FACE];
    bool isDeleted;
    bool isTouched;
    Face() {
      q = Eigen::Matrix4d::Zero();
      normal = Eigen::Vector3d::Zero();
      isDeleted = false;
      isTouched = false;
    }
  };

  // To use STL containers with Eigen, a 16-byte-aligned allocator must be used.
  // Eigen does provide one ready for use: aligned_allocator:
  // Source: https://eigen.tuxfamily.org/dox/group__TopicStlContainers.html
  std::vector<Vertex, Eigen::aligned_allocator<Vertex>> vertexes;
  std::vector<Face, Eigen::aligned_allocator<Face>> faces;

  int numThreads;
  bool isEquiError;

  void loadVertexes(const Eigen::MatrixXd& vertexesIn, const int start, const int end);
  void loadFaces(const Eigen::MatrixXi& facesIn, const int start, const int end);
  void computeSubQuadrics(const int start, const int end);
  void computeSubError(const int start, const int end);
  void computeInitialQuadrics();
  double getThreshold(const float percRemoveEachIter);
  void removeDeletedFaces();
  void assignFaceVertexes();
  void identifySubBoundaries(const int start, const int end);
  void identifyBoundaries();
  double computeError(const Vertex& v0, const Vertex& v1, Eigen::Vector3d& pTarget);
  std::vector<int> commonFaces(const int vertexIdx0, const int vertexIdx1);
  bool haveNormalsFlipped(Eigen::Vector3d p, int vertexIdx0, int vertexIdx1);
  void updateCosts(const int vertexIdx0, const int vertexIdx1, const Eigen::Vector3d& pTarget);
  void createFinalMesh();
};

} // namespace fb360_dep::render
