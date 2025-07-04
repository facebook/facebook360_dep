/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <ceres/ceres.h>
#include <fmt/format.h>

#include "source/util/Camera.h"

namespace fb360_dep {

inline Eigen::Transform<double, 3, Eigen::Affine> generateTransform(
    const Camera::Vector3& rotation,
    const Camera::Vector3& translation,
    const Eigen::UniformScaling<double>& scale,
    const bool applyInReverse = false) {
  const Eigen::AngleAxis<Camera::Real> x(rotation.x(), Camera::Vector3::UnitX());
  const Eigen::AngleAxis<Camera::Real> y(rotation.y(), Camera::Vector3::UnitY());
  const Eigen::AngleAxis<Camera::Real> z(rotation.z(), Camera::Vector3::UnitZ());
  const Eigen::Affine3d r = Eigen::Affine3d(z * y * x);

  const Eigen::Translation3d t(translation);

  // Create transform from rotation, position and scale
  Eigen::Transform<double, 3, Eigen::Affine> xform;
  if (applyInReverse) {
    xform = r * t * scale;
  } else {
    xform = scale * t * r;
  }
  return xform;
}

inline Eigen::Transform<double, 3, Eigen::Affine> generateTransform(
    double const* const rotation,
    double const* const translation,
    double const* const scale,
    const bool applyInReverse = false) {
  return generateTransform(
      Camera::Vector3(rotation),
      Camera::Vector3(translation),
      Eigen::UniformScaling<double>(scale[0]),
      applyInReverse);
}

inline void solve(ceres::Problem& problem) {
  ceres::Solver::Options options;
  options.use_inner_iterations = true;
  options.max_num_iterations = 500;
  options.minimizer_progress_to_stdout = false;
  ceres::Solver::Summary summary;

  Solve(options, &problem, &summary);
  LOG(INFO) << summary.BriefReport();
}

struct TransformationFunctor {
  static ceres::CostFunction* addResidual(
      ceres::Problem& problem,
      Camera::Vector3& rotation,
      Camera::Vector3& translation,
      Eigen::UniformScaling<double>& scale,
      const Camera& camera,
      const Camera& referenceCamera,
      const bool robust = false) {
    auto* cost = new CostFunction(new TransformationFunctor(camera, referenceCamera));
    auto* loss = robust ? new ceres::HuberLoss(1.0) : nullptr;
    problem.AddResidualBlock(cost, loss, rotation.data(), translation.data(), &scale.factor());
    return cost;
  }

  bool operator()(
      double const* const rotation,
      double const* const translation,
      double const* const scale,
      double* residuals) const {
    const Eigen::Transform<double, 3, Eigen::Affine> xform =
        generateTransform(rotation, translation, scale);
    const Camera::Vector3 newPosition = xform * camera.position;

    Eigen::Map<Camera::Vector3> r(residuals);
    r = referenceCamera.position - newPosition;

    return true;
  }

 private:
  using CostFunction = ceres::NumericDiffCostFunction<
      TransformationFunctor,
      ceres::CENTRAL,
      3, // residuals
      3, // rotation
      3, // translation
      1>; // scale

  TransformationFunctor(const Camera& camera, const Camera& referenceCamera)
      : camera(camera), referenceCamera(referenceCamera) {}

  const Camera& camera;
  const Camera& referenceCamera;
};

inline Camera::Rig transformRig(
    const Camera::Rig& rig,
    const Camera::Vector3& rotation,
    const Camera::Vector3& translation,
    const Eigen::UniformScaling<double>& scale,
    const bool applyInReverse = false) {
  Camera::Rig result;

  // Generate a transform with rotation only (no translation or scaling) for the forward, up and
  // right vectors
  const Eigen::Transform<double, 3, Eigen::Affine> rot = generateTransform(
      rotation, Camera::Vector3(0, 0, 0), Eigen::UniformScaling<double>(1), applyInReverse);
  const Eigen::Transform<double, 3, Eigen::Affine> xform =
      generateTransform(rotation, translation, scale, applyInReverse);

  for (Camera camera : rig) {
    const Camera::Vector3 forward = camera.forward();
    const Camera::Vector3 up = camera.up();
    const Camera::Vector3 right = camera.right();
    camera.setRotation(rot * forward, rot * up, rot * right);
    camera.position = xform * camera.position;
    result.push_back(camera);
  }
  return result;
}

inline Camera::Rig alignRig(
    const Camera::Rig& rig,
    const Camera::Rig& referenceRig,
    bool lockRotation = false,
    bool lockTranslation = false,
    bool lockScale = false) {
  ceres::Problem problem;
  Camera::Vector3 rotation(0, 0, 0);
  Camera::Vector3 translation(0, 0, 0);
  Eigen::UniformScaling<double> scale(1);

  for (const auto& r : rig) {
    const Camera referenceCamera = Camera::findCameraById(r.id, referenceRig);
    TransformationFunctor::addResidual(problem, rotation, translation, scale, r, referenceCamera);
  }

  problem.SetParameterLowerBound(&scale.factor(), 0, 0.25);
  problem.SetParameterLowerBound(rotation.data(), 0, -M_PI);
  problem.SetParameterLowerBound(rotation.data(), 1, -M_PI);
  problem.SetParameterLowerBound(rotation.data(), 2, -M_PI / 2);
  problem.SetParameterUpperBound(rotation.data(), 0, M_PI);
  problem.SetParameterUpperBound(rotation.data(), 1, M_PI);
  problem.SetParameterUpperBound(rotation.data(), 2, M_PI / 2);

  if (lockRotation) {
    problem.SetParameterBlockConstant(rotation.data());
  }
  if (lockTranslation) {
    problem.SetParameterBlockConstant(translation.data());
  }
  if (lockScale) {
    problem.SetParameterBlockConstant(&scale.factor());
  }

  solve(problem);

  LOG(INFO) << fmt::format("New rotation values: {} {} {}", rotation[0], rotation[1], rotation[2]);
  LOG(INFO) << fmt::format(
      "New translation values: {} {} {}", translation[0], translation[1], translation[2]);
  LOG(INFO) << fmt::format("New scale: {}", scale.factor());
  const Camera::Rig transformedRig = transformRig(rig, rotation, translation, scale);
  return transformedRig;
}
} // namespace fb360_dep
