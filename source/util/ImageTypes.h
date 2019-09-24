/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

namespace fb360_dep {
// background_color, background_disp, color, and foreground_masks are w.r.t. input_root
// The rest are w.r.t. output_root
#define IMAGE_TYPES(X)                                                \
  X(background_color, "background/color")                             \
  X(background_color_levels, "background/color_levels")               \
  X(background_disp, "background/disparity")                          \
  X(background_disp_levels, "background/disparity_levels")            \
  X(background_disp_upsample, "background/disparity_upsample")        \
  X(bin, "bin")                                                       \
  X(color, "video/color")                                             \
  X(color_levels, "video/color_levels")                               \
  X(confidence, "confidence")                                         \
  X(cost, "cost")                                                     \
  X(disparity, "disparity")                                           \
  X(disparity_upsample, "disparity_upsample")                         \
  X(disparity_levels, "disparity_levels")                             \
  X(disparity_time_filtered, "disparity_time_filtered")               \
  X(disparity_time_filtered_levels, "disparity_time_filtered_levels") \
  X(exports, "exports")                                               \
  X(exports_cubecolor, "exports/cubecolor")                           \
  X(exports_cubedisp, "exports/cubedisp")                             \
  X(exports_eqrcolor, "exports/eqrcolor")                             \
  X(exports_eqrdisp, "exports/eqrdisp")                               \
  X(exports_lr180, "exports/lr180")                                   \
  X(exports_tb3dof, "exports/tb3dof")                                 \
  X(exports_tbstereo, "exports/tbstereo")                             \
  X(foreground_masks, "video/foreground_masks")                       \
  X(foreground_masks_levels, "video/foreground_masks_levels")         \
  X(fused, "fused")                                                   \
  X(mismatches, "mismatches")                                         \
  X(video_bin, "video/bin")                                           \
  X(video_disp, "video/disparity")                                    \
  X(video_disp_levels, "video/disparity_levels")                      \
  X(video_fused, "video/fused")

enum struct ImageType {
#define ENUM_VALUE(name, str) name,
  IMAGE_TYPES(ENUM_VALUE)
#undef ENUM_VALUE
};

const std::string imageTypes[]{
#define NAME(name, str) str,
    IMAGE_TYPES(NAME)
#undef NAME
};
} // namespace fb360_dep
