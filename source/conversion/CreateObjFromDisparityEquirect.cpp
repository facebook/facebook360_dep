/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/Format.h>

#include "source/render/MeshSimplifier.h"
#include "source/render/MeshUtil.h"
#include "source/util/CvUtil.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;
using namespace fb360_dep::cv_util;
using namespace fb360_dep::mesh_util;
using namespace fb360_dep::render;
using namespace fb360_dep::system_util;

const std::string kUsageMessage = R"(
  - Creates an OBJ (optionally with texturing) from a disparity equirect.

  - Example:
    ./CreateObjFromDisparityEquirect \
    --input_png_color=/path/to/equirects/color.png \
    --input_png_disp=/path/to/equirects/disparity.png \
    --output_obj=/path/to/output/test.obj
  )";

DEFINE_bool(create_mtl, false, "cerate MTL file and attach to OBJ");
DEFINE_string(input_png_color, "", "path to input color png (required)");
DEFINE_string(input_png_disp, "", "path to input disparity png (required)");
DEFINE_double(max_depth, 700.0, "maximum depth. Use something like 20 to visualize");
DEFINE_int32(num_faces, 200000, "number of output faces");
DEFINE_string(output_obj, "", "path to output obj file (required)");
DEFINE_double(scale, 1.0, "depth map resolution before decimation");
DEFINE_double(strictness, 0.8, "[0, 1] mesh simplification aggressiveness. 0 = no simplification");
DEFINE_double(tear_ratio, 0.95, "depth ratio that causes mesh to tear");
DEFINE_int32(threads, 12, "number of threads");

int main(int argc, char** argv) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_input_png_disp, "");
  CHECK_NE(FLAGS_input_png_color, "");
  CHECK_NE(FLAGS_output_obj, "");

  CHECK(0 <= FLAGS_strictness && FLAGS_strictness <= 1) << "strictness must be between 0 and 1";

  LOG(INFO) << "Reading disparity image...";
  cv::Mat_<float> disp = cv_util::loadImage<float>(FLAGS_input_png_disp);

  if (FLAGS_scale < 1) {
    LOG(INFO) << "Resizing input file...";
    cv::resize(disp, disp, cv::Size(), FLAGS_scale, FLAGS_scale);
  }

  // Generate set of vertexes and faces
  LOG(INFO) << "Generating vertexes...";
  Eigen::MatrixXd vertexes = mesh_util::getVertexesEquirect(disp, FLAGS_max_depth);
  LOG(INFO) << "Generating faces...";
  const bool wrapHorizontally = true;
  const bool isRigCoordinates = true;
  Eigen::MatrixXi faces = mesh_util::getFaces(
      vertexes, disp.cols, disp.rows, wrapHorizontally, isRigCoordinates, FLAGS_tear_ratio);

  // Simplify
  if (FLAGS_strictness > 0) {
    LOG(INFO) << "Mesh simplification...";
    static const bool kIsEquiError = false;
    MeshSimplifier ms(vertexes, faces, kIsEquiError, FLAGS_threads);
    ms.simplify(FLAGS_num_faces, FLAGS_strictness);
    vertexes = ms.getVertexes();
    faces = ms.getFaces();
  }

  LOG(INFO) << folly::sformat("Num vertexes: {}, num faces: {}", vertexes.size(), faces.size());

  // Create MTL and OBJ files
  LOG(INFO) << "Creating OBJ...";

  std::string fnMtl;
  if (FLAGS_create_mtl) {
    mesh_util::addTextureCoordinatesEquirect(vertexes);
    std::string fnMtl = mesh_util::writeMtl(FLAGS_output_obj, FLAGS_input_png_color);
  }
  mesh_util::writeObj(vertexes, faces, FLAGS_output_obj, fnMtl);

  return EXIT_SUCCESS;
}
