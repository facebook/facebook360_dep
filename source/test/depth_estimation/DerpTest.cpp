/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "source/test/TestRig.h"
#include "source/util/ImageUtil.h"

namespace fb360_dep {

struct DerpTest : ::testing::Test {};

TEST_F(DerpTest, TestUtilFilterDestinations) {
  Camera::Rig testRig = Camera::loadRigFromJsonString(testRigJson);
  EXPECT_EQ(testRig.size(), 16);
  Camera::Rig filtered = image_util::filterDestinations(testRig, "cam4,cam15,cam0");
  EXPECT_EQ(filtered.size(), 3);
  EXPECT_TRUE(filtered[0].id == testRig[4].id);
  EXPECT_TRUE(filtered[1].id == testRig[15].id);
  EXPECT_TRUE(filtered[2].id == testRig[0].id);
}

} // namespace fb360_dep
