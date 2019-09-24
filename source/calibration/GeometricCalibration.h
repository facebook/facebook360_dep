/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <ceres/ceres.h>

#include "source/util/Camera.h"

namespace fb360_dep {
namespace calibration {

using ReprojectionErrorOutlier = std::pair<double, double>; // <original_error, weighted_error>

Camera makeCamera(
    const Camera& camera,
    const Camera::Vector3& position,
    const Camera::Vector3& rotation,
    const Camera::Vector2& principal,
    const Camera::Real& focal,
    const Camera::Distortion& distortion) {
  Camera result = camera;
  result.position = position;
  result.setRotation(rotation);
  result.principal = principal;
  result.setScalarFocal(focal);
  result.setDistortion(distortion);

  return result;
}

void cartesianToSpherical(
    Camera::Real& radius,
    Camera::Real& theta,
    Camera::Real& phi,
    const Camera::Vector3& cartesianCoords) {
  radius = cartesianCoords.norm();
  theta = acos(cartesianCoords.z() / radius);
  phi = atan(cartesianCoords.y() / cartesianCoords.x());
}

Camera::Vector3
sphericalToCartesian(const Camera::Real radius, const Camera::Real theta, const Camera::Real phi) {
  Camera::Vector3 cartesianCoords;
  cartesianCoords.x() = radius * sin(theta) * cos(phi);
  cartesianCoords.y() = radius * sin(theta) * sin(phi);
  cartesianCoords.z() = radius * cos(theta);
  return cartesianCoords;
}

struct SphericalReprojectionFunctor {
  static ceres::CostFunction* addResidual(
      ceres::Problem& problem,
      Camera::Real& theta,
      Camera::Real& phi,
      Camera::Vector3& rotation,
      Camera::Vector2& principal,
      Camera::Real& focal,
      Camera::Distortion& distortion,
      Camera::Vector3& world,
      Camera::Real radius,
      Camera::Vector3& referencePosition,
      const Camera& camera,
      const Camera::Vector2& pixel,
      bool robust = false,
      const int weight = 1) {
    auto* cost = new CostFunction(
        new SphericalReprojectionFunctor(camera, pixel, weight, radius, referencePosition));
    auto* loss = robust ? new ceres::HuberLoss(1.0) : nullptr;
    problem.AddResidualBlock(
        cost,
        loss,
        &theta,
        &phi,
        rotation.data(),
        principal.data(),
        &focal,
        distortion.data(),
        world.data());
    return cost;
  }

  bool operator()(
      double const* const theta,
      double const* const phi,
      double const* const rotation,
      double const* const principal,
      double const* const focal,
      double const* const distortion,
      double const* const world,
      double* residuals) const {
    // create a camera using parameters
    Camera::Vector3 position = sphericalToCartesian(radius, theta[0], phi[0]);
    position += referencePosition;
    Camera modified = makeCamera(
        camera,
        position,
        Eigen::Map<const Camera::Vector3>(rotation),
        Eigen::Map<const Camera::Vector2>(principal),
        *focal,
        Eigen::Map<const Camera::Distortion>(distortion));
    // transform world with that camera and compare to pixel
    Eigen::Map<const Camera::Vector3> w(world);
    Eigen::Map<Camera::Vector2> r(residuals);
    r = modified.pixel(w) - pixel;
    r = r / sqrt(weight);
    return true;
  }

 private:
  using CostFunction = ceres::NumericDiffCostFunction<
      SphericalReprojectionFunctor,
      ceres::CENTRAL,
      2, // residuals
      1, // theta
      1, // phi
      3, // rotation
      2, // principal
      1, // focal
      Camera::Distortion::SizeAtCompileTime, // distortion
      3>; // world

  SphericalReprojectionFunctor(
      const Camera& camera,
      const Camera::Vector2& pixel,
      const int weight,
      const double radius,
      const Camera::Vector3& referencePosition)
      : camera(camera),
        pixel(pixel),
        weight(weight),
        radius(radius),
        referencePosition(referencePosition) {}

