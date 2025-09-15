/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/algorithm/string.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/gpu/GlfwUtil.h"
#include "source/render/RigScene.h"
#include "source/render/VideoFile.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;

const std::string kUsageMessage = R"(
  - OpenGL-based viewer for binary 6dof data files.

  Keyboard navigation:
  - w, a, s, d as well as the arrow keyes  will rotate the view.
  - z, and x move forward and backward.

  Mouse navigation:
  - Left button drag the mouse to rotate.
  - Right button drag the mouse to pan.

  Misc:
  - Hit 'r' to reset the view to what was on the command line.
  - Hit 'p' to dump the current view parameters in the command line format.

  - Example:
    ./GlViewer \
    --rig=/path/to/output/fused/rig_calibrated.json \
    --catalog=/path/to/output/fused/fused.json \
    --strip_files=/path/to/output/fused/fused_0.bin
  )";

DEFINE_string(catalog, "", "json file describing strip files");
DEFINE_string(strip_files, "", "comma-separated list of strip files");
DEFINE_int32(readahead, 3, "how many frames to read ahead");
DEFINE_string(rig, "", "path to rig .json file (required)");

static const float kEffectIncrement = 1; // meters per frame
static const float kEffectMax = 15; // meters
// Rig scene use a flipped coordinate system relative to canopy scene
// so we permute the coordinates on the way to the graphics pipeline
// match opengl's coordinate system.
static const Eigen::Matrix4f kPermutationMatrix(
    (Eigen::Matrix4f() << 1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1).finished());

namespace {

class GlViewer : public GlWindow {
 private:
 public:
  RigScene scene;
  std::unique_ptr<AsyncLoader> asyncLoader;
  std::unique_ptr<VideoFile> videoFile;

  GlViewer() : GlWindow("GL viewer", 512, 512), scene(RigScene(FLAGS_rig)) {
    // Initialize the viewer
    CHECK_NE(FLAGS_strip_files, "");
    std::vector<std::string> disks;
    boost::split(disks, FLAGS_strip_files, boost::is_any_of(","));
    videoFile = std::make_unique<VideoFile>(FLAGS_catalog, disks);
    if (videoFile->frames.size() == 1) {
      videoFile->readBegin(scene);
      scene.subframes = videoFile->readEnd(scene);
    } else {
      for (int i = 0; i < FLAGS_readahead; ++i) {
        videoFile->readBegin(scene);
      }
    }
  }

  void keyPress(int key, int s, int action, int mods) override {
    GlWindow::keyPress(key, s, action, mods);

    if (action != GLFW_PRESS) {
      switch (key) {
        case GLFW_KEY_0:
          scene.debug = 0;
          break;

        case GLFW_KEY_1:
          scene.debug = 1;
          break;

        case GLFW_KEY_2:
          scene.debug = 2;
          break;

        case GLFW_KEY_3:
          scene.debug = 3;
          break;

        case GLFW_KEY_4:
          scene.debug = 4;
          break;

        case GLFW_KEY_5:
          scene.debug = 5;
          break;

        case GLFW_KEY_6:
          scene.debug = 6;
          break;

        case GLFW_KEY_7:
          scene.debug = 7;
          break;

        case GLFW_KEY_8:
          scene.debug = 8;
          break;

        case GLFW_KEY_9:
          scene.debug = 9;
          break;

        case GLFW_KEY_L:
          effectBegin();
          break;
      }
    }
  }

  void effectBegin() {
    scene.effect = kEffectIncrement;
  }

  void effectUpdate() {
    if (scene.effect != 0) {
      scene.effect += kEffectIncrement;
    }
    if (scene.effect > kEffectMax) {
      scene.effect = 0;
    }
  }

  void display() override {
    if (videoFile->frames.size() > 1) {
      scene.destroyFrame(scene.subframes);
      scene.subframes = videoFile->readEnd(scene);
      videoFile->readBegin(scene, true);
    }

    // Loop effect
    effectUpdate();

    // draw the scene
    scene.render(projection * transform.matrix() * kPermutationMatrix, 0, true, wireframe);

    // Let the read thread drain if when we finish
    if (done && asyncLoader) {
      asyncLoader->wait();
    }
  }
};

} // anonymous namespace

int main(int argc, char* argv[]) {
  FLAGS_stderrthreshold = 0;
  FLAGS_logtostderr = false;

  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_rig, "");
  CHECK_NE(FLAGS_catalog, "");
  GlViewer glViewer;
  GlWindow::mainLoop();

  return EXIT_SUCCESS;
}
