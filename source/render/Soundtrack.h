/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifdef WIN32

#include <iostream>

#include "OVR_CAPI_GL.h"
#include "TBE_AudioEngine.h"

namespace fb360_dep {

struct Soundtrack {
  TBE::AudioEngine* audioEngine;
  TBE::SpatDecoderFile* audioFile;
  bool isReady;

  Soundtrack() : isReady(false) {}

  void load(const std::string& filename) {
    auto err = TBE_CreateAudioEngine(audioEngine);
    CHECK(err == TBE::EngineError::OK) << "failed to create audio engine: " << int(err);

    err = audioEngine->createSpatDecoderFile(audioFile);

    CHECK(err == TBE::EngineError::OK) << "failed to create audio file decoder" << int(err);

    // the engine's audiod device and mixer must be started
    audioEngine->start();
    audioEngine->enablePositionalTracking(true, TBE::TBVector(0, 0, 0));

    // setup an event callback to know when the file is ready for playback
    audioFile->setEventCallback(
        [](TBE::Event event, void* owner, void* userData) {
          if (event == TBE::Event::DECODER_INIT) {
            std::cout << "ready to play soundtrack" << std::endl;
            static_cast<Soundtrack*>(userData)->isReady = true;
          }
        },
        this);

    err = audioFile->open(filename.c_str());
    CHECK(err == TBE::EngineError::OK) << "failed to load audio file: " << filename;

    audioFile->setSyncMode(TBE::SyncMode::EXTERNAL);
  }

  void play() {
    if (isReady) {
      audioFile->play();
    }
  }

  void stop() {
    if (isReady) {
      audioFile->stop();
    }
  }

  void pause() {
    if (isReady) {
      audioFile->pause();
    }
  }

  void restart() {
    if (isReady) {
      audioFile->stop();
      audioFile->play();
    }
  }

  float getElapsedMs() {
    if (isReady) {
      return static_cast<float>(audioFile->getElapsedTimeInMs());
    }
    return 0;
  }

  void setElapsedMs(float ms) {
    if (isReady) {
      return audioFile->setExternalClockInMs(ms);
    }
  }

  bool isPlaying() {
    if (isReady) {
      return audioFile->getPlayState() == TBE::PlayState::PLAYING;
    }
    return false;
  }

  void updatePositionalTracking(ovrPosef& pose) {
    if (!isReady) {
      return;
    }
    audioEngine->setListenerPosition(
        TBE::TBVector(pose.Position.x, pose.Position.y, -pose.Position.z));
    audioEngine->setListenerRotation(TBE::TBQuat(
        pose.Orientation.x, pose.Orientation.y, -pose.Orientation.z, pose.Orientation.w));
  }

  ~Soundtrack() {
    if (isReady) {
      TBE_DestroyAudioEngine(audioEngine);
    }
  }
};

} // namespace fb360_dep
#endif // end ifdef WIN32
