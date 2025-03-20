/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fstream>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <Eigen/Geometry>

#include "source/util/Camera.h"
#include "source/util/CvUtil.h"
#include "source/util/FilesystemUtil.h"
#include "source/util/MathUtil.h"

namespace fb360_dep::mesh_util {

Eigen::Vector3d calcBarycentrics(const Eigen::Vector2d& point, const Eigen::Matrix3d& triangle) {
  const Eigen::Vector2d base = triangle.row(2).head<2>();
  Eigen::Matrix2d m = triangle.topLeftCorner<2, 2>();
  m.row(0) -= base;
  m.row(1) -= base;
  Eigen::Vector3d result;
  result.head<2>() = m.transpose().colPivHouseholderQr().solve(point - base);
  result.z() = 1 - result.x() - result.y();
  return result;
}

inline void writePfm(
    const cv::Mat_<float>& original,
    const Camera::Vector2& resolution,
    const Eigen::MatrixXd& vertexes,
    const Eigen::MatrixXi& faces,
    const filesystem::path& filenamePfm) {
  LOG(INFO) << "Writing PFM file...";

  // rasterize each face into dst
  cv::Mat_<float> dst(original.size(), -FLT_MAX);
  for (int face = 0; face < faces.rows(); ++face) {
    Eigen::Matrix3d triangle;
    for (int i = 0; i < triangle.rows(); ++i) {
      triangle.row(i) = vertexes.row(faces(face, i));
    }
    // rescale x,y to original depth map's size
    triangle.col(0) *= original.cols / resolution.x();
    triangle.col(1) *= original.rows / resolution.y();
    // crude rasterizer
    Eigen::Vector3d bboxMin = triangle.colwise().minCoeff();
    Eigen::Vector3d bboxMax = triangle.colwise().maxCoeff();
    // for each pixel center in bounding box
    for (int y = floor(bboxMin.y()); y < ceil(bboxMax.y()); ++y) {
      for (int x = floor(bboxMin.x()); x < ceil(bboxMax.x()); ++x) {
        // ignore rasterization rules, just include all edges
        Eigen::Vector3d bary = calcBarycentrics({x + 0.5, y + 0.5}, triangle);
        if (bary.x() >= 0 && bary.y() >= 0 && bary.z() >= 0) {
          CHECK(0 <= x && x < dst.cols) << x << dst.cols;
          CHECK(0 <= y && y < dst.rows) << y << dst.rows;
          dst(y, x) = float(triangle.col(2).dot(bary));
        }
      }
    }
  }
  cv_util::writeCvMat32FC1ToPFM(filenamePfm, dst);
}

inline void writeDepth(
    const Eigen::MatrixXd& vertexes,
    const Eigen::MatrixXi& faces,
    const filesystem::path& fnVtx,
    const filesystem::path& fnIdx) {
  {
    std::ofstream file(fnVtx.string(), std::ios::binary);
    Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> v =
        vertexes.cast<float>();
    file.write(reinterpret_cast<char*>(v.data()), v.size() * sizeof(float));
  }
  {
    std::ofstream file(fnIdx.string(), std::ios::binary);
    Eigen::Matrix<uint32_t, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> f =
        faces.cast<uint32_t>();
    file.write(reinterpret_cast<char*>(f.data()), f.size() * sizeof(uint32_t));
  }
}

inline void writeObj(
    const Eigen::MatrixXd& vertexes,
    const Eigen::MatrixXi& faces,
    const filesystem::path& filenameObj,
    const filesystem::path& filenameMtl = "") {
  const bool st = vertexes.cols() == 5;
  CHECK(vertexes.cols() == 3 || st) << "expected xyz or xyzst";
  CHECK_EQ(st, !filenameMtl.empty()) << "texture coordinates and material go together";

  FILE* fp = fopen(filenameObj.c_str(), "w");
  CHECK(fp) << "file open failed: " << filenameObj;
  if (!filenameMtl.empty()) {
    fprintf(fp, "mtllib %s\nusemtl material\n", filenameMtl.c_str());
  }
  for (int i = 0; i < vertexes.rows(); ++i) {
    // Use the shortest representation: %e or %f
    fprintf(fp, "v %g %g %g\n", vertexes(i, 0), vertexes(i, 1), vertexes(i, 2));
    if (st) {
      fprintf(fp, "vt %g %g\n", vertexes(i, 3), vertexes(i, 4));
    }
  }
  for (int i = 0; i < faces.rows(); ++i) {
    // obj indexes are 1-based
    if (!st) {
      fprintf(fp, "f %d %d %d\n", faces(i, 0) + 1, faces(i, 1) + 1, faces(i, 2) + 1);
    } else {
      fprintf(
          fp,
          "f %d/%d %d/%d %d/%d\n",
          faces(i, 0) + 1,
          faces(i, 0) + 1,
          faces(i, 1) + 1,
          faces(i, 1) + 1,
          faces(i, 2) + 1,
          faces(i, 2) + 1);
    }
  }
  fclose(fp);
}

inline std::string writeMtl(const filesystem::path& pathObj, const filesystem::path& pathColor) {
  const std::string pathRelColor = filesystem::relative(pathColor, pathObj.parent_path()).string();

  filesystem::path pathMtl(pathObj);
  pathMtl.replace_extension(".mtl");

  std::ofstream f(pathMtl.string());
  f << "newmtl material" << std::endl;
  f << "illum 0" << std::endl;
  f << "Kd 1 1 1" << std::endl;
  f << "map_Kd " << pathRelColor << std::endl;

  return pathMtl.filename().string(); // just filename (with extension)
}

inline Eigen::MatrixXd readVertexes(const filesystem::path& fnVtx) {
  uint64_t size = filesystem::file_size(fnVtx);
  const int width = 3;
  uint64_t height = size / (width * sizeof(float));
  Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> v(height, width);
  std::ifstream file(fnVtx.string(), std::ios::binary);
  file.read(reinterpret_cast<char*>(v.data()), v.size() * sizeof(float));
  return v.cast<double>();
}

inline Eigen::MatrixXi readFaces(const filesystem::path& fnIdx) {
  uint64_t size = filesystem::file_size(fnIdx);
  const int width = 3;
  uint64_t height = size / (width * sizeof(int));
  Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> v(height, width);
  std::ifstream file(fnIdx.string(), std::ios::binary);
  file.read(reinterpret_cast<char*>(v.data()), v.size() * sizeof(int));
  return v;
}

// return a mask representing which of the 4 possible triangles to output
inline unsigned getTriangleMask(
    const Eigen::MatrixXd& verts,
    const int base,
    const int width,
    const float tearRatio,
    const bool isRigCoordinates) {
  const int tli = base;
  const int tri = base + 1;
  const int bli = base + width;
  const int bri = base + width + 1;

  // If isRigCoordinates, distance from rig center is the norm of the vector
  // If individual camera, z-coord is distance from rig center
  const double tl = isRigCoordinates ? verts.row(tli).norm() : verts(tli, 2);
  const double tr = isRigCoordinates ? verts.row(tri).norm() : verts(tri, 2);
  const double bl = isRigCoordinates ? verts.row(bli).norm() : verts(bli, 2);
  const double br = isRigCoordinates ? verts.row(bri).norm() : verts(bri, 2);

  std::vector<std::tuple<double, int>> v = {
      std::make_tuple(tl, 0),
      std::make_tuple(tr, 1),
      std::make_tuple(bl, 2),
      std::make_tuple(br, 3)};

  sort(v.begin(), v.end());

  // are all 4 values pretty close?
  if (std::get<0>(v.front()) / std::get<0>(v.back()) > tearRatio) {
    // output both triangles, splitting along the shallowest diagonal
    if (std::abs(tl - br) < std::abs(tr - bl)) {
      return 1 << 1 | 1 << 2; // triangles 1 and 2
    }
    return 1 << 0 | 1 << 3; // triangles 0 and 3
  }
  double lo = std::get<0>(v.front()) / std::get<0>(v[2]);
  double hi = std::get<0>(v[1]) / std::get<0>(v.back());

  // are the 3 lowest values pretty close?
  if (lo >= tearRatio && lo > hi) {
    // output the triangle that does not include back
    int index = std::get<1>(v.back()) ^ 0x3;
    return 1 << index;
  }

  // are the 3 highest values pretty close?
  if (hi >= tearRatio) {
    // output the triangle that does not include front
    int index = std::get<1>(v.front()) ^ 0x3;
    return 1 << index;
  }

  // don't output anything
  return 0;
}

template <typename T>
inline void addTriangle(T&& face, const int which, int base, int width) {
  // Triangles are defines counterclock-wise
  switch (which) {
    case 0: // top-left
      face(0) = base + width;
      face(1) = base + 1;
      face(2) = base;
      break;
    case 1: // top-right
      face(0) = base;
      face(1) = base + width + 1;
      face(2) = base + 1;
      break;
    case 2: // bottom-left
      face(0) = base + width + 1;
      face(1) = base;
      face(2) = base + width;
      break;
    case 3: // bottom-right
      face(0) = base + 1;
      face(1) = base + width;
      face(2) = base + width + 1;
      break;
  }
}

// Gets a set of vertexes and creates corresponding faces
// wrapHorizontally and isRigCoordinates explain the semantics of vertexes
// - wrapHorizontally: true to fix (link) meridian ends on equirect
// - isRigCoordinates: true if vertexes hold plain rig coords,
//                  false if vertex z represents distance from rig center
// examples of (wrapHorizontally, isRigCoordinates):
// - rig coordinate equirect mesh = (true, true)
// - rig coordinate camera mesh = (false, true)
// - equierror camera mesh = (false, false)
//
// tearRatio causes slivery triangles to be discarded if
//   min(depth) / max(depth) < tearRatio
//
// a reasonable value to try is ~0.95, which means it won't connect
// vertexes if one is at 10 m while the neighbor is at 9.5 m
inline Eigen::MatrixXi getFaces(
    const Eigen::MatrixXd& vertexes,
    const int width,
    const int height,
    const bool wrapHorizontally,
    const bool isRigCoordinates,
    const float tearRatio = 0.0f) {
  Eigen::MatrixXi faces(width * (height - 1) * 2, 3);
  int face = 0;
  for (int y = 0; y < height - 1; ++y) {
    for (int x = 0; x < width - 1; ++x) {
      int base = y * width + x;
      unsigned mask = getTriangleMask(vertexes, base, width, tearRatio, isRigCoordinates);
      for (int triangle = 0; triangle < 4; ++triangle) {
        if ((mask >> triangle) & 1) {
          addTriangle(faces.row(face++), triangle, base, width);
        }
      }
    }
  }

  if (wrapHorizontally) {
    // Link last and first longitudes
    // Note how triangles are always defined counterclock-wise
    for (int y = 0; y < height - 1; ++y) {
      int base = y * width;
      faces.row(face++) = Eigen::Vector3i(base + width, base, base + width - 1);
      faces.row(face++) = Eigen::Vector3i(base + width - 1, base + 2 * width - 1, base + width);
    }
  }

  return faces.topRows(face);
}

inline Eigen::MatrixXd getVertexesEquirect(const cv::Mat_<float>& disparity, const float maxDepth) {
  const int width = disparity.cols;
  const int height = disparity.rows;
  Eigen::MatrixXd vertexes(width * height, 3);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float u = float(x + 0.5) / float(width);
      const float v = float(y + 0.5) / float(height);
      const float theta = u * 2.0f * M_PI;
      const float phi = v * M_PI;
      const float depth = std::fmin(maxDepth, 1.0f / disparity(y, x));
      vertexes.row(y * width + x) =
          depth * Eigen::Vector3d(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));
    }
  }
  return vertexes;
}

