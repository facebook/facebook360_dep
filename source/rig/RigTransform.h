/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/util/Camera.h"

namespace fb360_dep {

Eigen::Transform<double, 3, Eigen::Affine> generateTransform(
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

Eigen::Transform<double, 3, Eigen::Affine> generateTransform(
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

Camera transformCamera(
    const Camera& camera,
    double const* const rotation,
    double const* const translation,
    double const* const scale,
    const bool applyInReverse = false) {
  Camera result = camera;

  // Generate a transform with rotation only (no translation or scaling) for the forward, up and
  // right vectors
  const Eigen::Transform<double, 3, Eigen::Affine> rot = generateTransform(
      Camera::Vector3(rotation),
      Camera::Vector3(0, 0, 0),
      Eigen::UniformScaling<double>(1),
      applyInReverse);
  const Eigen::Transform<double, 3, Eigen::Affine> xform =
      generateTransform(rotation, translation, scale, applyInReverse);

  const Camera::Vector3 forward = camera.forward();
  const Camera::Vector3 up = camera.up();
  const Camera::Vector3 right = camera.right();
  result.setRotation(rot * forward, rot * up, rot * right);
  result.position = xform * camera.position;
  return result;
}

Camera::Rig transformRig(
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

} // namespace fb360_dep
