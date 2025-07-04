/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>

#include <boost/filesystem.hpp>
#include <fmt/format.h>

#include <folly/Format.h>

#include "source/gpu/GlUtil.h"
#include "source/mesh_stream/StripedFile.h"
#include "source/render/RigScene.h"

namespace fb360_dep {

// a video file is a striped file with a catalog describing the layout
struct VideoFile {
  StripedFile stripedFile;
  folly::dynamic catalog;
  std::vector<std::string> frames;
  int current = 0;

  VideoFile(const VideoFile& videoFile) = delete;

  VideoFile& operator=(const VideoFile& videoFile) = delete;

  VideoFile(const std::string& catalogName, const std::vector<std::string>& diskNames)
      : stripedFile(diskNames), catalog(parseCatalog(catalogName)) {
    // find and sort all the frame names
    for (const auto& key : catalog["frames"].keys()) {
      frames.push_back(key.getString());
    }
    CHECK(frames.size()) << "no frames in catalog " << catalogName;
    sort(frames.begin(), frames.end());
    LOG(INFO) << fmt::format("{} frames found", frames.size());
  }

  int getFront() const {
    return static_cast<int>((current - pending.size() + frames.size()) % frames.size());
  }

  void readBegin(const RigScene& scene, bool cull = false) {
    const folly::dynamic& frame = catalog["frames"][frames[current]];
    pending.emplace_back();
    std::vector<Loader>& loaders = pending.back();
    // kick off a loader for every camera in scene.rig
    loaders.reserve(scene.rig.size());
    for (int i = 0; i < int(scene.rig.size()); ++i) {
      const Camera& camera = scene.rig[i];
      const folly::dynamic& layout = frame[camera.id];
      if (cull && i < int(scene.culled.size()) && scene.culled[i]) {
        loaders.push_back({nullptr, 0, 0, layout, nullptr});
        loaders.back().read = nullptr;
      } else {
        const uint64_t size = layout["size"].getInt();
        // when reading, size must be page aligned
        const uint64_t sizeAligned = align(size, kPageSize);
        // allocate, map and align a buffer
        const uint64_t sizeAlloc = sizeAligned + kPageSize - 1;
        const GLuint buffer = createBuffer(kBufferType, (uint8_t*)nullptr, sizeAlloc);
        uint8_t* const p = static_cast<uint8_t*>(glMapBuffer(kBufferType, GL_WRITE_ONLY));
        CHECK(p);
        uint8_t* const pAligned = align(p, kPageSize);
        glBindBuffer(kBufferType, 0);
        // start the read
        const uint64_t offset = layout["offset"].getInt();
        StripedFile::PendingRead* const read = stripedFile.readBegin(pAligned, offset, sizeAligned);
        // stash the loader information for this camera
        const uint64_t offsetUnaligned = offset - (pAligned - p);
        loaders.push_back({read, buffer, offsetUnaligned, layout, p});
      }
    }
    // increment frame counter
    current = (current + 1) % frames.size();
  }

  // blocking function: wait for disk read
  void readWait(const RigScene& scene, int index = 0) {
    CHECK(index < int(pending.size()));
    const std::vector<Loader>& loaders = pending[index];
    for (const Loader& loader : loaders) {
      if (loader.read != nullptr) {
        stripedFile.readEnd(loader.read);
      }
    }
  }

  // unmap gl buffer
  void readUnmap(const RigScene& scene, int index = 0) {
    CHECK(index < int(pending.size()));
    const std::vector<Loader>& loaders = pending[index];
    for (const Loader& loader : loaders) {
      if (loader.read != nullptr) {
        glBindBuffer(kBufferType, loader.buffer);
        glUnmapBuffer(kBufferType);
        glBindBuffer(kBufferType, 0);
      }
    }
  }

  // create subframes from read data
  std::vector<RigScene::Subframe> readFrame(const RigScene& scene) {
    CHECK(!pending.empty());
    const std::vector<Loader>& loaders = pending.front();
    CHECK_EQ(loaders.size(), scene.rig.size());
    std::vector<RigScene::Subframe> result;
    // create a subframe for every camera in scene.rig
    result.reserve(scene.rig.size());
    for (int i = 0; i < int(scene.rig.size()); ++i) {
      const Loader& loader = loaders[i];
      if (loader.read == nullptr) {
        result.emplace_back();
      } else {
        // create the frame
        result.emplace_back(
            scene.createSubframe(scene.rig[i], loader.buffer, loader.offset, loader.layout));
      }
    }
    pending.pop_front();
    return result;
  }

  // blocking function
  std::vector<RigScene::Subframe> readEnd(const RigScene& scene) {
    readWait(scene);
    readUnmap(scene);
    return readFrame(scene);
  }

 private:
  static folly::dynamic parseCatalog(const std::string& fileName) {
    CHECK(boost::filesystem::exists(boost::filesystem::path(fileName)));
    std::ifstream file(fileName, std::ios::binary);
    folly::dynamic catalog = folly::parseJson(std::string(
        (std::istreambuf_iterator<char>(file)), // most vexing parse
        (std::istreambuf_iterator<char>())));

    // Update legacy files without (both) metadata and frames entries
    if (catalog.find("metadata") == catalog.items().end()) {
      LOG(WARNING) << "No metadata found";
      CHECK(catalog.find("frames") == catalog.items().end()) << "Malformed catalog file";

      folly::dynamic oldCatalog = catalog;
      catalog = folly::dynamic::object;
      catalog["frames"] = oldCatalog;

      // Assume native endianness
      catalog["metadata"] = folly::dynamic::object;
      catalog["metadata"]["isLittleEndian"] = folly::kIsLittleEndian;
    }

    CHECK_EQ(folly::kIsLittleEndian, catalog["metadata"]["isLittleEndian"].getBool())
        << "Endianness mismatch between video file and native platform";

    return catalog;
  }

  GLenum kBufferType = GL_TEXTURE_BUFFER; // unimportant, pick unused type

  struct Loader {
    StripedFile::PendingRead* read;
    GLuint buffer;
    uint64_t offset; // offset of the unaligned buffer
    const folly::dynamic layout; // HACK FOR WINDOWS: should be reference
    uint8_t* p; // for debugging
  };

  std::deque<std::vector<Loader>> pending;
};

} // namespace fb360_dep
