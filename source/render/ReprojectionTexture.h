/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "source/gpu/GlUtil.h"
#include "source/render/ReprojectionTable.h"

namespace fb360_dep {

// a ReprojectionTexture holds the texture created from a ReprojectionTable
struct ReprojectionTexture {
  ReprojectionTexture(Camera dst, Camera src) {
    CHECK(!src.isNormalized()) << "can't compute tolerance";
    // accurate to 3% of a source pixel and covers 5% outside dst
    const Camera::Vector2 tol = 0.03 / src.resolution.array();
    const Camera::Vector2 margin(0.05, 0.05);
    dst.normalize();
    src.normalize();
    ReprojectionTable table(dst, src, tol, margin);
    texture = createTexture(table);
    scale = table.getScale();
    offset = table.getOffset();
  }

  ~ReprojectionTexture() {
    glDeleteTextures(1, &texture);
  }

  // moveable but not copyable
  ReprojectionTexture(ReprojectionTexture&& rhs) {
    texture = rhs.texture;
    scale = rhs.scale;
    offset = rhs.offset;
    rhs.texture = 0; // rhs is now invalid
  }

  GLuint texture;
  Eigen::Array3f scale;
  Eigen::Array3f offset;

  static GLuint createTexture(const ReprojectionTable& table) {
    GLuint texture = ::createTexture(GL_TEXTURE_3D);
    glTexImage3D(
        GL_TEXTURE_3D,
        0, // level
        GL_RG32F,
        table.shape.x(),
        table.shape.y(),
        table.shape.z(),
        0, // border
        GL_RG,
        GL_FLOAT,
        table.values.data());
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    return texture;
  }
};

}; // namespace fb360_dep
