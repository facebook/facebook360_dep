/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/gpu/GlfwUtil.h"

// Global thread-safe map variable
std::mutex GlWindow::windowMapMutex;
std::map<GLFWwindow*, GlWindow*> GlWindow::windows;