  const Camera& camera;
  const Camera::Vector2 pixel;
  const int weight;
  const Camera::Real radius;
  const Camera::Vector3& referencePosition;
};

struct ReprojectionFunctor {
  static ceres::CostFunction* addResidual(
      ceres::Problem& problem,
      Camera::Vector3& position,
      Camera::Vector3& rotation,
      Camera::Vector2& principal,
      Camera::Real& focal,
      Camera::Distortion& distortion,
      Camera::Vector3& world,
      const Camera& camera,
      const Camera::Vector2& pixel,
      bool robust = false,
      const int weight = 1) {
    auto* cost = new CostFunction(new ReprojectionFunctor(camera, pixel, weight));
    auto* loss = robust ? new ceres::HuberLoss(1.0) : nullptr;
    problem.AddResidualBlock(
        cost,
        loss,
        position.data(),
        rotation.data(),
        principal.data(),
        &focal,
        distortion.data(),
        world.data());
    return cost;
  }

  bool operator()(
      double const* const position,
      double const* const rotation,
      double const* const principal,
      double const* const focal,
      double const* const distortion,
      double const* const world,
      double* residuals) const {
    // create a camera using parameters
    Camera modified = makeCamera(
        camera,
        Eigen::Map<const Camera::Vector3>(position),
        Eigen::Map<const Camera::Vector3>(rotation),
        Eigen::Map<const Camera::Vector2>(principal),
        *focal,
        Eigen::Map<const Camera::Distortion>(distortion));
    // transform world with that camera and compare to pixel
    Eigen::Map<const Camera::Vector3> w(world);
    Eigen::Map<Camera::Vector2> r(residuals);
    r = modified.pixel(w) - pixel;
    r = r / sqrt(weight);

    return true;
  }

 private:
  using CostFunction = ceres::NumericDiffCostFunction<
      ReprojectionFunctor,
      ceres::CENTRAL,
      2, // residuals
      3, // position
      3, // rotation
      2, // principal
      1, // focal
      Camera::Distortion::SizeAtCompileTime, // distortion
      3>; // world

  ReprojectionFunctor(const Camera& camera, const Camera::Vector2& pixel, const int weight)
      : camera(camera), pixel(pixel), weight(weight) {}

  const Camera& camera;
  const Camera::Vector2 pixel;
  const int weight;
};

// The problem with using a world coordinate as the variable when triangulating is that the solver
// might overshoot and end up behind you. This happens a lot if the initial estimate is e.g. 1000 m
// away: The solver realizes it is way too far and follows the gradient back close to zero. Very
// often it will blow past zero and end up behind you

// The trick to fix this is to use inv = world / |world|^2 as the variable instead. Using
//   disparity = 1 / |world|,
// it can be seen that
//   inv = disparty * unit(world),
// so this accomplishes two things:
// - The variable is proportional to disparity. We care about pixels, not meters
// - To end up behind you, the solver needs to cross through infinity (hard) instead of zero (easy)
struct TriangulationFunctor {
  static ceres::CostFunction* addResidual(
      ceres::Problem& problem,
      Camera::Vector3& inv,
      const Camera& camera,
      const Camera::Vector2& pixel,
      const bool robust = false) {
    auto* cost = new CostFunction(new TriangulationFunctor(camera, pixel));
    auto* loss = robust ? new ceres::HuberLoss(1.0) : nullptr;
    problem.AddResidualBlock(cost, loss, inv.data());
    return cost;
  }

  bool operator()(double const* const inv, double* residuals) const {
    Eigen::Map<const Camera::Vector3> i(inv);
    Eigen::Map<Camera::Vector2> r(residuals);

    Camera::Vector3 w = i / i.squaredNorm();

    // transform world with camera and compare to pixel
    r = camera.pixel(w) - pixel;

    return true;
  }

 private:
  using CostFunction = ceres::NumericDiffCostFunction<
      TriangulationFunctor,
      ceres::CENTRAL,
      2, // residuals
      3>; // inverse world

  TriangulationFunctor(const Camera& camera, const Camera::Vector2& pixel)
      : camera(camera), pixel(pixel) {}

