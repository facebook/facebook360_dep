/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include <glog/logging.h>
#include <Eigen/Geometry>

#include <folly/FileUtil.h>
#include <folly/dynamic.h>
#include <folly/json.h>

#include "source/util/FilesystemUtil.h"

#ifdef WIN32

#ifndef M_PI
#define M_PI EIGEN_PI
#endif

// windows doesn't need the full rig i/o functionality
// yes it does -- TKS  #define SUPPRESS_RIG_IO

#endif // WIN32

namespace fb360_dep {

struct Camera {
  using Real = double;
  using Vector2 = Eigen::Matrix<Real, 2, 1>;
  using Vector3 = Eigen::Matrix<Real, 3, 1>;
  using Matrix3 = Eigen::Matrix<Real, 3, 3>;
  using Ray = Eigen::ParametrizedLine<Real, 3>;
  using Distortion = Eigen::Matrix<Real, 3, 1>;
  using Rig = std::vector<Camera>;
  static const Camera::Real kNearInfinity;

  // member variables
  enum struct Type { FTHETA, RECTILINEAR, EQUISOLID, ORTHOGRAPHIC } type;

  Vector3 position;
  Matrix3 rotation;

  Vector2 resolution;

  Vector2 principal;

 private:
  Distortion distortion_;
  Real distortionMax_;

 public:
  Vector2 focal;
  Real cosFov;

  std::string id;
  std::string group;

  // construction and de/serialization
  Camera(const Type type, const Vector2& resolution, const Vector2& focal);
  explicit Camera(const folly::dynamic& json);
  folly::dynamic serialize() const;
  static Rig loadRig(const filesystem::path& filename);
  static Rig loadRigFromJsonString(const std::string& json);
  static void saveRig(
      const std::string& filename,
      const Rig& rig,
      const std::vector<std::string>& comments = {},
      const int doubleNumDigits = -1);

  // access rotation as forward/up/right vectors
  Vector3 forward() const {
    return -backward();
  }
  Vector3 up() const {
    return rotation.row(1);
  }
  Vector3 right() const {
    return rotation.row(0);
  }
  void setRotation(const Vector3& forward, const Vector3& up, const Vector3& right);
  void setRotation(const Vector3& forward, const Vector3& up);

  // access rotation as angle * axis
  Vector3 getRotation() const;
  void setRotation(const Vector3& angleAxis);

  // set distortion
  void setDefaultDistortion();
  void setDistortion(const Distortion& distortion);
  const Distortion& getDistortion() const {
    return distortion_;
  }

  // access distortionMax
  Real getDistortionMax() const {
    return distortionMax_;
  }

  // access focal as a scalar (x right, y down, square pixels)
  void setScalarFocal(const Real& scalar);
  Real getScalarFocal() const;

  // access fov (measured in radians from optical axis)
  void setFov(const Real& radians);
  Real getFov() const;
  void setDefaultFov();
  static Real getDefaultCosFov(Type type);
  bool isDefaultFov() const;

  Camera rescale(const Vector2& newResolution = {1, 1}) const;
  void normalize();
  bool isNormalized() const;
  static void normalizeRig(Camera::Rig& rig);

  // compute pixel coordinates
  Vector2 pixel(const Vector3& rig) const {
    // transform from rig to camera space
    Vector3 camera = rotation * (rig - position);
    // transform from camera to distorted sensor coordinates
    Vector2 sensor = cameraToSensor(camera);
    // transform from sensor coordinates to pixel coordinates
    return focal.cwiseProduct(sensor) + principal;
  }

  // compute rig coordinates, returns a ray, inverse of pixel()
  Ray rig(const Vector2& pixel) const {
    // transform from pixel to distorted sensor coordinates
    Vector2 sensor = (pixel - principal).cwiseQuotient(focal);
    // transform from distorted sensor coordinates to unit camera vector
    Vector3 unit = sensorToCamera(sensor);
    // transform from camera space to rig space
    return Ray(position, rotation.transpose() * unit);
  }

  // compute rig coordinates for point at a particular depth
  Vector3 rig(const Vector2& pixel, const Real depth) const {
    return rig(pixel).pointAt(depth);
  }

  // compute rig coordinates for point near infinity, inverse of pixel()
  Vector3 rigNearInfinity(const Vector2& pixel) const {
    return rig(pixel, kNearInfinity);
  }

  bool isBehind(const Vector3& rig) const {
    return backward().dot(rig - position) >= 0;
  }

