/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/rig/RigAligner.h"

#include <random>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/Format.h>

#include "source/util/SystemUtil.h"

using namespace fb360_dep;

const std::string kUsageMessage = R"(
   - Aligns the scale, position, and orientation of the input rig to a reference rig via rescaling,
   translating, and rotating respectively. These can be selectively locked.

   - Example:
     ./RigAligner \
     --rig_in=/path/to/rigs/rig.json \
     --rig_reference=/path/to/rigs/reference.json \
     --rig_out=/path/to/rigs/aligned.json
 )";

DEFINE_bool(lock_rotation, false, "don't rotate the rig");
DEFINE_bool(lock_scale, false, "don't scale the rig");
DEFINE_bool(lock_translation, false, "don't translate the rig");
DEFINE_bool(
    randomize_rig,
    false,
    "create a test rig by applying a random rotation, translation and scale to the original rig");
DEFINE_string(rig_in, "", "path to rig .json file (required)");
DEFINE_string(rig_out, "", "path to output rig .json file (required)");
DEFINE_string(rig_reference, "", "path to the reference rig .json file (required)");
DEFINE_double(rng_seed, 1, "random number generator seed");
DEFINE_string(transformed_rig, "", "path to transformed test rig .json file");

Camera::Rig randomizeRig(Camera::Rig rig, int seed) {
  std::mt19937 gen(seed);
  std::uniform_real_distribution<> r(0, M_PI);
  std::uniform_int_distribution<> t(-100, 100);
  std::uniform_real_distribution<> s(0.5, 2);

  const Camera::Vector3 randomRotation(r(gen), r(gen), r(gen));
  const Camera::Vector3 randomTranslation(t(gen), t(gen), t(gen));
  const Eigen::UniformScaling<double> randomScale(s(gen));

  const bool applyInReverse = true;
  rig = transformRig(rig, randomRotation, randomTranslation, randomScale, applyInReverse);
  if (!FLAGS_transformed_rig.empty()) {
    LOG(INFO) << folly::sformat("Saving randomized rig to {}", FLAGS_transformed_rig);
    Camera::saveRig(FLAGS_transformed_rig, rig);
  }
  LOG(INFO) << folly::sformat(
      "Random rotation values: {} {} {}", randomRotation[0], randomRotation[1], randomRotation[2]);
  LOG(INFO) << folly::sformat(
      "Random translation values: {} {} {}",
      randomTranslation[0],
      randomTranslation[1],
      randomTranslation[2]);
  LOG(INFO) << folly::sformat("Random scale values: {}", randomScale.factor());
  return rig;
}

int main(int argc, char* argv[]) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_rig_in, "");
  CHECK_NE(FLAGS_rig_reference, "");
  CHECK_NE(FLAGS_rig_out, "");

  // Read in the rig and reference rig
  LOG(INFO) << "Loading the cameras";
  Camera::Rig rig = Camera::loadRig(FLAGS_rig_in);
  const Camera::Rig referenceRig = Camera::loadRig(FLAGS_rig_reference);

  if (FLAGS_randomize_rig) {
    // Randomly transform the original rig
    LOG(INFO) << "Randomizing rig";
    rig = randomizeRig(rig, FLAGS_rng_seed);
  }
  const Camera::Rig transformedRig =
      alignRig(rig, referenceRig, FLAGS_lock_rotation, FLAGS_lock_translation, FLAGS_lock_scale);
  Camera::saveRig(FLAGS_rig_out, transformedRig);

  return 0;
}