  const Camera& camera;
  const Camera::Vector2 pixel;
};

using Observations = std::vector<std::pair<const Camera&, Camera::Vector2>>;

Camera::Vector3 averageAtDistance(const Observations& observations, const Camera::Real distance) {
  Camera::Vector3 sum(0, 0, 0);
  for (const auto& obs : observations) {
    sum += obs.first.rig(obs.second, distance);
  }
  return sum / observations.size();
}

Camera::Vector3 triangulateNonlinear(const Observations& observations, const bool forceInFront) {
  CHECK_GE(observations.size(), 2);
  ceres::Solver::Options options;
  options.max_num_iterations = 10;

  // initial value is average of distant points
  const Camera::Real kInitialDistance = 10; // 10 meters, not hugely important
  Camera::Vector3 world = averageAtDistance(observations, kInitialDistance);
  Camera::Vector3 inv = world / world.squaredNorm();

  ceres::Problem problem;
  for (const auto& obs : observations) {
    TriangulationFunctor::addResidual(problem, inv, obs.first, obs.second);
  }

  ceres::Solver::Summary summary;
  ceres::Solve(options, &problem, &summary);

  world = inv / inv.squaredNorm();

  if (forceInFront) {
    for (const auto& obs : observations) {
      if (obs.first.isBehind(world)) {
        return averageAtDistance(observations, Camera::kNearInfinity);
      }
    }
  }

  return world;
}

double calcPercentile(std::vector<double> values, double percentile = 0.5) {
  if (values.empty()) {
    return NAN;
  }
  CHECK_LT(percentile, 1);
  size_t index(percentile * values.size());
  std::nth_element(values.begin(), values.begin() + index, values.end());
  return values[index];
}

Camera::Vector2 reprojectionError(const ceres::Problem& problem, ceres::ResidualBlockId id) {
  auto cost = problem.GetCostFunctionForResidualBlock(id);
  std::vector<double*> parameterBlocks;
  problem.GetParameterBlocksForResidualBlock(id, &parameterBlocks);
  Camera::Vector2 residual;
  cost->Evaluate(parameterBlocks.data(), residual.data(), nullptr);
  return residual;
}

bool reprojectionErrorOutlier(
    const ceres::Problem& problem,
    ceres::ResidualBlockId id,
    double& originalError,
    double& weightedError) {
  auto loss = problem.GetLossFunctionForResidualBlock(id);
  originalError = reprojectionError(problem, id).norm();
  double lossOutput[3];
  loss->Evaluate(originalError, lossOutput);
  weightedError = lossOutput[0];
  return lossOutput[1] < 1 || lossOutput[2] < 0;
}

std::vector<double> getReprojectionErrorNorms(
    const ceres::Problem& problem,
    const double* parameter = nullptr,
    bool weighted = false) {
  std::vector<double> result;
  std::vector<ceres::ResidualBlockId> ids;
  if (parameter) {
    problem.GetResidualBlocksForParameterBlock(parameter, &ids);
  } else {
    problem.GetResidualBlocks(&ids);
  }
  for (auto& id : ids) {
    double errorNorm = reprojectionError(problem, id).norm();
    if (weighted) {
      double weightedErrorNorm;
      reprojectionErrorOutlier(problem, id, errorNorm, weightedErrorNorm);
      result.push_back(weightedErrorNorm);
    } else {
      result.push_back(errorNorm);
    }
  }
  return result;
}

std::vector<ReprojectionErrorOutlier> getReprojectionErrorOutliers(
    const ceres::Problem& problem,
    const double* parameter = nullptr) {
  std::vector<ReprojectionErrorOutlier> result;
  std::vector<ceres::ResidualBlockId> ids;
  if (parameter) {
    problem.GetResidualBlocksForParameterBlock(parameter, &ids);
  } else {
    problem.GetResidualBlocks(&ids);
  }
  for (auto& id : ids) {
    double originalError;
    double weightedError;
    if (reprojectionErrorOutlier(problem, id, originalError, weightedError)) {
      result.push_back(std::make_pair(originalError, weightedError));
    }
  }
  return result;
}

} // namespace calibration
} // namespace fb360_dep
