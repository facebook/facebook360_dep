# Copyright 2004-present Facebook. All Rights Reserved.
#
# This software is proprietary and confidential to Facebook, Inc.  Any reproduction, distribution,
# modification, performance, display, and other use or disclosure is prohibited except to the
# extent expressly permitted in a written agreement executed by an authorized representative of
# Facebook, Inc.

cmake_minimum_required(VERSION 3.4)

project(Facebook360_DEP CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(UNIX AND NOT APPLE)
  set(LINUX TRUE)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(APPLE)
  set(CMAKE_OSX_ARCHITECTURES "${ARCHS_STANDARD}")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -m64 -O3 -funroll-loops -pipe")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mmmx -mavx -msse -msse3")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wsign-compare")

if(BUILD_LINE_COVERAGE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-generate -fcoverage-mapping")
endif()

if(NOT APPLE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

# Add extra directories that may not have been added by default
set(OPT_LOCAL_DIR "/opt/local")
if(EXISTS "${OPT_LOCAL_DIR}/lib")
  link_directories("${OPT_LOCAL_DIR}/lib")
  include_directories(SYSTEM "${OPT_LOCAL_DIR}/include")
endif()

set(USR_LOCAL_DIR "/usr/local")
if(EXISTS "${USR_LOCAL_DIR}/lib")
  link_directories("${USR_LOCAL_DIR}/lib")
  include_directories(SYSTEM "${USR_LOCAL_DIR}/include")
endif()

# Add custom targets to show all source and header files in Xcode
file(GLOB SRC "${CMAKE_CURRENT_SOURCE_DIR}/source/*")
add_custom_target(_source SOURCES ${SRC})

find_package(OpenCV 3 REQUIRED core imgproc imgcodecs highgui objdetect video)
set(OPENCV_COMPONENTS
    opencv_core
    opencv_imgproc
    opencv_imgcodecs
    opencv_highgui
    opencv_objdetect
    opencv_video)

# When adding Boost::chrono to target_link_libraries make sure it is listed after Boost::timer
# because Boost::timer depends on Boost::chrono
find_package(Boost REQUIRED COMPONENTS chrono filesystem system program_options regex timer)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Ceres 1.14 REQUIRED)
find_package(gflags 2.2 REQUIRED)
find_package(glog 0.3 REQUIRED)
find_package(Eigen3 3.3 REQUIRED)
find_package(double-conversion 3.0)
include_directories(${EIGEN3_INCLUDE_DIR})

if(APPLE)
  set(FOLLY_LIB folly)
else()
  find_package(Folly REQUIRED)
  set(FOLLY_LIB Folly::folly)
endif()
set(FOLLY_TARGET ${FOLLY_LIB} double-conversion)

###### Targets ######

### TARGET AlignColors ###

add_executable(
  AlignColors
  source/calibration/AlignColors.cpp
)
target_link_libraries(
  AlignColors
  LibUtil
)

### TARGET AlignPointCloud ###

add_executable(
  AlignPointCloud
  source/rig/AlignPointCloud.cpp
  source/conversion/PointCloudUtil.cpp
)
target_link_libraries(
  AlignPointCloud
  CalibrationLib
  LibUtil
)


### TARGET Calibration ###

add_executable(
  Calibration
  source/calibration/CalibrationMain.cpp
)
target_link_libraries(
  Calibration
  CalibrationLib
  LibUtil
  glfw
)

### TARGET CalibrationLib ###

add_library(
  CalibrationLib
  source/calibration/Calibration.cpp
  source/calibration/CalibrationLib.cpp
  source/calibration/FeatureDetector.cpp
  source/calibration/FeatureMatcher.cpp
  source/calibration/MatchCorners.cpp
  source/calibration/GeometricCalibration.cpp
)
target_link_libraries(
  CalibrationLib
  LibUtil
)
target_include_directories(
  CalibrationLib
  PRIVATE
  source/calibration
  source/util
)

### TARGET CalibrationLibMain ###

add_executable(
  CalibrationLibMain
  source/calibration/CalibrationLibMain.cpp
  source/calibration/CalibrationLib.h
)
target_link_libraries(
  CalibrationLibMain
  CalibrationLib
)

### TARGET DepUnitTest ###

add_executable(
  DepUnitTest
  source/test/DepUnitTest.cpp
  source/test/calibration/MatchCornersTest.cpp
  source/test/depth_estimation/DerpTest.cpp
  source/test/util/FThetaTest.cpp
  source/test/util/RectilinearTest.cpp
  source/test/util/OrthographicTest.cpp
  source/test/util/CameraTestUtil.cpp
)
target_link_libraries(
  DepUnitTest
  CalibrationLib
  LibUtil
  gtest
)

### TARGET ComputeRephotographyErrors ###

add_executable(
  ComputeRephotographyErrors
  source/render/ComputeRephotographyErrors.cpp
)
target_compile_features(ComputeRephotographyErrors PRIVATE cxx_range_for)
target_link_libraries(
  ComputeRephotographyErrors
  LibRender
)

### TARGET ConvertToBinary ###

include("${CMAKE_CURRENT_SOURCE_DIR}/ISPC.cmake")
add_executable(
  ConvertToBinary
  source/mesh_stream/ConvertToBinary.cpp
  source/render/MeshSimplifier.cpp
)
target_link_libraries(
  ConvertToBinary
  LibUtil
  ispc_texcomp
)

### TARGET CorrectVignetting ###

add_executable(
  CorrectVignetting
  source/isp/CorrectVignetting.cpp
)
target_link_libraries(
  CorrectVignetting
  LibUtil
)


### TARGET CreateObjFromDisparityEquirect ###

add_executable(
  CreateObjFromDisparityEquirect
  source/conversion/CreateObjFromDisparityEquirect.cpp
  source/render/MeshSimplifier.cpp
)
target_link_libraries(
  CreateObjFromDisparityEquirect
  LibUtil
)

### TARGET DerpCLI ###

add_executable(
  DerpCLI
  source/depth_estimation/DerpCLI.cpp
  source/depth_estimation/Derp.cpp
  source/depth_estimation/DerpUtil.cpp
  source/depth_estimation/UpsampleDisparityLib.cpp
)
target_link_libraries(
  DerpCLI
  LibUtil
)

### TARGET ExportPointCloud ###

add_executable(
  ExportPointCloud
  source/conversion/ExportPointCloud.cpp
)
target_link_libraries(
  ExportPointCloud
  LibUtil
)

### TARGET GenerateCameraOverlaps ###

add_executable(
  GenerateCameraOverlaps
  source/render/GenerateCameraOverlaps.cpp
)
target_link_libraries(
  GenerateCameraOverlaps
  LibUtil
)

### TARGET GenerateEquirect ###

add_executable(
  GenerateEquirect
  source/render/GenerateEquirect.cpp
)
target_link_libraries(
  GenerateEquirect
  LibUtil
)

### TARGET GenerateForegroundMasks ###

add_executable(
  GenerateForegroundMasks
  source/render/GenerateForegroundMasks.cpp
)
target_link_libraries(
  GenerateForegroundMasks
  LibUtil
)

### TARGET GenerateKeypointProjections ###

add_executable(
  GenerateKeypointProjections
  source/render/GenerateKeypointProjections.cpp
)
target_link_libraries(
  GenerateKeypointProjections
  CalibrationLib
  LibUtil
)

### TARGET GeometricCalibration ###

add_executable(
  GeometricCalibration
  source/calibration/GeometricCalibrationMain.cpp
)
target_link_libraries(
  GeometricCalibration
  CalibrationLib
  LibUtil
)

### TARGET GeometricConsistency ###

add_executable(
  GeometricConsistency
  source/render/GeometricConsistency.cpp
)
target_link_libraries(
  GeometricConsistency
  LibRender
)

### TARGET GlViewer ###

add_executable(
  GlViewer
  source/viewer/GlViewer.cpp
)
target_link_libraries(
  GlViewer
  LibRender
)

### TARGET ImportPointCloud ###

add_executable(
  ImportPointCloud
  source/conversion/ImportPointCloud.cpp
  source/conversion/PointCloudUtil.cpp
)
target_link_libraries(
  ImportPointCloud
  LibUtil
)

### TARGET LayerDisparities ###

add_executable(
  LayerDisparities
  source/depth_estimation/LayerDisparities.cpp
  source/depth_estimation/DerpUtil.cpp
)
target_link_libraries(
  LayerDisparities
  LibUtil
)

### TARGET LibRender ###

if (LINUX)
  find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)