// for equi error discussion, see cameraMeshVS in RigScene.cpp
inline Eigen::MatrixXd getVertexesEquiError(const cv::Mat_<float>& depth, const Camera& camera) {
  int width = depth.cols;
  int height = depth.rows;
  const double kRadius = 1; // change this to 100 if rig is in cm
  const double scale = camera.getScalarFocal() * kRadius;
  Eigen::MatrixXd vertexes(width * height, 3);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      // equi-error coordinates
      Eigen::Vector3d equi(
          camera.resolution.x() / width * (x + 0.5),
          camera.resolution.y() / height * (y + 0.5),
          scale / depth(y, x));

      // actual rig coordinates would be
      //   Camera::Vector2 pixel(x + 0.5, y + 0.5);
      //   Camera::Vector3 rig = camera.rig(pixel).pointAt(depth(y, x));
      int i = y * width + x;
      for (int c = 0; c < equi.size(); ++c) {
        vertexes(i, c) = equi[c];
      }
    }
  }
  return vertexes;
}

// Assumes vertexes have been generated in order from a depth map stored row-major so that
// mask(y, x) corresponds to vertexes(y * mask.cols + x)
inline void applyMaskToVertexesAndFaces(
    Eigen::MatrixXd& vertexes,
    Eigen::MatrixXi& faces,
    const cv::Mat_<bool> mask) {
  const int width = mask.cols;
  const int height = mask.rows;
  CHECK_EQ(width * height, vertexes.rows());
  bool* originalVertexMask = (bool*)mask.data;

  // Keep faces only if all vertexes have a non-zero mask
  std::vector<int> outputFaceIndexes;
  for (int i = 0; i < faces.rows(); ++i) {
    const Eigen::Vector3i& face = faces.row(i);
    bool allVertexesRetained = true;
    for (int j = 0; j < face.size(); ++j) {
      if (!originalVertexMask[face(j)]) {
        allVertexesRetained = false;
        break;
      }
    }
    if (allVertexesRetained) {
      outputFaceIndexes.push_back(i);
    }
  }

  // Keep only vertexes of retained faces
  std::vector<bool> vertexMask(vertexes.rows(), false);
  for (int i : outputFaceIndexes) {
    const Eigen::Vector3i& face = faces.row(i);
    for (int j = 0; j < face.size(); ++j) {
      vertexMask[face(j)] = true;
    }
  }

  // Compute mapping between old and new vertex indices
  std::map<int, int> inputToOutputVertexIndexes;
  int numOutputVertexes = 0;
  for (int i = 0; i < vertexes.rows(); ++i) {
    if (vertexMask[i]) {
      inputToOutputVertexIndexes[i] = numOutputVertexes++;
    }
  }

  // create output vertex matrix
  Eigen::MatrixXd inputVertexes = vertexes;
  vertexes = Eigen::MatrixXd(numOutputVertexes, 3);
  for (auto iter = inputToOutputVertexIndexes.begin(); iter != inputToOutputVertexIndexes.end();
       ++iter) {
    vertexes.row(iter->second) = inputVertexes.row(iter->first);
  }

  // Create output face matrix and re-index vertexes
  Eigen::MatrixXi inputFaces = faces;
  faces = Eigen::MatrixXi(outputFaceIndexes.size(), 3);
  for (ssize_t i = 0; i < ssize(outputFaceIndexes); ++i) {
    const Eigen::Vector3i& face = inputFaces.row(outputFaceIndexes[i]);
    for (int j = 0; j < face.size(); ++j) {
      faces(i, j) = inputToOutputVertexIndexes[face(j)];
    }
  }
}

// Add texture coordinates to vertexes
inline void addTextureCoordinatesEquirect(Eigen::MatrixXd& vertexes) {
  vertexes.conservativeResize(Eigen::NoChange, 5);
  for (int v = 0; v < vertexes.rows(); ++v) {
    // similar to createEquirectProgram() in ParallaxViewer.cpp:
    // texture goes +x, +z, -x, -z, +x from left to right (0 to 1)
    // and -y to +y from top to bottom (0 to 1)
    Eigen::Vector3d pos = vertexes.row(v).head<3>();
    const double xzNorm = Eigen::Vector2d(pos.x(), pos.z()).norm();
    vertexes(v, 3) = std::atan2(-pos.z(), -pos.x()) * 0.5 / M_PI + 0.5;
    vertexes(v, 4) = -std::atan2(-pos.y(), xzNorm) / M_PI + 0.5;
  }
}

} // namespace fb360_dep::mesh_util
