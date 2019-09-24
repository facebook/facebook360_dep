/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "source/util/Camera.h"

namespace fb360_dep {

// a reprojection table tells you - for each pixel in dst - where in src to look
// i.e. it implements
//   f(xy, disparity) = src.pixel(dst.rig(xy, 1 / disparity))

// the underlying implementation is a 3D piecewise linear table with enough
// points to stay within tolerance of the correct answer

// the reprojection table is a 3D table with dimensions shape. the first entry
// represents
//   (0 - margin.x, 0 - margin.y, min disparity)

// the last entry represents
//   (1 + margin.x, 1 + margin.y, max disparity)

// the reprojection table is meant to be used as a 3D texture. textures are
// addressed with texture coordinates that range from 0 at the *outside* corner
// of the first texel to 1 at the outside corner of the last texel

// that means that the value represented by the first entry must be remapped to
// the texture coordinate of the center of the first texel and vice versa:
//   0 + texel / 2 ... 1 - texel / 2

// scale and offset perform this remapping:
//   texture coordinates = input * scale + offset

// there is another similar coordinate system used internally while building the
// table, called normalized coordinates. they are 0 at the center of the first
// texel and 1 at the center of the last texel. so similar to texture coors but
// without the half-texel adjustment

class ReprojectionTable {
 public:
  ReprojectionTable(
      const Camera& dst,
      const Camera& src,
      const Camera::Vector2& tolerance,
      const Camera::Vector2& margin = {0, 0})
      : margin(margin) {
    CHECK(dst.isNormalized());
    if (src.overlap(dst) == 0) {
      shape.setOnes();
      values = {{-1, -1}}; // outside
      return;
    }

    // compute the resolution required in each dimension
    for (int dim = 0; dim < shape.size(); ++dim) {
      // start by checking error at kN^3 cells, increasing end[dim] as needed
      static const int kN = 10;
      static const float kFactor = 1.2f;
      for (IndexType end = IndexType::Constant(kN);; end[dim] *= kFactor) {
        if (isWithinTolerance(dst, src, end, dim, tolerance, margin)) {
          shape[dim] = end[dim] + 1;
          break;
        }
      }
    }

    // now create table with the computed shape
    values.resize(shape.prod());
    for (IndexType i(0, 0, 0); good(i, shape); increment(i, shape)) {
      const Camera::Vector3 normalized = divide(i, shape - 1);
      values[flatten(i, shape)] = compute(dst, src, normalized, margin);
    }
  }

  using Entry = Eigen::Vector2f; // not 16B, ok to put in vector
  using IndexType = Eigen::Array3i;

  IndexType shape;
  const Camera::Vector2 margin;
  std::vector<Entry> values;

  Eigen::Array3f getScale() const {
    // input range
    Eigen::Array3f input(1 + 2 * margin.x(), 1 + 2 * margin.y(), maxDisparity() - minDisparity());
    // output range
    Eigen::Array3f output = 1 - 1 / shape.cast<float>();
    return output / input;
  }

  Eigen::Array3f getOffset() const {
    // input
    Eigen::Array3f input(-margin.x(), -margin.y(), minDisparity());
    // output
    Eigen::Array3f output = 0.5f / shape.cast<float>();
    // output = input * scale + offset <=>
    return output - input * getScale();
  }

