/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <map>
#include <mutex>

#ifdef __APPLE__
#define GLFW_INCLUDE_GLCOREARB
#endif

#include "Eigen/Geometry"
// GlUtil.h needs to come first in this ordering
#include "source/gpu/GlUtil.h"
#include "source/util/MathUtil.h"

#include <GLFW/glfw3.h>
#include <glog/logging.h>

#include <folly/Format.h>

#ifdef __linux__
#define USE_EGL
#endif

#ifdef USE_EGL
#include <EGL/egl.h>
#undef None /* Avoid name colisions with folly */
#undef Bool

static const EGLint configAttribs[] = {
    EGL_SURFACE_TYPE,
    EGL_PBUFFER_BIT,
    EGL_BLUE_SIZE,
    8,
    EGL_GREEN_SIZE,
    8,
    EGL_RED_SIZE,
    8,
    EGL_DEPTH_SIZE,
    8,
    EGL_RENDERABLE_TYPE,
    EGL_OPENGL_BIT,
    EGL_NONE};

static const int pbufferWidth = 9;
static const int pbufferHeight = 9;

static const EGLint pbufferAttribs[] = {
    EGL_WIDTH,
    pbufferWidth,
    EGL_HEIGHT,
    pbufferHeight,
    EGL_NONE,
};
#endif

//!  A multi-window, OS independent opengl window
/*!
  Abstracts the glfw window across multiple instances and factors out
  code common to all glfw windows into the base class.

  The fundamental idea behind the window abstract is that the common
  cases should be simple and intuitive.  In particular:

  An instance with no arguments to the constructor creates an
  offscreen window. All on screen instances require a window name,
  width, and height.

  These two are the most common cases so the idea is that we make it
  easy to create those.  All the extra constructor arguments are
  designed for more complicated arrangements like borderless windows
  or windows that have both on and offscreen buffers.
*/
class GlWindow {
 public:
  enum ScreenState : unsigned { ON_SCREEN = 0x1, OFF_SCREEN = 0x2, BOTH_SCREEN = 0x3 };

 private:
  static std::mutex windowMapMutex;
  static std::map<GLFWwindow*, GlWindow*> windows;

  static void reshapeCallBack(GLFWwindow* window, const int w, const int h) {
    // Use this window
    glfwMakeContextCurrent(window);
    windows[window]->reshape(w, h);
  }

  static void
  mouseCallBack(GLFWwindow* window, const int button, const int action, const int mods) {
    glfwMakeContextCurrent(window);
    windows[window]->mouse(button, action, mods);
  }

  static void motionCallBack(GLFWwindow* window, const double x, const double y) {
    glfwMakeContextCurrent(window);
    windows[window]->motion(x, y);
  }

  static void
  keyCallBack(GLFWwindow* window, const int key, const int s, const int action, const int mods) {
    glfwMakeContextCurrent(window);
    windows[window]->keyPress(key, s, action, mods);
  }

 protected:
  const ScreenState screenState;
  int mouseButton; // GLFW_MOUSE_BUTTON_xxxx
  int mouseAction; // GLFW_MOUSE_xxx
  int mouseMods;
  Eigen::Vector2f mousePos;

  Eigen::Vector2i viewport;
  float scale; // the size of a pixel at z = -1
  Eigen::Projective3f projection;

  Eigen::Affine3f transform = Eigen::Affine3f::Identity();
  Eigen::Vector3f up;
  float pitch;
  float yaw;
  Eigen::Vector3f origin;
  bool wireframe;
  bool done;

  std::string name;
  int width;
  int height;

  GLFWwindow* window;
  GLuint fbo;

#ifdef USE_EGL
  EGLDisplay eglDpy;
#endif

  virtual void updateTransform() {
    const Eigen::Vector3f forward = {
        std::sin(pitch) * std::cos(yaw), std::sin(pitch) * std::sin(yaw), std::cos(pitch)};
    const Eigen::Vector3f right = up.cross(-forward).normalized();

    transform.linear().row(0) = right;
    transform.linear().row(1) = right.cross(forward);
    transform.linear().row(2) = -forward;
    transform.translation() = origin * scale;
  }

  virtual void reshape(const int w, const int h) {
    // Update this window's width and height state
    width = w;
    height = h;

    // Update the transformation matrix
    viewport = Eigen::Vector2i(w, h);
    glViewport(0, 0, w, h);

    // fit +/-1 into the window at z = -1
    scale = w < h ? 2.0f / w : 2.0f / h;

    static const float kNear = 0.1f;
    projection = frustum(
        -kNear * scale * w / 2.0f,
        kNear * scale * w / 2.0f,
        -kNear * scale * h / 2.0f,
        kNear * scale * h / 2.0f,
        kNear);
    updateTransform();
  }

