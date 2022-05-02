/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma GCC diagnostic push
#if !defined(__has_warning)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#elif __has_warning("-Wmaybe-uninitialized")
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include "source/render/MeshSimplifier.h"
#pragma GCC diagnostic pop

#include <map>
#include <set>
#include <thread>

#include <folly/Format.h>

using namespace fb360_dep;
using namespace fb360_dep::render;

void MeshSimplifier::loadVertexes(
    const Eigen::MatrixXd& vertexesIn,
    const int begin,
    const int end) {
  for (int i = begin; i < end; ++i) {
    Vertex vertex;
    vertex.coord = vertexesIn.row(i);
    vertexes[i] = vertex;
  }
}

void MeshSimplifier::loadFaces(const Eigen::MatrixXi& facesIn, const int begin, const int end) {
  for (int i = begin; i < end; ++i) {
    Face face;
    for (int j = 0; j < NUM_VERTEXES_FACE; ++j) {
      face.vertexesIdx[j] = facesIn(i, j);
    }
    faces[i] = face;
  }
}

MeshSimplifier::MeshSimplifier(
    const Eigen::MatrixXd& vertexesIn,
    const Eigen::MatrixXi& facesIn,
    const bool equiError,
    const int nThreads) {
  numThreads = nThreads;
  isEquiError = equiError;

  LOG(INFO) << folly::sformat("Getting {} vertexes...", vertexesIn.rows());

  vertexes.resize(vertexesIn.rows());
  std::vector<std::thread> threads;
  for (int i = 0; i < numThreads; ++i) {
    const int begin = i * vertexesIn.rows() / numThreads;
    const int end = (i + 1) * vertexesIn.rows() / numThreads;
    threads.emplace_back(&MeshSimplifier::loadVertexes, this, vertexesIn, begin, end);
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  LOG(INFO) << folly::sformat("Getting {} faces...", facesIn.rows());

  faces.resize(facesIn.rows());
  threads.clear();
  for (int i = 0; i < numThreads; ++i) {
    const int begin = i * facesIn.rows() / numThreads;
    const int end = (i + 1) * facesIn.rows() / numThreads;
    threads.emplace_back(&MeshSimplifier::loadFaces, this, facesIn, begin, end);
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

Eigen::MatrixXd MeshSimplifier::getVertexes() {
  Eigen::MatrixXd vertexesOut(vertexes.size(), NUM_VERTEXES_FACE);
  for (int i = 0; i < int(vertexes.size()); ++i) {
    vertexesOut.row(i) = vertexes[i].coord;
  }
  return vertexesOut;
}

Eigen::MatrixXi MeshSimplifier::getFaces() {
  Eigen::MatrixXi facesOut(faces.size(), NUM_VERTEXES_FACE);
  for (int i = 0; i < int(faces.size()); ++i) {
    Face& face = faces[i];
    facesOut.row(i) =
        Eigen::Vector3i(face.vertexesIdx[0], face.vertexesIdx[1], face.vertexesIdx[2]);
  }
  return facesOut;
}

double computeFastError(Eigen::Matrix4d q, Eigen::Vector3d& v) {
  return q(0, 0) * v.x() * v.x() + 2 * q(0, 1) * v.x() * v.y() + 2 * q(0, 2) * v.x() * v.z() +
      2 * q(0, 3) * v.x() + q(1, 1) * v.y() * v.y() + 2 * q(1, 2) * v.y() * v.z() +
      2 * q(1, 3) * v.y() + q(2, 2) * v.z() * v.z() + 2 * q(2, 3) * v.z() + q(3, 3);
}

// error = vT * Q * v, where Q = (Q1 + Q2)
// We find the target vertex by setting the derivative of error to 0 and solving
// for v. This is equivalent to solving
// v' = Q * [0 0 0 1]
//      | q11 q12 q13 q14 |^-1   | 0 |
//    = | q12 q22 q23 q24 |    * | 0 |
//      | q13 q23 q33 q34 |      | 0 |
//      | 0   0   0   1   |      | 1 |
// Note that we set the last row of Q to [0 0 0 1] because v is an homogeneous
// vector
// If the modified Q is not invertible, or if we are at the mesh boundary, we
// use the optimal (lowest cost) vector from v1, v2, and (v1 + v2) / 2
//
// Since Q is symmetric and the last row is homogeneous we can simplify the
// error computation:
// 1) The determinant of Q is just the determinant of the top-left 3x3
// 2) Multiplying Q^-1 by a homogeneous vector means we only want the first 3
//    elements of the last column of Q^-1. This is, we only need to compute 3
//    3x3 minors, so that v = 1/det * [-M234; M134; -M124], where
//    MXYZ = det(colX|colY|colZ)
// 3) The error becomes
//    vT * Q * v = q11x^2 + 2q12xy + 2q13xz + 2q14x + q22y^2 + 2q23yz + 2q24y +
//                 q33z^2 + 2q34z + q44
double MeshSimplifier::computeError(
    const Vertex& vertex0,
    const Vertex& vertex1,
    Eigen::Vector3d& vTarget) {
  Eigen::Matrix4d q = vertex0.q + vertex1.q;
  const double det = q.block<3, 3>(0, 0).determinant();

  // Do not do quadric approach on boundary edges
  const bool isBoundary = vertex0.isBoundary && vertex1.isBoundary;

  double error;
  if (det != 0 && !isBoundary) {
    Eigen::Matrix3d mX;
    mX << q(0, 1), q(0, 2), q(0, 3), q(1, 1), q(1, 2), q(1, 3), q(2, 1), q(2, 2), q(2, 3);
    Eigen::Matrix3d mY;
    mY << q(0, 0), q(0, 2), q(0, 3), q(1, 0), q(1, 2), q(1, 3), q(2, 0), q(2, 2), q(2, 3);
    Eigen::Matrix3d mZ;
    mZ << q(0, 0), q(0, 1), q(0, 3), q(1, 0), q(1, 1), q(1, 3), q(2, 0), q(2, 1), q(2, 3);
    vTarget = 1 / det * Eigen::Vector3d(-mX.determinant(), mY.determinant(), -mZ.determinant());

    error = computeFastError(q, vTarget);
  } else {
    std::vector<Eigen::Vector3d> vCandidates = {
        vertex0.coord, vertex1.coord, (vertex0.coord + vertex1.coord) / 2};
    std::vector<double> errors;
    for (auto& v : vCandidates) {
      errors.push_back(computeFastError(q, v));
    }
    auto minIter = std::min_element(errors.begin(), errors.end());
    vTarget = vCandidates[minIter - errors.begin()];
    error = *minIter;
  }

  // If mesh is not distributed to have equierror, we need to penalize costs for
  // changes further away (e.g. a change of 1m far away is less noticeable than
  // a change of 1m up close). Dividing by the square distance of the target
  // vertex is a good penalization
  return isEquiError ? error : error / vTarget.squaredNorm();
}

// Q = q * qT
// q = [a, b, c, d]
// n = (p1 - p0) x (p2 - p0) = [a, b, c]
// d = -n.dot(p0)
// n: normal to plane going though [p0, p1, p2]
//     | aa ab ac ad |
// Q = | ab bb bc bd |
//     | ac bc cc cd |
//     | ad bd cd dd |
// Note how Q is symmetric
void MeshSimplifier::computeSubQuadrics(const int begin, const int end) {
  for (int i = begin; i < end; ++i) {
    Face& face = faces[i];
    face.isDeleted = false;
    Eigen::Vector3d p[NUM_VERTEXES_FACE];
    for (int j = 0; j < NUM_VERTEXES_FACE; ++j) {
      p[j] = vertexes[face.vertexesIdx[j]].coord;
    }
    Eigen::Vector3d normal = (p[1] - p[0]).cross(p[2] - p[0]).normalized();
    face.normal = normal;
    Eigen::Vector4d q(normal.x(), normal.y(), normal.z(), -normal.dot(p[0]));
    face.q = q * q.transpose();
  }
}

void MeshSimplifier::computeSubError(const int begin, const int end) {
  for (int i = begin; i < end; ++i) {
    Face& face = faces[i];
    for (int j = 0; j < NUM_VERTEXES_FACE; ++j) {
      const int i0 = face.vertexesIdx[j];
      const int i1 = face.vertexesIdx[(j + 1) % NUM_VERTEXES_FACE];
      Eigen::Vector3d p;
      face.cost[j] = computeError(vertexes[i0], vertexes[i1], p);
    }
  }
}

void MeshSimplifier::computeInitialQuadrics() {
  std::vector<std::thread> threads;

  LOG(INFO) << "Computing quadrics...";
  for (int i = 0; i < numThreads; ++i) {
    const int begin = i * faces.size() / numThreads;
    const int end = (i + 1) * faces.size() / numThreads;
    threads.emplace_back(&MeshSimplifier::computeSubQuadrics, this, begin, end);
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  LOG(INFO) << "Accumulating quadrics...";
  for (auto& face : faces) {
    for (int j = 0; j < NUM_VERTEXES_FACE; ++j) {
      vertexes[face.vertexesIdx[j]].q += face.q;
    }
  }

  LOG(INFO) << "Updating faces costs...";
  threads.clear();
  for (int i = 0; i < numThreads; ++i) {
    const int begin = i * faces.size() / numThreads;
    const int end = (i + 1) * faces.size() / numThreads;
    threads.emplace_back(&MeshSimplifier::computeSubError, this, begin, end);
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

// Remove from the list all faces that have been marked as deleted
void MeshSimplifier::removeDeletedFaces() {
  int idx = 0;
  for (auto& face : faces) {
    face.isTouched = false;
    if (!face.isDeleted) {
      faces[idx++] = face;
    }
  }
  faces.resize(idx);
}

void MeshSimplifier::assignFaceVertexes() {
  for (auto& vertex : vertexes) {
    vertex.facesIdx.clear();
  }

  for (int i = 0; i < int(faces.size()); ++i) {
    const Face& face = faces[i];
    for (int j = 0; j < NUM_VERTEXES_FACE; ++j) {
      vertexes[face.vertexesIdx[j]].facesIdx.push_back(i);
    }
  }
}

std::vector<int> MeshSimplifier::commonFaces(const int vIdx0, const int vIdx1) {
  std::vector<int> commonFaces;
  for (int i1 : vertexes[vIdx0].facesIdx) {
    for (int i2 : vertexes[vIdx1].facesIdx) {
      if (i1 == i2) {
        commonFaces.push_back(i1);
      }
    }
  }
  return commonFaces;
}

// A vertex is is considered a to be on the boundary if it only shares one face
// with any adjacent vertex
void MeshSimplifier::identifySubBoundaries(const int begin, const int end) {
  for (int i = begin; i < end; ++i) {
    vertexes[i].isBoundary = false;
  }

  for (int i = begin; i < end; ++i) {
    // Ignore if it has already been marked as boundary
    if (vertexes[i].isBoundary) {
      continue;
    }

    // If it only has one face, it is a boundary
    if (vertexes[i].facesIdx.size() == 1) {
      vertexes[i].isBoundary = true;
      continue;
    }

    bool isBorder = false;
    std::set<int> vertexesVisited;
    for (auto& faceIdx : vertexes[i].facesIdx) {
      for (int j = 0; j < NUM_VERTEXES_FACE; ++j) {
        const int vIdx = faces[faceIdx].vertexesIdx[j];
        if (vIdx != i) {
          // Check if we already visited this vertex on a previous face
          if (vertexesVisited.count(vIdx) == 0) {
            vertexesVisited.insert(vIdx);
            if (vertexes[vIdx].facesIdx.size() == 1 || commonFaces(i, vIdx).size() == 1) {
              vertexes[vIdx].isBoundary = true;
              isBorder = true;
              continue;
            }
          }
        }
      }
    }
    if (isBorder) {
      vertexes[i].isBoundary = true;
    }
  }
}

void MeshSimplifier::identifyBoundaries() {
  std::vector<std::thread> threads;
  for (int i = 0; i < numThreads; ++i) {
    const int begin = i * vertexes.size() / numThreads;
    const int end = (i + 1) * vertexes.size() / numThreads;
    threads.emplace_back(&MeshSimplifier::identifySubBoundaries, this, begin, end);
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

double MeshSimplifier::getThreshold(const float strictness) {
  std::vector<double> errors(faces.size() * NUM_VERTEXES_FACE);
  for (int i = 0; i < int(faces.size()); ++i) {
    Face& face = faces[i];
    for (int j = 0; j < NUM_VERTEXES_FACE; ++j) {
      errors[i * NUM_VERTEXES_FACE + j] = face.cost[j];
    }
  }
  const int idxPerc = strictness * (errors.size() - 1);
  std::nth_element(errors.begin(), errors.begin() + idxPerc, errors.end());
  return errors[idxPerc];
}

// Compare the normal of each neighboring face before and after the contraction.
// If the normal flips, that contraction will be disallowed
bool MeshSimplifier::haveNormalsFlipped(Eigen::Vector3d p, int vIdx0, int vIdx1) {
  for (int i = 0; i < int(vertexes[vIdx0].facesIdx.size()); ++i) {
    Face& face = faces[vertexes[vIdx0].facesIdx[i]];

    // Ignore faces marked as deleted
    if (face.isDeleted) {
      continue;
    }

    // Find vertex index in the face, clockwise
    int order = 0;
    for (int j = 0; j < NUM_VERTEXES_FACE; ++j) {
      if (face.vertexesIdx[j] == vIdx0) {
        order = j;
        break;
      }
    }
    int i0 = face.vertexesIdx[(order + 1) % 3];
    int i1 = face.vertexesIdx[(order + 2) % 3];

    // Ignore edge formed by vertex0 and vertex1 (= deleted face)
    if (i0 == vIdx1 || i1 == vIdx1) {
      continue;
    }

    // Opposite directions begin at negative dot product
    Eigen::Vector3d v0 = (vertexes[i0].coord - p).normalized();
    Eigen::Vector3d v1 = (vertexes[i1].coord - p).normalized();
    Eigen::Vector3d normal = v0.cross(v1).normalized();
    if (normal.dot(face.normal) < 0) {
      return true;
    }
  }
  return false;
}

void MeshSimplifier::updateCosts(const int vIdx0, const int vIdx1, const Eigen::Vector3d& pTarget) {
  // Both vertexes collapse into new one, defined by pTarget and Q0 + Q1
  // Vertex 0 will act as new vertex
  vertexes[vIdx0].coord = pTarget;
  vertexes[vIdx0].q += vertexes[vIdx1].q;

  // Gather all faces touched by both vertexes
  // No need to check for duplicates, since they are faces marked for deletion
  std::vector<int> allFaces(vertexes[vIdx0].facesIdx);
  std::vector<int> facesVertex1 = vertexes[vIdx1].facesIdx;
  allFaces.insert(allFaces.end(), facesVertex1.begin(), facesVertex1.end());

  for (int faceIdx : allFaces) {
    Face& face = faces[faceIdx];

    if (face.isDeleted) {
      continue;
    }

    // Find vertex index in the face
    for (int i = 0; i < NUM_VERTEXES_FACE; ++i) {
      if (face.vertexesIdx[i] == vIdx0 || face.vertexesIdx[i] == vIdx1) {
        face.vertexesIdx[i] = vIdx0;
        face.isTouched = true;
        break;
      }
    }

    for (int i = 0; i < NUM_VERTEXES_FACE; ++i) {
      const int i0 = face.vertexesIdx[i];
      const int i1 = face.vertexesIdx[(i + 1) % NUM_VERTEXES_FACE];

      Eigen::Vector3d p;
      face.cost[i] = computeError(vertexes[i0], vertexes[i1], p);
    }
  }
}

// Reassign all indeces of all vertexes and faces to construct final mesh
void MeshSimplifier::createFinalMesh() {
  for (auto& vertex : vertexes) {
    vertex.isDeleted = true;
    vertex.facesIdx.clear();
  }

  // Remove faces marked for deletion, and mark all valid vertexes
  removeDeletedFaces();
  for (auto& face : faces) {
    for (int i = 0; i < NUM_VERTEXES_FACE; ++i) {
      vertexes[face.vertexesIdx[i]].isDeleted = false;
    }
  }

  // Reassign vertexes coordinates
  std::map<int, int> mapVertexes;
  int currIdx = 0;
  for (int i = 0; i < int(vertexes.size()); ++i) {
    if (!vertexes[i].isDeleted) {
      mapVertexes.insert(std::make_pair(i, currIdx));
      vertexes[currIdx++].coord = vertexes[i].coord;
    }
  }
  vertexes.resize(currIdx);

  // Reassign faces' vertexes
  for (auto& face : faces) {
    for (int i = 0; i < NUM_VERTEXES_FACE; ++i) {
      face.vertexesIdx[i] = mapVertexes.at(face.vertexesIdx[i]);
    }
  }
}

void MeshSimplifier::simplify(
    const int numFacesOut,
    const float strictness,
    const bool removeBoundaryEdges) {
  // Compute Q matrices and errors for all vertexes
  LOG(INFO) << "Computing initial costs...";
  computeInitialQuadrics();

  const int numFacesIn = faces.size();
  int numFacesDeleted = 0;
  int numFacesDeletedPrev = 0;
  double threshold = 0;
  int countNumFacesSame = 0;
  int iteration = 0;
  while (int(faces.size()) > numFacesOut) {
    removeDeletedFaces();

    if (iteration == 0) {
      LOG(INFO) << "Assigning faces and vertexes...";
    }
    assignFaceVertexes();

    if (iteration == 0) {
      LOG(INFO) << "Identifying boundaries...";
      identifyBoundaries();
    }

    if (iteration == 0 || numFacesDeletedPrev != numFacesDeleted) {
      threshold = getThreshold(strictness);
      countNumFacesSame = 0;
    } else {
      // Scale threshold up to avoid getting stuck
      threshold *= 2 * ++countNumFacesSame;
      if (std::isinf(threshold)) {
        // Sometimes there are no qualifying faces left, so the threshold would
        // increase without limit
        break;
      }
    }
    numFacesDeletedPrev = numFacesDeleted;

    if (iteration % 1 == 0) {
      LOG(INFO) << folly::sformat(
          "Iter: {}, faces: {}, threshold: {}", iteration, faces.size(), threshold);
    }

    for (auto& face : faces) {
      if (face.isDeleted || face.isTouched) {
        continue;
      }

      // Select all valid vertex pairs
      for (int i = 0; i < NUM_VERTEXES_FACE; ++i) {
        // Ignore if error (cost) is higher than threshold
        if (face.cost[i] > threshold) {
          continue;
        }

        int vIdx0 = face.vertexesIdx[i];
        int vIdx1 = face.vertexesIdx[(i + 1) % NUM_VERTEXES_FACE];

        // Ignore non-boundary edges with one boundary vertex
        if (vertexes[vIdx0].isBoundary != vertexes[vIdx1].isBoundary) {
          continue;
        }

        // Optionally ignore boundary edges entirely
        if (!removeBoundaryEdges && (vertexes[vIdx0].isBoundary || vertexes[vIdx1].isBoundary)) {
          continue;
        }

        // Compute optimal target point
        Eigen::Vector3d pTarget;
        computeError(vertexes[vIdx0], vertexes[vIdx1], pTarget);

        // Prevent mesh inversion
        if (haveNormalsFlipped(pTarget, vIdx0, vIdx1) ||
            haveNormalsFlipped(pTarget, vIdx1, vIdx0)) {
          continue;
        }

        // Mark faces for deletion. These are the faces common to both vertexes
        const std::vector<int> commonFacesIdxs = commonFaces(vIdx0, vIdx1);
        for (int faceIdx : commonFacesIdxs) {
          faces[faceIdx].isDeleted = true;
        }
        numFacesDeleted += commonFacesIdxs.size();

        // Update costs of all valid pairs involving new vertex
        updateCosts(vIdx0, vIdx1, pTarget);

        // Nothing else to do on the remaining vertexes of current face
        break;
      }

      // Check remaining faces after processing every face
      if (numFacesIn - numFacesDeleted <= numFacesOut) {
        break;
      }
    }
    ++iteration;
  }

  // Assign final values to vertexes and faces
  LOG(INFO) << "Creating final mesh...";
  createFinalMesh();
}
