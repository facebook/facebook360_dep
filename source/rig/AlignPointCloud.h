/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <ceres/ceres.h>
#include <opencv2/core/core.hpp>

#include "source/rig/RigTransform.h"
#include "source/util/Camera.h"

namespace fb360_dep {

struct Match3D {
  double score;
  Camera::Vector2 coords;
  Camera::Vector3 point;
  Camera::Vector2 lidarCoords;
};

struct PointCloudFunctor {
  static ceres::CostFunction* addResidual(
      ceres::Problem& problem,
      Camera::Vector3& rotation,
      Camera::Vector3& translation,
      Eigen::UniformScaling<double>& scale,
      const Camera& camera,
      const Match3D& match3D,
      const bool robust = false) {
    auto* cost = new CostFunction(new PointCloudFunctor(camera, match3D));
    auto* loss = robust ? new ceres::HuberLoss(1.0) : nullptr;
    problem.AddResidualBlock(cost, loss, rotation.data(), translation.data(), &scale.factor());
    return cost;
  }

  bool operator()(
      double const* const rotation,
      double const* const translation,
      double const* const scale,
      double* residuals) const {
    const Camera& newCamera = transformCamera(camera, rotation, translation, scale);
    Eigen::Map<Camera::Vector2> r(residuals);

    r = newCamera.pixel(match3D.point) - match3D.coords;

    return true;
  }

 private:
  using CostFunction = ceres::NumericDiffCostFunction<
      PointCloudFunctor,
      ceres::CENTRAL,
      2, // residuals
      3, // rotation
      3, // translation
      1>; // scale

  PointCloudFunctor(const Camera& camera, const Match3D& match3D)
      : camera(camera), match3D(match3D) {}

  const Camera& camera;
  const Match3D& match3D;
};
} // namespace fb360_dep