  virtual void mouse(const int button, const int action, const int mods) {
    mouseButton = button;
    mouseAction = action;
    mouseMods = mods;
  }

  virtual void motion(const double x, const double y) {
    if (mouseAction == GLFW_PRESS) {
      if (mouseButton == GLFW_MOUSE_BUTTON_LEFT) { // rotate
        const float dPitch = (mousePos.y() - static_cast<float>(y));
        const float dYaw = (mousePos.x() - static_cast<float>(x));
        pitch += (dPitch / height) * (static_cast<float>(M_PI) / 2.0f);
        yaw -= (dYaw / width) * (static_cast<float>(M_PI) / 2.0f);
      } else { // pan
        Eigen::Vector3f move(
            static_cast<float>(x) - mousePos.x(),
            mousePos.y() - static_cast<float>(y),
            0); // y is down
        origin += move;
      }
      updateTransform();
    }
    mousePos = {x, y};
  }

  virtual void keyPress(const int key, const int s, const int action, const int mods) {
    if (action != GLFW_PRESS) {
      switch (key) {
        case GLFW_KEY_RIGHT:
        case GLFW_KEY_A:
          yaw -= static_cast<float>(M_PI) / 90;
          updateTransform();
          break;

        case GLFW_KEY_LEFT:
        case GLFW_KEY_D:
          yaw += static_cast<float>(M_PI) / 90;
          updateTransform();
          break;

        case GLFW_KEY_RIGHT_BRACKET:
        case GLFW_KEY_DOWN:
          pitch += static_cast<float>(M_PI) / 90;
          updateTransform();
          break;

        case GLFW_KEY_LEFT_BRACKET:
        case GLFW_KEY_UP:
          pitch -= static_cast<float>(M_PI) / 90;
          updateTransform();
          break;

        case GLFW_KEY_MINUS:
        case GLFW_KEY_S:
          origin[2] -= 1;
          updateTransform();
          break;

        case GLFW_KEY_EQUAL:
        case GLFW_KEY_W:
          origin[2] += 1;
          updateTransform();
          break;

        case GLFW_KEY_PERIOD:
          wireframe = !wireframe;
          updateTransform();
          break;

        case GLFW_KEY_R:
          resetTransformState();
          break;

        case GLFW_KEY_ESCAPE:
        case GLFW_KEY_Q:
          done = true;
          break;
      }
    }
  }

  void resetTransformState() {
    mousePos = {0, 0};
    transform = Eigen::Affine3f::Identity();
    origin = {0, 0, 0};
    pitch = static_cast<float>(M_PI) / 2;
    yaw = 0;
    updateTransform();
  }

  // This must be filled in by the sub-class
  virtual void display() = 0;

 public:
  //! Off-screen window constructor.
  GlWindow() : GlWindow("offscreen", 8, 8, true, 8, OFF_SCREEN) {}

  //! On-screen window constructor.
  GlWindow(
      const std::string& name, /*! \param Window name that appears in the border. required*/
      const int width, /*! \param Window width in pixels. required*/
      const int height, /*! \param Window height in pixels. required */
      const bool borderless = false, /*! \param Enable borderless windows, default=false. */
      const int outputBPP = 8, /*! \param Bits per pixel, default=8. */
      const ScreenState screenState =
          ON_SCREEN /*! \param Enable offscreen rendering or both. Default = ON_SCREEN */)

      : screenState(screenState),
        up({0, 0, 1}),
        wireframe(false),
        done(false),
        name(name),
        width(width),
        height(height) {
#ifdef USE_EGL
    // Only use EGL for offscreen rendering.
    if (screenState & OFF_SCREEN) {
      // 1. Initialize EGL
      eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);

      EGLint major, minor;
      eglInitialize(eglDpy, &major, &minor);

      // 2. Select an appropriate configuration
      EGLint numConfigs;
      EGLConfig eglCfg;
      eglChooseConfig(eglDpy, configAttribs, &eglCfg, 1, &numConfigs);

      // 3. Create a surface
      EGLSurface eglSurf = eglCreatePbufferSurface(eglDpy, eglCfg, pbufferAttribs);

      // 4. Bind the API
      eglBindAPI(EGL_OPENGL_API);

      // 5. Create a context and make it current
      EGLContext eglCtx = eglCreateContext(eglDpy, eglCfg, EGL_NO_CONTEXT, NULL);

      eglMakeCurrent(eglDpy, eglSurf, eglSurf, eglCtx);

      // Create a frame buffer to render into
      fbo = createFramebuffer();

      // Message the graphics device info
      LOG(INFO) << fmt::format("OpenGL off-screen renderer: {}", (char*)(glGetString(GL_RENDERER)));

      // Return early avoiding glfw entirely for offscreen rendering
      return;
    }
#endif
    // Used to report glfw faulures
    const char* lastGlfwErrorMessage;

