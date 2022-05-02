/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <math.h>

#include <gtest/gtest.h>

#include "source/test/util/CameraTestUtil.h"
#include "source/util/Camera.h"

using namespace fb360_dep;

static const char* testOrthographicJson = R"({
  "version" : 1,
  "type" : "ORTHOGRAPHIC",
  "origin" : [0, 0, 0],
  "forward" : [-1, 0, 0],
  "up" : [0, 0, 1],
  "right" : [0, 1, 0],
  "resolution" : [2048, 2048],
  "focal" : [1240, -1240],
  "id" : "cam3"
})";

// Test fixture for the camera. Read about google tests here:
//   https://github.com/google/googletest/blob/master/googletest/docs/primer.md

struct OrthographicTest : ::testing::Test {
  const Camera orthographic = Camera(folly::parseJson(testOrthographicJson));
};

TEST_F(OrthographicTest, TestInitialization) {
  // Orthographic
  EXPECT_EQ(orthographic.id, "cam3");
  EXPECT_EQ(orthographic.position, Camera::Vector3(0, 0, 0));
  Camera::Vector3 right(0, 1, 0);
  EXPECT_TRUE(orthographic.right().isApprox(right, 1e-3)) << orthographic.right();
  auto center = orthographic.pixel(orthographic.position + orthographic.forward());
  EXPECT_NEAR(2048 / 2, center.x(), 1e-10);
  EXPECT_NEAR(2048 / 2, center.y(), 1e-10);
  EXPECT_NEAR(orthographic.cosFov, 0, 1e-10);
}

TEST_F(OrthographicTest, TestFOV) {
  Camera camera = orthographic;
  EXPECT_TRUE(camera.isDefaultFov());
  EXPECT_FALSE(camera.isOutsideSensor(camera.pixel(camera.rigNearInfinity({0, 0}))));
  EXPECT_TRUE(camera.sees(camera.rigNearInfinity({1, 1})));
  camera.setFov(0.5 * M_PI);
  EXPECT_NEAR(camera.getFov(), 0.5 * M_PI, 1e-10);
  camera.setFov(0.1 * M_PI);
  EXPECT_NEAR(camera.getFov(), 0.1 * M_PI, 1e-10);
  EXPECT_FALSE(camera.sees(camera.rigNearInfinity({1, 1})));
  EXPECT_TRUE(camera.isOutsideImageCircle({1, 1}));
  EXPECT_TRUE(camera.sees(camera.rigNearInfinity({1200, 1000})));
  EXPECT_FALSE(camera.isOutsideImageCircle({1200, 1000}));
  camera.setDefaultFov();
  EXPECT_TRUE(camera.sees(camera.rigNearInfinity({1, 1})));
  EXPECT_FALSE(camera.isOutsideImageCircle({1, 1}));
  EXPECT_TRUE(camera.isBehind(Camera::Vector3(1, 1, 0)));
  EXPECT_FALSE(camera.isBehind(Camera::Vector3(-1, 1, 0)));
}

TEST_F(OrthographicTest, TestUndoPixel) {
  auto d = 1234.5;
  Camera::Vector3 withinFov = orthographic.position + d * Camera::Vector3(-2, 3, -1).normalized();
  EXPECT_TRUE(testUndoPixel(orthographic, withinFov, d, withinFov));

  Camera::Vector3 edgeOfFov = orthographic.position + d * Camera::Vector3(0, 1, 0).normalized();
  EXPECT_TRUE(testUndoPixel(orthographic, edgeOfFov, d, edgeOfFov));

  Camera::Vector3 outsideFov = orthographic.position + d * Camera::Vector3(1, 1, 0).normalized();
  EXPECT_TRUE(testUndoPixel(orthographic, outsideFov, d, edgeOfFov));
}

