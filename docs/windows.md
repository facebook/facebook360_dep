---
id: rift
title: Rift Viewer
---

To view your contents in the Rift headset, follow the instructions below. This build currently
*only* works on Windows.

## Dependencies

### vcpkg
~~~~
https://github.com/Microsoft/vcpkg
~~~~

Once installed:
~~~~
vcpkg install boost:x64-windows-static \
  eigen3:x64-windows-static \
  glog:x64-windows-static \
  stb:x64-windows-static \
  folly:x64-windows-static \
  pthreads:x64-windows-static \
  glew:x64-windows-static
~~~~

### Oculus SDK (OCULUS_ROOT)
~~~~
https://developer3.oculus.com/downloads/
~~~~


### Audio360/TwoBigEars (AUDIO360_ROOT)
~~~~
https://s3.amazonaws.com/fb360-spatial-workstation/RenderingEngine/1.0.2/Audio360_SDK_1.0.2.zip
~~~~


### GLFW 3.3 source (GLFW_ROOT)
~~~~
https://www.glfw.org/download.html
~~~~


## Generate Visual Studio project files
Assuming dependencies above are downloaded/installed in C:/Software and we're building with Visual Studio 2017, first, generate relevant VS project and solution files for GLFW:

~~~~
cd C:/Software/glfw-3.3
mkdir build
cd build
cmake \
 -DBUILD_SHARED_LIBS=OFF \
 -DUSE_MSVC_RUNTIME_LIBRARY_DLL=OFF \
 -G "Visual Studio 15 2017 Win64" ..
~~~~

Open and build GLFW in Visual Studio. Then generate the same for facebook360_dep:

~~~~
cd $FACEBOOK360_DEP_ROOT/windows
mkdir build
cd build
cmake \
  -DOCULUS_ROOT="C:/Software/ovr_sdk_win_1.35.0" \
  -DAUDIO360_ROOT="C:/Software/Audio360_SDK_1.0.2/Audio360" \
  -DGLFW_ROOT="C:/Software/glfw-3.3" \
  -DCMAKE_TOOLCHAIN_FILE="C:/Software/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-windows-static \
  -DCMAKE_BUILD_TYPE=Release \
  -G "Visual Studio 15 2017 Win64" -Wno-dev ..
~~~~

Open the VS project $FACEBOOK360_DEP_ROOT/windows/Project.sln with VS and modify the RiftViewer project properties: under _C/C++: All options_, remove `/permissive-` from _Additional Options_.

Optionally use `-DCMAKE_BUILD_TYPE=Debug` for a debug build.

NOTE:
- You must copy res/logo.png into the same directory as RiftViewer.exe.
- $AUDIO360_ROOT/x64 must be on the PATH.