else(LINUX)
  find_package(OpenGL)
endif(LINUX)

find_package(glfw3 3.3 REQUIRED)

if(APPLE)
  add_definitions(-DGL_SILENCE_DEPRECATION)
endif()

add_library(
  LibRender STATIC
  source/gpu/GlfwUtil.cpp
  source/render/CanopyScene.cpp
  source/render/RigScene.cpp
)
if (LINUX)
  target_link_libraries(
    LibRender
    LibUtil
    glfw
    OpenGL::OpenGL
    OpenGL::EGL
  )
else()
  find_package(GLEW REQUIRED)
  target_link_libraries(
    LibRender
    LibUtil
    glfw
    ${GLEW_LIBRARY}
    ${OPENGL_LIBRARIES}
  )
endif()


# For some reason the linking to the the GLUT library has to be made explicit for buck
# TARGET_MACOS needs to be triggered by an external cmake build system
if(TARGET_MACOS)
  target_link_libraries(
    LibRender
    /System/Library/Frameworks/GLUT.framework
  )
endif()

### TARGET LibUtil ###

file(GLOB util_SRC "source/util/*.cpp")
file(GLOB_RECURSE fb360_dep_HDRS "source/*.h")
add_library(
  LibUtil STATIC
  ${util_SRC}
  ${fb360_dep_HDRS}
)
target_link_libraries(
  LibUtil
  gflags
  Boost::filesystem
  Boost::system
  Boost::program_options
  Boost::timer
  Boost::chrono
  Eigen3::Eigen
  glog::glog
  ${OPENCV_COMPONENTS}
  ${CERES_LIBRARIES}
  ${FOLLY_TARGET}
)

