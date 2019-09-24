/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <math.h>

#include <gtest/gtest.h>

#include "source/test/util/CameraTestUtil.h"
#include "source/util/Camera.h"

using namespace fb360_dep;

static const char* testFthetaJson = R"({
  "version" : 1,
  "type" : "FTHETA",
  "origin" : [-10.51814, 13.00734, -4.22656],
  "forward" : [-0.6096207796429852, 0.7538922995778138, -0.24496715221587234],
  "up" : [0.7686134846014325, 0.6376793279268061, 0.050974366338976666],
  "right" : [0.19502945167097138, -0.15702371237098722, -0.9681462011153862],
  "resolution" : [2448, 2048],
  "focal" : [1240, -1240],
  "id" : "cam1"
})";


// Test fixture for the camera. Read about google tests here:
//   https://github.com/google/googletest/blob/master/googletest/docs/primer.md

struct FThetaTest : ::testing::Test {
  const Camera ftheta = Camera(folly::parseJson(testFthetaJson));
};

TEST_F(FThetaTest, TestInitialization) {
  EXPECT_EQ(ftheta.id, "cam1");
  EXPECT_EQ(ftheta.position, Camera::Vector3(-10.51814, 13.00734, -4.22656));
  // use isApprox() because camera orthogonalizes the rotation
  Camera::Vector3 right(0.19502945167097138, -0.15702371237098722, -0.9681462011153862);
  EXPECT_TRUE(ftheta.right().isApprox(right, 1e-3)) << ftheta.right();
  auto center = ftheta.pixel(ftheta.position + ftheta.forward());
  EXPECT_NEAR(2448 / 2, center.x(), 1e-10);
  EXPECT_NEAR(2048 / 2, center.y(), 1e-10);
}

TEST_F(FThetaTest, TestFOV) {
  Camera camera = ftheta;
  EXPECT_TRUE(camera.isDefaultFov());
  EXPECT_TRUE(camera.sees(camera.rigNearInfinity({1, 1})));
  camera.setFov(0.9 * M_PI);
  EXPECT_NEAR(camera.getFov(), 0.9 * M_PI, 1e-10);
  camera.setFov(0.1 * M_PI);
  EXPECT_NEAR(camera.getFov(), 0.1 * M_PI, 1e-10);
  EXPECT_FALSE(camera.sees(camera.rigNearInfinity({1, 1})));
  EXPECT_TRUE(camera.isOutsideImageCircle({1, 1}));
  EXPECT_TRUE(camera.sees(camera.rigNearInfinity({1200, 1000})));
  EXPECT_FALSE(camera.isOutsideImageCircle({1200, 1000}));
  camera.setDefaultFov();
  EXPECT_TRUE(camera.sees(camera.rigNearInfinity({1, 1})));
  EXPECT_FALSE(camera.isOutsideImageCircle({1, 1}));
}

TEST_F(FThetaTest, TestUndoPixel) {
  // check that rig undoes pixel
  auto d = 3.1;
  Camera::Vector3 withinFov = ftheta.position + d * Camera::Vector3(-2, 3, -1).normalized();
  EXPECT_TRUE(testUndoPixel(ftheta, withinFov, d, withinFov));

  Camera::Vector3 outsideFov = ftheta.position + d * Camera::Vector3(-2, 3, -1).normalized();
  EXPECT_TRUE(testUndoPixel(ftheta, outsideFov, d, outsideFov));
}

TEST_F(FThetaTest, TestGetSetRotation) {
  // check that rotation survives getting/setting parameters
  auto d = 3.1;
  Camera::Vector3 expected = ftheta.position + d * Camera::Vector3(-2, 3, -1).normalized();
  Camera modified = ftheta;
  modified.setRotation(ftheta.getRotation());
  auto modifiedActual = modified.rig(modified.pixel(expected)).pointAt(d);
  EXPECT_TRUE(expected.isApprox(modifiedActual, 1e-3)) << expected << "\n\n" << modifiedActual;
  EXPECT_TRUE(modified.getRotation().isApprox(ftheta.getRotation()))
      << modified.getRotation() << "\n\n"
      << ftheta.getRotation();
}

TEST_F(FThetaTest, TestDistortUndistortNOP) {
  // check that undistort undoes no-op distort
  Camera camera = ftheta;
  camera.setDefaultDistortion();
  Camera::Real expected = 3;
  Camera::Real distorted = camera.distort(expected);
  Camera::Real undistorted = camera.undistort(distorted);
  EXPECT_NEAR(expected, undistorted, 1.0 / Camera::kNearInfinity);
}

TEST_F(FThetaTest, TestDistortUndistort) {
  // check that undistort undoes distort
  Camera camera = ftheta;
  Camera::Distortion distortion = camera.getDistortion();
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

TEST_F(FThetaTest, TestUndistortMonotonic) {
  // check that undistort is monotonic
  Camera camera = ftheta;
  Camera::Distortion distortion = camera.getDistortion();
  distortion[0] = -0.03658484692522479;
  distortion[1] = -0.004515457470690702;
  camera.setDistortion(distortion);
  Camera::Real prev = 0;
  for (Camera::Real y = 0; y < 3; y += 0.1) {
    Camera::Real x = camera.undistort(y);
    EXPECT_LE(prev, x + 1.0 / Camera::kNearInfinity) << y;
    prev = x;
  }
}

TEST_F(FThetaTest, TestNormalize) {
  // check that normalize() correctly normalizes the resolution and rescales the camera accordingly
  Camera camera = ftheta;
  auto expectedPrincipal = camera.principal.cwiseQuotient(camera.resolution);
  auto expectedFocal = camera.focal.cwiseQuotient(camera.resolution);
  EXPECT_FALSE(camera.isNormalized());
  camera.normalize();
  EXPECT_TRUE(expectedPrincipal.isApprox(camera.principal, 1e-10));
  EXPECT_TRUE(expectedFocal.isApprox(camera.focal, 1e-10));
  EXPECT_TRUE(camera.isNormalized());
}

TEST_F(FThetaTest, TestRescale) {
  // check that rescale() correctly rescales the principal, focal, and resolution
  Camera camera = ftheta;
  auto scaleFactor = 1.2;
  Camera::Vector2 newResolution = camera.resolution * scaleFactor;
  Camera expected = camera.rescale(newResolution);
  EXPECT_TRUE(expected.principal.isApprox(camera.principal * scaleFactor, 1e-10));
  EXPECT_TRUE(expected.focal.isApprox(camera.focal * scaleFactor, 1e-10));
  EXPECT_TRUE(expected.resolution.isApprox(camera.resolution * scaleFactor, 1e-10));
}
