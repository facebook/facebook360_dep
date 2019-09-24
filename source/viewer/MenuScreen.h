/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#ifdef WIN32

#include <algorithm>
#include <functional>
#include <iostream>

#include "OVR_CAPI_GL.h"
#include "Samples/OculusRoomTiny_Advanced/Common/Win32_GLAppUtil.h"
#include "source/thirdparty/stb_image.h"

namespace fb360_dep {

struct MenuScreen {
  Model* model = nullptr;
  ShaderFill* gridMaterial = nullptr;

  int transitionCounter = 0;
  static const int kTransitionFrames = 180;
  bool isHidden = false;
  bool doFadeOut = false;

  std::function<void()> exitMenuCallback; // this will be called when we leave the menu

  MenuScreen() {
    setupShaders();
    model = new Model(Vector3f(0, 0, 0), gridMaterial);
    model->AddSolidColorBox(-0.5, -0.5, 2, 0.5, 0.5, 2, 0xffffffff); // logo box
    model->AllocateBuffers();
  }

  void startFadeOut() {
    if (doFadeOut) {
      return;
    } // dont restart transition if it already started

    doFadeOut = true;
    transitionCounter = 0;
  }

  void resetToMenu() {
    doFadeOut = false;
    transitionCounter = 0;
    isHidden = false;
  }

  void update() {
    if (isHidden) {
      return;
    }

    if (doFadeOut) {
      ++transitionCounter;
      transitionCounter = std::min(transitionCounter, kTransitionFrames);
    }

    if (doFadeOut && (transitionCounter == kTransitionFrames)) {
      isHidden = true;
      doFadeOut = false;
      exitMenuCallback();
    }
  }

  void setupShaders() {
    static const GLchar* VertexShaderSrc =
        "#version 150\n"
        "uniform mat4 matWVP;\n"
        "in      vec4 Position;\n"
        "in      vec4 Color;\n"
        "in      vec2 TexCoord;\n"
        "out     vec2 oTexCoord;\n"
        "out     vec4 oColor;\n"
        "void main()\n"
        "{\n"
        "   gl_Position = (matWVP * Position);\n"
        "   oTexCoord   = TexCoord;\n"
        "   oColor.rgb  = pow(Color.rgb, vec3(2.2));\n" // convert from sRGB to linear
        "   oColor.a    = Color.a;\n"
        "}\n";

    static const char* FragmentShaderSrc =
        "#version 150\n"
        "uniform sampler2D Texture0;\n"
        "in      vec4      oColor;\n"
        "in      vec2      oTexCoord;\n"
        "out     vec4      FragColor;\n"
        "void main()\n"
        "{\n"
        "   FragColor = oColor * texture2D(Texture0, -oTexCoord + vec2(0.5, 0.5));\n"
        "}\n";

    GLuint vshader = CreateShader(GL_VERTEX_SHADER, VertexShaderSrc);
    GLuint fshader = CreateShader(GL_FRAGMENT_SHADER, FragmentShaderSrc);

    static const std::string kLogoFilename = "logo.png";
    static const int kDstChannels = 4;
    int width, height, channels;
    uint8_t* textureBytes =
        stbi_load(kLogoFilename.c_str(), &width, &height, &channels, kDstChannels);
    if (!textureBytes) {
      std::cout << "ERROR LOADING LOGO TEXTURE: " << kLogoFilename << std::endl;
      static uint8_t dummy[] = {
          255,
          0,
          0,
          0,
      };
      textureBytes = dummy;
      width = 1;
      height = 1;
      channels = 4;
    }

    TextureBuffer* generated_texture =
        new TextureBuffer(false, Sizei(width, height), 4, (unsigned char*)textureBytes);

    gridMaterial = new ShaderFill(vshader, fshader, generated_texture);
    glDeleteShader(vshader);
    glDeleteShader(fshader);
  }

  ~MenuScreen() {
    if (model != nullptr) {
      delete model;
    }
    if (gridMaterial != nullptr) {
      delete gridMaterial;
    }
  }

  void draw(Matrix4f view, Matrix4f proj) {
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    glClearColor(0, 0, 0, 1);
    if (transitionCounter > 0) {
      float brightness =
          pow(std::min(1.0f, float(transitionCounter) / float(kTransitionFrames)), 2.2f);
      glClearColor(brightness, brightness, brightness, 1);
    }

    model->Render(view, proj);
    CHECK_EQ(glGetError(), GL_NO_ERROR);
  }

  // NOTE: this is copy-pasted from Win32_GLAppUtil.h
  static GLuint CreateShader(GLenum type, const GLchar* src) {
    GLuint shader = glCreateShader(type);

    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint r;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &r);
    if (!r) {
      GLchar msg[1024];
      glGetShaderInfoLog(shader, sizeof(msg), 0, msg);
      if (msg[0]) {
        OVR_DEBUG_LOG(("Compiling shader failed: %s\n", msg));
      }
      return 0;
    }

    return shader;
  }
};

} // namespace fb360_dep
#endif // end ifdef WIN32