    // Init the window system stuff
    const int glfwInitialization = glfwInit();
    glfwGetError(&lastGlfwErrorMessage);
    CHECK(glfwInitialization) << "failed -> " << lastGlfwErrorMessage;

    if (screenState & OFF_SCREEN) {
      glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    if (borderless) {
      glfwWindowHint(GLFW_DECORATED, GL_FALSE);
    }

    glfwWindowHint(GLFW_RED_BITS, outputBPP);
    glfwWindowHint(GLFW_GREEN_BITS, outputBPP);
    glfwWindowHint(GLFW_BLUE_BITS, outputBPP);

#ifdef USE_EGL_
    // Only use EGL for offscreen rendering.
    if (screenState & OFF_SCREEN) {
      glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
    }
#endif

    window = glfwCreateWindow(width, height, name.c_str(), NULL, NULL);
    glfwGetError(&lastGlfwErrorMessage);
    CHECK(window) << "creation failed -> " << lastGlfwErrorMessage;

    // Map this window to this instance
    {
      std::lock_guard<std::mutex> windowLock(windowMapMutex);
      windows[window] = this;
    }

    // Make this window current
    glfwMakeContextCurrent(window);

    // Only init Glew if we're using it and not, e.g., the Oculus-SDK-specific CAPI_GLE_GL
#ifdef __GLEW_H__
    glewExperimental = GL_TRUE;
    GLint GlewInitResult = glewInit();
    if (GLEW_OK != GlewInitResult) {
      throw std::runtime_error((char*)(glewGetErrorString(GlewInitResult)));
    }
#endif

    // Message the graphics device info
    LOG(INFO) << fmt::format("OpenGL on screen renderer: {}", (char*)(glGetString(GL_RENDERER)));

    // Create a place to render offscreen pixesl
    if (screenState & OFF_SCREEN) {
      fbo = createFramebuffer();
    }

    // Initialize UX transformation state
    resetTransformState();

    // Set callback functions
    if (screenState & ON_SCREEN) {
      glfwSetFramebufferSizeCallback(window, reshapeCallBack);
      glfwSetKeyCallback(window, keyCallBack);
      glfwSetMouseButtonCallback(window, mouseCallBack);
      glfwSetCursorPosCallback(window, motionCallBack);

      glfwSwapInterval(1);

      // Make sure the window and view port are the same
      glfwGetFramebufferSize(window, &(this->width), &(this->height));
      reshape(this->width, this->height);
    }
  }

  virtual ~GlWindow() {
    {
      std::lock_guard<std::mutex> windowLock(windowMapMutex);
      windows.erase(window);
    }

#ifdef USE_EGL
    if (screenState & OFF_SCREEN) {
      // cleanup offscreen rendering
      glDeleteFramebuffers(1, &fbo);
      eglTerminate(eglDpy);
    }
#else
    if (screenState & OFF_SCREEN) {
      // cleanup offscreen rendering
      glDeleteFramebuffers(1, &fbo);
    }
    glfwDestroyWindow(window);
    glfwTerminate();
#endif
  }

  static void mainLoop() {
    // This is not re-entrant so we'll only let one in.
    static std::mutex once;
    static std::lock_guard<std::mutex> onlyOnce(once);

    // Loop until all the windows exit
    static bool windowsAvailable;

    // Make sure we read the windows data structure in a thread safe manner.
    {
      std::lock_guard<std::mutex> windowLock(windowMapMutex);
      windowsAvailable = !windows.empty();
    }

    // Loop until we kill all the windows
    while (windowsAvailable) {
      // This lock allows thread safe window creation and destruction
      // to happen between window updates.
      std::lock_guard<std::mutex> windowLock(windowMapMutex);

      // Loop over all the windows giving each a chance to draw.
      for (std::map<GLFWwindow*, GlWindow*>::iterator it = windows.begin(); it != windows.end();) {
        // Check for input between window draws
        glfwPollEvents();

        // Grab the window ptrs
        GLFWwindow* glfwWindow = it->first;
        GlWindow* glWindow = it->second;

        // Use this window
        glfwMakeContextCurrent(glfwWindow);

        // Call the user's display code
        glWindow->display();

        // Show the frame buffer
        glfwSwapBuffers(glfwWindow);

        // Remove the window if it's been flagged as done
        it = (glWindow->done) ? windows.erase(it) : ++it;
      }
      windowsAvailable = !windows.empty();
    }
  }
};