  bool isOutsideFov(const Vector3& rig) const {
    if (cosFov == -1) {
      return false;
    }
    if (cosFov == 0) {
      return isBehind(rig);
    }
    Vector3 v = rig - position;
    Real dot = forward().dot(v);
    return dot * std::abs(dot) <= cosFov * std::abs(cosFov) * v.squaredNorm();
  }

  bool isOutsideImageCircle(const Vector2& pix) const {
    if (isDefaultFov()) {
      return false;
    }

    // find an edge point by projecting a point from the fov cone
    const Real sinFov = std::sqrt(1 - cosFov * cosFov);
    const Vector2 edge = cameraToSensor(Vector3(0, sinFov, -cosFov));

    // pix is outside fov if it is farther from principal than the edge point
    const Vector2 sensor = (pix - principal).cwiseQuotient(focal);
    return sensor.squaredNorm() >= edge.squaredNorm();
  }

  bool isOutsideSensor(const Vector2& pix) const {
    return !(0 <= pix.x() && pix.x() < resolution.x() && 0 <= pix.y() && pix.y() < resolution.y());
  }

  bool sees(const Vector3& rig, Vector2& pix) const {
    if (isOutsideFov(rig)) {
      return false;
    }
    pix = pixel(rig);
    return !isOutsideSensor(pix);
  }

  bool sees(const Vector3& rig) const {
    Vector2 ignored;
    return sees(rig, ignored);
  }

  // estimate the fraction of the frame that is covered by the other camera
  Real overlap(const Camera& other) const {
    // just brute force probeCount x probeCount points
    const int kProbeCount = 10;
    int inside = 0;
    for (int y = 0; y < kProbeCount; ++y) {
      for (int x = 0; x < kProbeCount; ++x) {
        const Vector2 p = Vector2(x, y).cwiseProduct(resolution) / (kProbeCount - 1);
        if (!isOutsideImageCircle(p) && other.sees(rigNearInfinity(p))) {
          ++inside;
        }
      }
    }
    return inside / Real(kProbeCount * kProbeCount);
  }

  static void perturbCameras(
      std::vector<Camera>& cameras,
      const double posAmount,
      const double rotAmount,
      const double principalAmount,
      const double focalAmount);

  static const Camera& findCameraById(const std::string id, const Camera::Rig& rig);

 private:
  static void perturbScalar(Camera::Real& r, Camera::Real amount) {
    r += amount * 2 * (std::rand() / double(RAND_MAX) - 0.5);
  }

  template <typename T>
  static void perturb(T& v, Camera::Real amount) {
    for (int i = 0; i < v.size(); ++i)
      perturbScalar(v[i], amount);
  }

  Vector3 backward() const {
    return rotation.row(2);
  }

  Real distortFactor(Real rSquared) const {
    Eigen::Index i = getDistortion().size();
    Real result = getDistortion()[--i];
    while (i > 0) {
      result = getDistortion()[--i] + rSquared * result;
    }
    return 1 + rSquared * result;
  }

 public:
  // distortion is modeled in pixel space as:
  //   distort(r) = r + d0 * r^3 + d1 * r^5
  Real distort(Real r) const {
    r = std::min(r, distortionMax_);
    return distortFactor(r * r) * r;
  }

  Real undistort(const Real y) const {
    if (getDistortion().isZero()) {
      return y; // short circuit common case
    }

    if (y >= distort(distortionMax_)) {
      return distortionMax_;
    }

    const Real smidgen = 1.0 / kNearInfinity;
    const int kMaxSteps = 10;

    // solve y = distort(x) for y using newton's method
    Real x0 = 0;
    Real y0 = 0;
    Real dy0 = 1;
    for (int step = 0; step < kMaxSteps; ++step) {
      const Real x1 = (y - y0) / dy0 + x0;
      const Real y1 = distort(x1);
      if (std::abs(y1 - y) < smidgen) {
        return x1; // close enough
      }
      Real dy1 = (distort(x1 + smidgen) - y1) / smidgen;
      CHECK_GE(dy1, 0) << "went past a maximum";
      x0 = x1;
      y0 = y1;
      dy0 = dy1;
    }
    return x0; // this should not happen
  }

  Vector3 pixelToCamera(const Vector2& pixel) const {
    // transform from pixel to distorted sensor coordinates
    Vector2 sensor = (pixel - principal).cwiseQuotient(focal);
    // transform from distorted sensor coordinates to unit camera vector
    return sensorToCamera(sensor);
  }

  Vector2 cameraToPixel(const Vector3& camera) const {
    // transform from unit camera vector to distorted sensor coordinates
    Vector2 sensor = cameraToSensor(camera);
    // transform from distorted sensor coordinates to pixel
    return sensor.cwiseProduct(focal) + principal;
  }

