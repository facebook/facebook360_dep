/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "source/util/CvUtil.h"

#ifdef WIN32
#include "source/calibration/Calibration.cpp"
#endif

int main(int argc, char** argv) {
  ::cv::setNumThreads(1);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
