/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/format.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/Format.h>

#include "source/rig/RigAligner.h"
#include "source/util/Camera.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;

const std::string kUsageMessage = R"(
  - Performs a camera-to-camera compare between an input rig and a reference rig.

  - Example:
    ./RigCompare \
    --rig=/path/to/rigs/rig.json \
    --reference=/path/to/rigs/reference.json
)";

DEFINE_string(reference, "", "path to reference rig .json file (required)");
DEFINE_string(rig, "", "path to rig .json file (required)");
DEFINE_bool(skip_align, false, "skip rig alignment before comparing");

void compareRigs(const Camera::Rig& rig, const Camera::Rig& reference) {
  double averagePositionDiff = 0;
  double averageForwardDiff = 0;
  double averageUpDiff = 0;
  double averagePrincipalDiff = 0;
  double averageFocalDiff = 0;
  for (const Camera& camera : rig) {
    const Camera ref = Camera::findCameraById(camera.id, reference);
    LOG(INFO) << fmt::format("{}:", camera.id);

    double currentPositionDiff = (camera.position - ref.position).norm();
    double currentForwardDiff = acos(camera.forward().dot(ref.forward()));
    double currentUpDiff = acos(camera.up().dot(ref.up()));
    double currentPrincipalDiff = (camera.principal - ref.principal).norm();
    double currentFocalDiff = camera.getScalarFocal() - ref.getScalarFocal();

    averagePositionDiff += currentPositionDiff;
    averageForwardDiff += currentForwardDiff;
    averageUpDiff += currentUpDiff;
    averagePrincipalDiff += currentPrincipalDiff;
    averageFocalDiff += currentFocalDiff;

    LOG(INFO) << fmt::format("- position diff: {}", currentPositionDiff);
    LOG(INFO) << fmt::format("- forward diff (radians): {}", currentForwardDiff);
    LOG(INFO) << fmt::format("- up diff (radians): {}", currentUpDiff);
    LOG(INFO) << fmt::format("- principal diff: {}", currentPrincipalDiff);
    LOG(INFO) << fmt::format("- focal diff: {}", currentFocalDiff);
  }

  LOG(INFO) << "Average:";
  LOG(INFO) << fmt::format("- position diff: {}", averagePositionDiff / rig.size());
  LOG(INFO) << fmt::format("- forward diff (radians): {}", averageForwardDiff / rig.size());

  LOG(INFO) << fmt::format("- up diff (radians): {}", averageUpDiff / rig.size());

  LOG(INFO) << fmt::format("- principal diff: {}", averagePrincipalDiff / rig.size());
  LOG(INFO) << fmt::format("- focal diff: {}", averageFocalDiff / rig.size());
}

int main(int argc, char* argv[]) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_rig, "");
  CHECK_NE(FLAGS_reference, "");

  // read the rigs
  Camera::Rig rig = Camera::loadRig(FLAGS_rig);
  Camera::Rig reference = Camera::loadRig(FLAGS_reference);

  if (!FLAGS_skip_align) {
    rig = alignRig(rig, reference);
  }

  compareRigs(rig, reference);

  return EXIT_SUCCESS;
}
