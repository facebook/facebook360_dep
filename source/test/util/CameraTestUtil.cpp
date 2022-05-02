/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/util/Camera.h"

namespace fb360_dep {
bool testUndoPixel(
    Camera camera,
    Camera::Vector3 targetPoint,
    Camera::Real depth,
    Camera::Vector3 expected) {
  // check that rig undoes pixel
  Camera::Vector3 actual = camera.rig(camera.pixel(targetPoint)).pointAt(depth);
  return expected.isApprox(actual, 1e-10);
}
} // namespace fb360_dep