 private:
  Vector2 cameraToSensor(const Vector3& camera) const {
    // FTHETA: r = theta
    // RECTILINEAR: r = tan(theta)
    // EQUISOLID: r = 2 sin(theta / 2)
    // ORTHOGRAPHIC: r = sin(theta)
    // see https://wiki.panotools.org/Fisheye_Projection
    if (type == Type::FTHETA) {
      Real xy = camera.head<2>().norm();
      // r = theta <=>
      // r = atan2(|xy|, -z)
      Real r = atan2(xy, -camera.z());
      return distort(r) / xy * camera.head<2>();
    } else if (type == Type::RECTILINEAR) {
      // r = tan(theta) <=>
      // r = |xy| / -z <=>
      // pre-distortion result is xy / -z
      Real xy = camera.head<2>().norm();
      Real r;
      if (-camera.z() <= 0) { // outside fov
        r = tan(M_PI/2);
      } else {
        r = xy / -camera.z();
      }
      return distort(r) / xy * camera.head<2>();
    } else if (type == Type::EQUISOLID) {
      Real xy = camera.head<2>().norm();
      // r = 2 sin(theta / 2) <=>
      //   using sin(theta / 2) = sqrt((1 - cos(theta)) / 2)
      // r = 2 sqrt((1 + z / |xyz|) / 2)
      Real r = 2 * sqrt((1 + camera.z() / camera.norm()) / 2);
      return distort(r) / xy * camera.head<2>();
    } else {
      CHECK(type == Type::ORTHOGRAPHIC) << "unexpected: " << int(type);
      // r = sin(theta) <=>
      // r = |xy| / |xyz| <=>
      // pre-distortion result is xy / |xyz|
      Vector2 pre =
          camera.z() < 0 ? camera.head<2>() / camera.norm() : camera.head<2>().normalized();
      return distortFactor(pre.squaredNorm()) * pre;
    }
  }

  // compute unit vector in camera coors from normalized sensor coors
  Vector3 sensorToCamera(const Vector2& sensor) const {
    // FTHETA: r = theta
    // RECTILINEAR: r = tan(theta)
    // EQUISOLID: r = 2 sin(theta / 2)
    // ORTHOGRAPHIC: r = sin(theta)
    // see https://wiki.panotools.org/Fisheye_Projection
    Real squaredNorm = sensor.squaredNorm();
    if (squaredNorm == 0) {
      // avoid divide-by-zero later
      return Vector3(0, 0, -1);
    }
    Real norm = sqrt(squaredNorm);
    Real r = undistort(norm);
    Real theta;
    if (type == Type::FTHETA) {
      // r = theta
      theta = r;
    } else if (type == Type::RECTILINEAR) {
      // r = tan(theta)
      theta = atan(r);
    } else if (type == Type::EQUISOLID) {
      // r = 2 sin(theta / 2)
      // Note: arcsin function is undefined outside the interval [-1, 1]
      theta = r <= 2 ? 2 * asin(r / 2) : M_PI;
    } else {
      CHECK(type == Type::ORTHOGRAPHIC) << "unexpected: " << int(type);
      // r = sin(theta)
      theta = r <= 1 ? asin(r) : M_PI / 2;
    }
    Vector3 unit;
    unit.head<2>() = sin(theta) / norm * sensor;
    unit.z() = -cos(theta);

    return unit;
  }

  template <typename T>
  static folly::dynamic serializeVector(const T& v) {
    return folly::dynamic(v.data(), v.data() + v.size());
  }

  template <int kSize>
  static Eigen::Matrix<Real, kSize, 1> deserializeVector(const folly::dynamic& json) {
    CHECK_EQ(kSize, json.size()) << "bad vector" << json;
    Eigen::Matrix<Real, kSize, 1> result;
    for (int i = 0; i < kSize; ++i) {
      result[i] = json[i].asDouble();
    }
    return result;
  }

  static std::string serializeType(const Type& type) {
    switch (type) {
      case Type::FTHETA:
        return "FTHETA";
      case Type::RECTILINEAR:
        return "RECTILINEAR";
      case Type::EQUISOLID:
        return "EQUISOLID";
      case Type::ORTHOGRAPHIC:
        return "ORTHOGRAPHIC";
      default:
        CHECK(0) << "unexpected: " << int(type) << "; defaulting to ORTHOGRAPHIC";
        return "ORTHOGRAPHIC";
    }
  }

  static Type deserializeType(const folly::dynamic& json) {
    for (int i = 0;; ++i) {
      if (serializeType(Type(i)) == json.getString()) {
        return Type(i);
      }
    }
  }

}; // Camera
} // namespace fb360_dep