  // do a trilinear lookup in the table
  // note: this is slow. use a GPU, that is what they are for
  Entry lookup(const Entry& xy, const float disparity) const {
    Eigen::Array3f texcoor = getScale() * Eigen::Array3f(xy.x(), xy.y(), disparity) + getOffset();
    Eigen::Array3f unnorm = texcoor * shape.cast<float>();

    // accumulate entries from a 2^3 cube beginning with floor(unnorm - 0.5)
    IndexType begin = floor(unnorm - 0.5).cast<int>();
    CHECK((0 <= begin && begin < shape - 1).all()) << begin << "out of range";
    Entry result = {0, 0};
    for (int z = 0; z < 2; ++z) {
      for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
          IndexType index = begin + IndexType(x, y, z);
          Eigen::Array3f diff = (index.cast<float>() + 0.5f - unnorm).abs();
          float weight = (1 - diff).prod();
          CHECK(0 <= weight && weight <= 1) << weight;
          result += weight * values[flatten(index, shape)];
        }
      }
    }
    return result;
  }

  std::string toString(const int kRes = 10) const {
    std::string result;
    result += "[";
    for (float z = 0; z < kRes; ++z) {
      result += "[";
      const float d = unnormalizeDisparity((z + 0.5f) / kRes);
      for (float y = 0; y < kRes + 1; ++y) {
        result += "[";
        for (float x = 0; x < kRes + 1; ++x) {
          result += "[";
          ReprojectionTable::Entry p = lookup({x / kRes, y / kRes}, d);
          result += std::to_string(p.x()) + "," + std::to_string(p.y());
          result += "],";
        }
        result += "],";
      }
      result += "],";
    }
    result += "],";
    return result;
  }

  static float maxDisparity() {
    return 1.0f;
  }
  static float minDisparity() {
    return 1.0f / Camera::kNearInfinity;
  }
  static float normalizeDisparity(float disparity) {
    // remap disparity from minDisparity() ... maxDisparity() to 0 ... 1
    return (disparity - minDisparity()) / (maxDisparity() - minDisparity());
  }
  static float unnormalizeDisparity(float normalized) {
    // remap normalized from 0 ... 1 to minDisparity() ... maxDisparity()
    CHECK(0 <= normalized && normalized <= 1);
    return (1 - normalized) * minDisparity() + normalized * maxDisparity();
  }

 private:
  static Camera::Vector2 unnormalizeXY(
      const Camera::Vector3& normalized,
      const Camera::Vector2& margin) {
    // remap normalized.xy from 0 ... 1 to -margin ... 1 + margin
    CHECK(0 <= normalized.x() && normalized.x() <= 1);
    CHECK(0 <= normalized.y() && normalized.y() <= 1);
    const Camera::Vector2 nxy = normalized.head<2>();
    return nxy.array() * (1 + 2 * margin.array()) - margin.array();
  }

  static int flatten(const IndexType& index, const IndexType& shape) {
    CHECK((index < shape).all());
    return (index[2] * shape[1] + index[1]) * shape[0] + index[0];
  }

  static bool good(const IndexType& index, const IndexType& shape) {
    return index[2] < shape[2];
  }

  static void increment(IndexType& index, const IndexType& shape) {
    if (++index[0] == shape[0]) {
      index[0] = 0;
      if (++index[1] == shape[1]) {
        index[1] = 0;
        ++index[2];
      }
    }
  }

  static Camera::Vector3
  divide(const IndexType& num, const IndexType& den, const Camera::Real offset = 0) {
    return (num.cast<Camera::Real>() + offset) / den.cast<Camera::Real>();
  }

  static bool isWithinTolerance(
      const Camera& dst,
      const Camera& src,
      const IndexType& end,
      int dim,
      const Camera::Vector2& tolerance,
      const Camera::Vector2& margin) {
    const Eigen::Array2f tol = tolerance.array().cast<float>();
    for (IndexType i(0, 0, 0); good(i, end); increment(i, end)) {
      // compute values in center of cell
      Camera::Vector3 normalized = divide(i, end, 0.5);
      const Camera::Vector2 xy = unnormalizeXY(normalized, margin);
      if (!dst.isOutsideImageCircle(xy)) {
        const float disparity = unnormalizeDisparity(normalized.z());
        Camera::Vector2 exact;
        if (src.sees(dst.rig(xy, 1 / disparity), exact)) {
          // compute sample on either side of normalized along dimension dim
          normalized[dim] -= 0.5 / end[dim];
          Entry lo = compute(dst, src, normalized, margin);
          normalized[dim] += 1.0 / end[dim];
          Entry hi = compute(dst, src, normalized, margin);

          // does sub-texel precision error exceed tolerance?
          const float kSubtexelPrecision = 1.0f / 512;
          Eigen::Array2f sub = (hi - lo).array() * kSubtexelPrecision;
          if ((abs(sub.array()) > tol).any()) {
            return false; // error exceeds tolerance
          }

          // does linear approximation error exceed tolerance?
          Entry lin = (lo + hi) / 2 - exact.cast<float>();
          if ((abs(lin.array()) > tol).any()) {
            return false; // error exceeds tolerance
          }
        }
      }
    }
    return true;
  }

  static Entry compute(
      const Camera& dst,
      const Camera& src,
      const Camera::Vector3& normalized,
      const Camera::Vector2& margin) {
    const Camera::Vector2 xy = unnormalizeXY(normalized, margin);
    const float disparity = unnormalizeDisparity(normalized.z());
    return src.pixel(dst.rig(xy, 1 / disparity)).cast<float>();
  }
};

} // namespace fb360_dep
