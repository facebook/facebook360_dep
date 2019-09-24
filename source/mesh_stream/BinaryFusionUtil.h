/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdio>
#include <fstream>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/mesh_stream/StripedFile.h"
#include "source/render/VideoFile.h"
#include "source/util/Camera.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"

namespace fb360_dep {
namespace binary_fusion {

void addFile(std::vector<FILE*>& disks, uint64_t& offset, const filesystem::path& filename) {
  uint64_t aligned = align(offset, kStripeSize);
  uint64_t end = offset == aligned ? offset + kStripeSize : aligned;
  uint64_t size = filesystem::file_size(filename);
  FILE* file = fopen(filename.c_str(), "rb");
  LOG(INFO) << folly::sformat("Fusing {}...", filename.string());
  while (size) {
    std::vector<uint8_t> buffer(std::min(size, end - offset));
    CHECK_EQ(fread(buffer.data(), 1, buffer.size(), file), buffer.size())
        << "Error reading buffer data";
    uint64_t local, disk;
    StripedFile::calcStripe(local, disk, offset, disks.size());
    fwrite(buffer.data(), 1, buffer.size(), disks[disk]);
    offset += buffer.size();
    end = offset + kStripeSize;
    size -= buffer.size();
  }
  fclose(file);
}

void pad(std::vector<FILE*>& disks, uint64_t& offset) {
  uint64_t aligned = align(offset, kStripeSize);
  if (offset == aligned) {
    return; // our work here is done
  }
  std::vector<uint8_t> buffer(aligned - offset, 0x5A);
  uint64_t local, disk;
  StripedFile::calcStripe(local, disk, offset, disks.size());
  fwrite(buffer.data(), 1, buffer.size(), disks[disk]);
  offset += buffer.size();
}

void fuseFrame(
    folly::dynamic& catalog,
    std::vector<FILE*>& disks,
    uint64_t& offset,
    const filesystem::path& dirBin,
    const std::string& frameName,
    const Camera::Rig& rig,
    const std::vector<std::string>& extensions) {
  // Fuse each camera in the frame
  folly::dynamic& frame = catalog["frames"][frameName];
  frame = folly::dynamic::object;
  for (const Camera& cam : rig) {
    uint64_t begin = offset;

    // Fuse each extension in the camera
    folly::dynamic& camera = frame[cam.id];
    camera = folly::dynamic::object;
    for (const std::string& extension : extensions) {
      uint64_t begin = offset;
      addFile(disks, offset, dirBin / cam.id / (frameName + extension));
      camera[extension] = folly::dynamic::object("offset", begin)("size", offset - begin);
    }
    camera["offset"] = begin;
    camera["size"] = offset - begin;
    pad(disks, offset);
  }
}

} // namespace binary_fusion
} // namespace fb360_dep