### TARGET MatchCorners ###

add_executable(
  MatchCorners
  source/calibration/MatchCornersMain.cpp
)
target_link_libraries(
  MatchCorners
  CalibrationLib
  LibUtil
)

### TARGET PngToPfm ###
add_executable(
  PngToPfm
  source/conversion/PngToPfm.cpp
)
target_link_libraries(
  PngToPfm
  LibUtil
)

### TARGET ProjectCamerasToEquirects ###

add_executable(
  ProjectCamerasToEquirects
  source/conversion/ProjectCamerasToEquirects.cpp
)
target_link_libraries(
  ProjectCamerasToEquirects
  LibRender
)

### TARGET ProjectEquirectsToCameras ###

add_executable(
  ProjectEquirectsToCameras
  source/conversion/ProjectEquirectsToCameras.cpp
)
target_link_libraries(
  ProjectEquirectsToCameras
  LibUtil
)

### TARGET RawToRgb ###

add_executable(
  RawToRgb
  source/isp/RawToRgb.cpp
)
target_link_libraries(
  RawToRgb
  LibUtil
  ${OPENGL_LIBRARIES}
)

### TARGET RigAligner ###

add_executable(
  RigAligner
  source/rig/RigAligner.cpp
)
target_link_libraries(
  RigAligner
  LibUtil
)

### TARGET RigAnalyzer ###

add_executable(
  RigAnalyzer
  source/rig/RigAnalyzer.cpp
)
target_compile_features(RigAnalyzer PRIVATE cxx_range_for)
target_link_libraries(
  RigAnalyzer
  LibUtil
)

### TARGET RigCompare ###

add_executable(
  RigCompare
  source/rig/RigCompare.cpp
)
target_link_libraries(
  RigCompare
  ${CERES_LIBRARIES}
  LibUtil
)

### TARGET RigSimulator ###

add_executable(
  RigSimulator
  source/rig/RigSimulator.cpp
)
target_link_libraries(
  RigSimulator
  LibUtil
)

### TARGET SimpleMeshRenderer ###

add_executable(
  SimpleMeshRenderer
  source/render/SimpleMeshRenderer.cpp
)
target_link_libraries(
  SimpleMeshRenderer
  LibRender
)

### TARGET TemporalBilateralFilter ###

add_executable(
  TemporalBilateralFilter
  source/depth_estimation/TemporalBilateralFilter.cpp
  source/depth_estimation/Derp.cpp
  source/depth_estimation/DerpUtil.cpp
)
target_link_libraries(
  TemporalBilateralFilter
  LibUtil
)

### TARGET UpsampleDisparity ###

add_executable(
  UpsampleDisparity
  source/depth_estimation/UpsampleDisparity.cpp
  source/depth_estimation/DerpUtil.cpp
  source/depth_estimation/UpsampleDisparityLib.cpp
)
target_link_libraries(
  UpsampleDisparity
  LibUtil
)

### TARGET ViewColorVarianceThresholds ###

add_executable(
  ViewColorVarianceThresholds
  source/render/ViewColorVarianceThresholds.cpp
  source/depth_estimation/DerpUtil.cpp
)
target_link_libraries(
  ViewColorVarianceThresholds
  LibUtil
)

### TARGET ViewForegroundMaskThresholds ###

add_executable(
  ViewForegroundMaskThresholds
  source/render/ViewForegroundMaskThresholds.cpp
)
target_link_libraries(
  ViewForegroundMaskThresholds
  LibUtil
)
