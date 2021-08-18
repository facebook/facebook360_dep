/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/util/Camera.h"

#include <unsupported/Eigen/Polynomials>

#include <folly/Format.h>

namespace fb360_dep {

const Camera::Real Camera::kNearInfinity = 1e4;

Camera::Camera(const Type type, const Vector2& res, const Vector2& focal)
    : type(type), resolution(res), focal(focal) {
  position.setZero();
  rotation.setIdentity();
  principal = resolution / 2;
  setDefaultDistortion();
  setDefaultFov();
}

Camera::Camera(const folly::dynamic& json) {
  CHECK_GE(json["version"].asDouble(), 1.0);

  id = json["id"].getString();

  type = deserializeType(json["type"]);

  position = deserializeVector<3>(json["origin"]);

  setRotation(
      deserializeVector<3>(json["forward"]),
      deserializeVector<3>(json["up"]),
      deserializeVector<3>(json["right"]));

  resolution = deserializeVector<2>(json["resolution"]);

  if (json.count("principal")) {
    principal = deserializeVector<2>(json["principal"]);
  } else {
    principal = resolution / 2;
  }

  if (json.count("distortion")) {
    const folly::dynamic& entry = json["distortion"];
    CHECK_LE(entry.size(), getDistortion().size()) << "bad distortion " << entry;
    Distortion distortion;
    for (int i = 0; i < distortion.size(); ++i) {
      distortion[i] = i < int(entry.size()) ? entry[i].asDouble() : 0;
    }
    setDistortion(distortion);
  } else {
    setDefaultDistortion();
  }

  if (json.count("fov")) {
    setFov(json["fov"].asDouble());
  } else {
    setDefaultFov();
  }

  focal = deserializeVector<2>(json["focal"]);

  if (json.count("group")) {
    group = json["group"].getString();
  }
}

void Camera::setRotation(const Vector3& forward, const Vector3& up, const Vector3& right) {
  CHECK_LT(right.cross(up).dot(forward), 0) << "rotation must be right-handed";
  rotation.row(2) = -forward; // +z is back
  rotation.row(1) = up; // +y is up
  rotation.row(0) = right; // +x is right
  // re-unitarize
  const Camera::Real tol = 0.001;
  CHECK(rotation.isUnitary(tol)) << rotation << " is not close to unitary";
  Eigen::AngleAxis<Camera::Real> aa(rotation);
  rotation = aa.toRotationMatrix();
}

void Camera::setRotation(const Vector3& forward, const Vector3& up) {
  setRotation(forward, up, forward.cross(up));
}

void Camera::setRotation(const Vector3& angleAxis) {
  // convert angle * axis to rotation matrix
  Real angle = angleAxis.norm();
  Vector3 axis = angleAxis / angle;
  if (angle == 0) {
    axis = Vector3::UnitX();
  }
  rotation = Eigen::AngleAxis<Real>(angle, axis).toRotationMatrix();
}

Camera::Vector3 Camera::getRotation() const {
  // convert rotation matrix to angle * axis
  Eigen::AngleAxis<Real> angleAxis(rotation);
  if (angleAxis.angle() > M_PI) {
    angleAxis.angle() = 2 * M_PI - angleAxis.angle();
    angleAxis.axis() = -angleAxis.axis();
  }

  return angleAxis.angle() * angleAxis.axis();
}

void Camera::setDefaultDistortion() {
  distortion_.setZero();
  distortionMax_ = INFINITY;
}

void Camera::setDistortion(const Distortion& distortion) {
  // ignore trailing zeros
  Eigen::Index count = distortion.size();
  while (distortion[count - 1] == 0) {
    if (--count == 0) {
      return setDefaultDistortion();
    }
  }

  // distortion polynomial is x + d[0] * x^3 + d[1] * x^5 ...
  // derivative is: 1 + d[0] * 3 x^2 + d[1] * 5 x^4 ...
  // using y = x^2: 1 + d[0] * 3 y + d[1] * 5 y^2 ...
  VLOG(2) << "Solving for camera distortions..."; // HACK: Prevents seg fault in solver
  Eigen::Matrix<Real, Eigen::Dynamic, 1> derivative(count + 1);
  derivative[0] = 1;
  for (int i = 0; i < count; ++i) {
    derivative[i + 1] = distortion[i] * (2 * i + 3);
  }

  // find real roots in derivative
  Eigen::PolynomialSolver<Real, Eigen::Dynamic> solver;
  solver.compute(derivative);
  std::vector<Real> roots;
  solver.realRoots(roots);

  // find smallest root greater than zero
  Real y = INFINITY;
  for (const Real& root : roots) {
    if (0 < root && root < y) {
      y = root;
    }
  }

  distortion_ = distortion;
  distortionMax_ = sqrt(y);
}

#ifndef SUPPRESS_RIG_IO

folly::dynamic Camera::serialize() const {
  // clang-format off
  folly::dynamic result = folly::dynamic::object("version", 1)("type", serializeType(type))(
      "origin", serializeVector(position))("forward", serializeVector(forward()))(
      "up", serializeVector(up()))("right", serializeVector(right()))(
      "resolution", serializeVector(resolution))("focal", serializeVector(focal))("id", id);
  // clang-format on
  if (principal != resolution / 2) {
    result["principal"] = serializeVector(principal);
  }
  if (!getDistortion().isZero()) {
    result["distortion"] = serializeVector(getDistortion());
  }
  if (!isDefaultFov()) {
    result["fov"] = getFov();
  }
  if (!group.empty()) {
    result["group"] = group;
  }

  return result;
}

#endif // SUPPRESS_RIG_IO

void Camera::setScalarFocal(const Real& scalar) {
  focal = {scalar, -scalar};
}

Camera::Real Camera::getScalarFocal() const {
  CHECK_EQ(focal.x(), -focal.y()) << "pixels are not square";
  return focal.x();
}

Camera::Real Camera::getDefaultCosFov(Camera::Type type) {
  switch (type) {
    case Camera::Type::RECTILINEAR:
    case Camera::Type::ORTHOGRAPHIC:
      return 0; // hemisphere
    default:
      return -1; // sphere
  }
}

void Camera::setDefaultFov() {
  cosFov = getDefaultCosFov(type);
}

void Camera::setFov(const Real& fov) {
  cosFov = std::cos(fov);
  CHECK(cosFov >= getDefaultCosFov(type));
}

Camera::Real Camera::getFov() const {
  return std::acos(cosFov);
}

bool Camera::isDefaultFov() const {
  return cosFov == getDefaultCosFov(type);
}

Camera Camera::rescale(const Vector2& newResolution) const {
  Camera result = *this;
  result.principal.array() *= newResolution.array() / result.resolution.array();
  result.focal.array() *= newResolution.array() / result.resolution.array();
  result.resolution = newResolution;
  return result;
}

void Camera::normalize() {
  principal = principal.cwiseQuotient(resolution);
  focal = focal.cwiseQuotient(resolution);
  resolution = Vector2::Ones();
}

bool Camera::isNormalized() const {
  return resolution == Vector2::Ones();
}

// WARNING: modifies input cameras
void Camera::normalizeRig(Camera::Rig& rig) {
  for (Camera& cam : rig) {
    if (!cam.isNormalized()) {
      cam.normalize();
    }
  }
}

Camera::Rig Camera::loadRig(const filesystem::path& filename) {
  std::string json;
  folly::readFile(filename.string().c_str(), json);
  CHECK(!json.empty()) << "could not read JSON file: " << filename;
  return loadRigFromJsonString(json);
}

Camera::Rig Camera::loadRigFromJsonString(const std::string& json) {
  folly::dynamic dynamic = folly::parseJson(json);
  std::vector<Camera> cameras;
  for (const auto& camera : dynamic["cameras"]) {
    cameras.emplace_back(camera);
  }
  return cameras;
}

void Camera::perturbCameras(
    std::vector<Camera>& cameras,
    const double posAmount,
    const double rotAmount,
    const double principalAmount,
    const double focalAmount) {
  for (auto& camera : cameras) {
    if (&camera != &cameras[0]) {
      perturb(camera.position, posAmount);
      auto rotation = camera.getRotation();
      perturb(rotation, rotAmount);
      camera.setRotation(rotation);
    }
    perturb(camera.principal, principalAmount);
    if (focalAmount != 0) {
      Camera::Real scalarFocal = camera.getScalarFocal();
      perturbScalar(scalarFocal, focalAmount);
      camera.setScalarFocal(scalarFocal);
    }
  }
}

const Camera& Camera::findCameraById(const std::string id, const Camera::Rig& rig) {
  for (const Camera& camera : rig) {
    if (camera.id == id) {
      return camera;
    }
  }
  LOG(FATAL) << folly::sformat("Camera id {} not found", id);
}

#ifndef SUPPRESS_RIG_IO

void Camera::saveRig(
    const std::string& filename,
    const std::vector<Camera>& cameras,
    const std::vector<std::string>& comments,
    const int doubleNumDigits) {
  folly::dynamic dynamic = folly::dynamic::object("cameras", folly::dynamic::array());
  for (const auto& camera : cameras) {
    dynamic["cameras"].push_back(camera.serialize());
  }
  if (!comments.empty()) {
    dynamic["comments"] = folly::dynamic::array(comments.begin(), comments.end());
  }
  folly::json::serialization_opts opts;
  opts.sort_keys = true;
  opts.pretty_formatting = true;
  if (doubleNumDigits > 0) {
    opts.double_mode = double_conversion::DoubleToStringConverter::FIXED;
    opts.double_num_digits = doubleNumDigits;
  }
  folly::writeFile(folly::json::serialize(dynamic, opts), filename.c_str());
}

#endif // SUPPRESS_RIG_IO

} // namespace fb360_dep