TEST_F(OrthographicTest, TestGetSetRotation) {
  // check that rotation survives getting/setting parameters
  auto d = 0.1;
  Camera::Vector3 expected = orthographic.position + d * Camera::Vector3(-2, 3, -1).normalized();
  Camera modified = orthographic;
  modified.setRotation(orthographic.getRotation());
  auto modifiedActual = modified.rig(modified.pixel(expected)).pointAt(d);
  EXPECT_TRUE(expected.isApprox(modifiedActual, 1e-10)) << expected << "\n\n" << modifiedActual;
  EXPECT_TRUE(modified.getRotation().isApprox(orthographic.getRotation()))
      << modified.getRotation() << "\n\n"
      << orthographic.getRotation();
}

TEST_F(OrthographicTest, TestDistortUndistortNOP) {
  // check that undistort undoes no-op distort
  Camera camera = orthographic;
  camera.setDefaultDistortion();
  Camera::Real expected = 3;
  Camera::Real distorted = camera.distort(expected);
  Camera::Real undistorted = camera.undistort(distorted);
  EXPECT_NEAR(expected, undistorted, 1.0 / Camera::kNearInfinity);
}

TEST_F(OrthographicTest, TestDistortUndistort) {
  // check that undistort undoes distort
  Camera camera = orthographic;
  Camera::Distortion distortion = camera.getDistortion();

  // normal case
  distortion[0] = 0.20;
  distortion[1] = 0.02;
  camera.setDistortion(distortion);
  Camera::Real expected = 2;
  Camera::Real distorted = camera.distort(expected);
  Camera::Real undistorted = camera.undistort(distorted);
  EXPECT_NEAR(undistorted, expected, 1.0 / Camera::kNearInfinity);

  // negative (real) roots
  distortion[0] = 2 / 3.0;
  distortion[1] = 1 / 5.0;
  camera.setDistortion(distortion);
  EXPECT_TRUE(isinf(camera.getDistortionMax()));

  // imaginary roots
  distortion[0] = 1;
  distortion[1] = 1;
  camera.setDistortion(distortion);
  EXPECT_TRUE(isinf(camera.getDistortionMax()));
}

TEST_F(OrthographicTest, TestUndistortMonotonic) {
  // check that undistort is monotonic
  Camera camera = orthographic;
  Camera::Distortion distortion = camera.getDistortion();
  distortion[0] = 0.04012303891;
  distortion[1] = 0.08782249937;
  camera.setDistortion(distortion);
  Camera::Real prev = 0;
  for (Camera::Real y = 0; y < 3; y += 0.1) {
    Camera::Real x = camera.undistort(y);
    EXPECT_LE(prev, x + 1.0 / Camera::kNearInfinity) << y;
    prev = x;
  }
}

TEST_F(OrthographicTest, TestNormalize) {
  // check that normalize() correctly normalizes the resolution and rescales the camera accordingly
  Camera camera = orthographic;
  auto expectedPrincipal = camera.principal.cwiseQuotient(camera.resolution);
  auto expectedFocal = camera.focal.cwiseQuotient(camera.resolution);
  EXPECT_FALSE(camera.isNormalized());
  camera.normalize();
  EXPECT_TRUE(expectedPrincipal.isApprox(camera.principal, 1e-10));
  EXPECT_TRUE(expectedFocal.isApprox(camera.focal, 1e-10));
  EXPECT_TRUE(camera.isNormalized());
}

TEST_F(OrthographicTest, TestRescale) {
  // check that rescale() correctly rescales the principal, focal, and resolution
  Camera camera = orthographic;
  auto scaleFactor = 9999.9;
  Camera::Vector2 newResolution = camera.resolution * scaleFactor;
  Camera expected = camera.rescale(newResolution);
  EXPECT_TRUE(expected.principal.isApprox(camera.principal * scaleFactor, 1e-10));
  EXPECT_TRUE(expected.focal.isApprox(camera.focal * scaleFactor, 1e-10));
  EXPECT_TRUE(expected.resolution.isApprox(camera.resolution * scaleFactor, 1e-10));
}
