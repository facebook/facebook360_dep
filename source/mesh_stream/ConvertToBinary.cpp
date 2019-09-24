/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <set>
#include <string>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/Format.h>

#include "source/conversion/BC7Util.h"
#include "source/mesh_stream/BinaryFusionUtil.h"
#include "source/render/MeshSimplifier.h"
#include "source/render/MeshUtil.h"
#include "source/util/FilesystemUtil.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;
using namespace fb360_dep::bc7_util;
using namespace fb360_dep::cv_util;
using namespace fb360_dep::image_util;
using namespace fb360_dep::system_util;

using PixelType = cv::Vec4f;
using Image = cv::Mat_<PixelType>;

const std::string kUsageMessage = R"(
       - Expects all files to be in the format <dir>/<camera>/<frame>.extension

       If <color> is specified:
       - Read .png files and save them as .rgba files in <bin> folder
       If <disparity> is specified:
       - Read .pfm files and save them as .vtx and .idx files in <bin> folder

       <bin> folder is created for each frame if it does not exist

       If <rgba> is specified:
       - Convert color image into an RGBA binary stream

       If <obj> is specified:
       - Read .vtx and .idx files from <bin> and save .obj files to <obj> folder

       - Example:
         ./ConvertToBinary \
         --color=/path/to/video/color \
         --rig=/path/to/rigs/rig.json \
         --first=000000 \
         --last=000000 \
         --disparity=/path/to/output/disparity \
         --bin=/path/to/output/bin \
         --fused=/path/to/output/fused
     )";

DEFINE_string(bin, "bin", "output directory containing binary data");
DEFINE_string(cameras, "", "cameras to render (comma-separated)");
DEFINE_string(color, "", "path to input color images");
DEFINE_double(color_scale, 1, "optional color scale before compression & fusion (>= 1 = no scale)");
DEFINE_double(depth_scale, 1, "optional depthmap scale before simplification (>= 1 = no scale)");
DEFINE_string(disparity, "", "path to disparity images (pfm)");
DEFINE_string(first, "", "first frame to process (lexical) (required)");
DEFINE_string(
    foreground_masks,
    "",
    "path to foreground masks specifying regions to include in per-frame geometry");
DEFINE_int32(fuse_strip, 1, "number of strip files");
DEFINE_string(fused, "", "output directory containing fused binary data, ready for playback");
DEFINE_double(gamma_correction, 2.2 / 1.8, "exponent to raise color channels before BC7 encoding");
DEFINE_string(last, "", "last frame to process (lexical) (required)");
DEFINE_string(
    output_formats,
    "idx,vtx,bc7",
    "saved formats, comma separated (idx, vtx, bc7 default; rgba, pfm, obj also supported)");
DEFINE_string(rig, "", "path to camera rig .json (required)");
DEFINE_bool(run_conversion, true, "whether or not to run binary conversion");
DEFINE_double(tear_ratio, 0.95, "depth ratio that causes mesh to tear");
DEFINE_int32(threads, -1, "number of threads (-1 = max allowed, 0 = no threading)");
DEFINE_int32(triangles, 150000, "number of triangles per camera mesh (<= 0: no simplification)");

void verifyInputs(const Camera::Rig& rig, const std::vector<std::string>& outputFormats) {
  CHECK_NE(FLAGS_rig, "");
  CHECK_NE(FLAGS_first, "");
  CHECK_NE(FLAGS_last, "");

  const std::set<std::string> supportedFormats = {"idx", "vtx", "bc7", "obj", "pfm", "rgba"};
  for (const std::string& outputFormat : outputFormats) {
    // We allow size 0 inputs to ensure stray commas are ignored, i.e. exr,,png is fine
    CHECK(outputFormat.size() == 0 || supportedFormats.find(outputFormat) != supportedFormats.end())
        << "Invalid output format specified: " << outputFormat;

    const bool isColor = outputFormat == "bc7" || outputFormat == "rgba";
    const bool isDisparity = outputFormat == "idx" || outputFormat == "vtx" ||
        outputFormat == "pfm" || outputFormat == "obj";
    if (!FLAGS_color.empty() && isColor) {
      verifyImagePaths(FLAGS_color, rig, FLAGS_first, FLAGS_last);
    } else {
      LOG(INFO) << "No color directory provided. Ignoring color conversion...";
    }
    if (!FLAGS_disparity.empty() && isDisparity) {
      verifyImagePaths(FLAGS_disparity, rig, FLAGS_first, FLAGS_last);
    } else {
      LOG(INFO) << "No disparity directory provided. Ignoring depth conversion...";
    }
    if (!FLAGS_foreground_masks.empty() && isDisparity) {
      verifyImagePaths(FLAGS_foreground_masks, rig, FLAGS_first, FLAGS_last);
    }
  }
}

void convertColor(
    const std::string& camId,
    const std::string& frameName,
    const bool saveBc7,
    const bool saveRgba) {
  if (!saveRgba && !saveBc7) {
    return;
  }

  LOG(INFO) << folly::sformat("Converting color: frame {}, camera {}...", frameName, camId);

  Image image = image_util::loadScaledImage<PixelType>(
      FLAGS_color, camId, frameName, FLAGS_color_scale, cv::INTER_AREA);

  if (saveBc7) {
    const filesystem::path bc7Path = image_util::imagePath(FLAGS_bin, camId, frameName, ".bc7");
    filesystem::create_directories(bc7Path.parent_path());
    const bool writeDDSHeader = false;
    bc7_util::compressBC7(image, bc7Path, FLAGS_gamma_correction, writeDDSHeader);
  }

  if (saveRgba) {
    // .rgba is just uncompressed 8-bit color
    cv::Mat_<cv::Vec4b> image = image_util::loadScaledImage<cv::Vec4b>(
        FLAGS_color, camId, frameName, FLAGS_color_scale, cv::INTER_AREA);
    cv::cvtColor(image, image, cv::COLOR_BGRA2RGBA, 4);
    const filesystem::path rgbaPath = image_util::imagePath(FLAGS_bin, camId, frameName, ".rgba");
    std::ofstream dstFile(rgbaPath.string(), std::ios::binary);
    dstFile.write(image.ptr<char>(), image.total() * image.elemSize());
  }
}

void convertDepth(
    const Camera& cam,
    const std::string& frameName,
    const bool saveIdx,
    const bool saveVtx,
    const bool savePfm,
    const bool saveObj) {
  if (!saveIdx && !saveVtx && !savePfm && !saveObj) {
    return;
  }

  const std::string& camId = cam.id;
  LOG(INFO) << folly::sformat("Converting depth: frame {}, camera {}...", frameName, camId);

  cv::Mat_<float> disparity = image_util::loadPfmImage(FLAGS_disparity, camId, frameName);
  cv::Mat_<float> depth = 1.0f / disparity;
  if (FLAGS_depth_scale < 1) {
    // nearest neighbor resize filter since we don't want to do any averaging of depths here
    cv::resize(depth, depth, cv::Size(), FLAGS_depth_scale, FLAGS_depth_scale, cv::INTER_NEAREST);
  }
  Eigen::MatrixXd vertexes = mesh_util::getVertexesEquiError(depth, cam);
  static const bool kWrapHorizontally = false;
  static const bool kIsSpherical = false;
  Eigen::MatrixXi faces = mesh_util::getFaces(
      vertexes, depth.cols, depth.rows, kWrapHorizontally, kIsSpherical, FLAGS_tear_ratio);

  // Remove geometry where we don't have valid depth data
  cv::Mat_<bool> vertexMask(depth.size());
  for (int i = 0; i < depth.rows; ++i) {
    for (int j = 0; j < depth.cols; ++j) {
      vertexMask(i, j) = !std::isnan(depth(i, j));
    }
  }

  if (!FLAGS_foreground_masks.empty()) {
    cv::Mat_<bool> foregroundMask =
        image_util::loadImage<bool>(FLAGS_foreground_masks, camId, frameName);
    cv::resize(foregroundMask, foregroundMask, depth.size(), 0, 0, cv::INTER_NEAREST);
    vertexMask = vertexMask & foregroundMask;
  }

  const int originalFaceCount = faces.rows();
  mesh_util::applyMaskToVertexesAndFaces(vertexes, faces, vertexMask);
  const int numFacesRemoved = originalFaceCount - faces.rows();
  LOG(INFO) << folly::sformat(
      "Removed {} of {} faces ({:.2f}%) corresponding to invalid depths and masked vertexes",
      numFacesRemoved,
      originalFaceCount,
      100.f * numFacesRemoved / (float)originalFaceCount);

  if (FLAGS_triangles > 0) {
    LOG(INFO) << folly::sformat("Target number of faces: {}", FLAGS_triangles);
    static const bool kIsEquierror = true;
    static const int kThreads = 1;
    render::MeshSimplifier ms(vertexes, faces, kIsEquierror, kThreads);
    static const float kStrictness = 0.2;
    static const bool kRemoveBoundaryEdges = false;
    ms.simplify(FLAGS_triangles, kStrictness, kRemoveBoundaryEdges);
    vertexes = ms.getVertexes();
    faces = ms.getFaces();

    // If depth is slightly negative, the viewer will take it to -infinity (it
    // does the inverse). We force this values to the minimum positive value
    for (int i = 0; i < vertexes.rows(); ++i) {
      if (vertexes.row(i).z() < 0) {
        vertexes.row(i).z() = FLT_MIN;
      }
    }
  }

  const filesystem::path vertexFilename =
      image_util::imagePath(FLAGS_bin, camId, frameName, ".vtx");
  filesystem::create_directories(vertexFilename.parent_path());

  const filesystem::path indexFilename = image_util::imagePath(FLAGS_bin, camId, frameName, ".idx");
  filesystem::create_directories(indexFilename.parent_path());

  if (saveIdx || saveVtx) {
    mesh_util::writeDepth(vertexes, faces, vertexFilename, indexFilename);
  }

  if (savePfm) {
    const filesystem::path depthFilename =
        image_util::imagePath(FLAGS_bin, camId, frameName, ".pfm");
    filesystem::create_directories(depthFilename.parent_path());
    mesh_util::writePfm(depth, cam.resolution, vertexes, faces, depthFilename);
  }

  if (saveObj) {
    LOG(INFO) << folly::sformat("Exporting obj: frame {}, camera {}...", frameName, camId);
    const filesystem::path objFilename = image_util::imagePath(FLAGS_bin, camId, frameName, ".obj");
    filesystem::create_directories(objFilename.parent_path());
    mesh_util::writeObj(
        mesh_util::readVertexes(vertexFilename), mesh_util::readFaces(indexFilename), objFilename);
  }
}

bool containsFormat(const std::vector<std::string>& formats, const std::string& format) {
  return std::find(formats.begin(), formats.end(), format) != formats.end();
}

void convertFrame(
    const Camera& cam,
    const std::string& frameName,
    const std::vector<std::string>& outputFormats) {
  if (!FLAGS_color.empty()) {
    const bool saveBc7 = containsFormat(outputFormats, "bc7");
    const bool saveRgba = containsFormat(outputFormats, "rgba");
    convertColor(cam.id, frameName, saveBc7, saveRgba);
  }
  if (!FLAGS_disparity.empty()) {
    const bool saveIdx = containsFormat(outputFormats, "idx");
    const bool saveVtx = containsFormat(outputFormats, "vtx");
    const bool savePfm = containsFormat(outputFormats, "pfm");
    const bool saveObj = containsFormat(outputFormats, "obj");
    convertDepth(cam, frameName, saveIdx, saveVtx, savePfm, saveObj);
  }
}

void fuse(const Camera::Rig& rig, const std::vector<std::string>& outputFormats) {
  // Open disks
  std::vector<FILE*> disks;
  boost::filesystem::create_directories(FLAGS_fused);
  for (int i = 0; i < FLAGS_fuse_strip; ++i) {
    const std::string diskName = folly::sformat("{}/fused_{}.bin", FLAGS_fused, std::to_string(i));
    FILE* disk = fopen(diskName.c_str(), "wb");
    CHECK(disk) << folly::sformat("Failed to open {}", diskName);
    disks.push_back(disk);
  }

  uint64_t offset = 0;
  folly::dynamic catalog = folly::dynamic::object;
  catalog["metadata"] = folly::dynamic::object;
  catalog["frames"] = folly::dynamic::object;
  catalog["metadata"]["isLittleEndian"] = folly::kIsLittleEndian;

  std::vector<std::string> extensions;
  for (const std::string& outputFormat : outputFormats) {
    extensions.push_back("." + outputFormat);
  }

  const int numFrames = std::stoi(FLAGS_last) - std::stoi(FLAGS_first) + 1;
  for (int iFrame = 0; iFrame < numFrames; ++iFrame) {
    const std::string frameName =
        image_util::intToStringZeroPad(iFrame + std::stoi(FLAGS_first), 6);
    LOG(INFO) << folly::sformat("Fusing frame {}...", frameName);
    binary_fusion::fuseFrame(catalog, disks, offset, FLAGS_bin, frameName, rig, extensions);
  }

  const std::string catalogFn = FLAGS_fused + "/fused.json";
  std::ofstream ostream(catalogFn, std::ios::binary);
  folly::PrintTo(catalog, &ostream); // PrintTo instead of toPrettyJson for sorted keys

  // Close disks
  for (FILE* disk : disks) {
    fclose(disk);
  }

  // Copy original fused rig
  const filesystem::path jsonSrc = filesystem::getFirstFile(FLAGS_bin, false, false, ".json");
  const filesystem::path jsonDst = filesystem::path(FLAGS_fused) / jsonSrc.filename();
  filesystem::copy_file(jsonSrc, jsonDst, filesystem::copy_option::overwrite_if_exists);
}

void resizeRig(Camera::Rig& rig) {
  for (Camera& camera : rig) {
    const Image image = image_util::loadScaledImage<PixelType>(
        FLAGS_color, camera.id, FLAGS_first, FLAGS_color_scale);
    const float xScale = float(image.cols) / camera.resolution.x();
    const float yScale = float(image.rows) / camera.resolution.y();
    CHECK_EQ(xScale, yScale) << folly::sformat(
        "Aspect ratio must be kept. {}x{} vs {}x{}, x-scale: {}, y-scale: {}",
        camera.resolution.x(),
        camera.resolution.y(),
        image.cols,
        image.rows,
        xScale,
        yScale);
    if (camera.id == rig[0].id) {
      LOG(INFO) << folly::sformat(
          "Fusing color images at {}x{} resolution", image.cols, image.rows);
    }
    if (xScale != 1) {
      camera = camera.rescale(xScale * camera.resolution);
    }
  }
}

int main(int argc, char** argv) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_LE(FLAGS_color_scale, 1.);
  CHECK_LE(FLAGS_depth_scale, 1.);

  CHECK_NE(FLAGS_rig, "");
  Camera::Rig rig = filterDestinations(Camera::loadRig(FLAGS_rig), FLAGS_cameras);
  CHECK_GT(rig.size(), 0) << "No cameras to convert";

  // Scale camera resolution to input color
  if (!FLAGS_color.empty()) {
    resizeRig(rig);
  }

  std::vector<std::string> outputFormats;
  folly::split(",", FLAGS_output_formats, outputFormats);
  verifyInputs(rig, outputFormats);

  if (FLAGS_run_conversion) {
    ThreadPool threadPool(FLAGS_threads);
    const int numFrames = std::stoi(FLAGS_last) - std::stoi(FLAGS_first) + 1;
    for (int iFrame = 0; iFrame < numFrames; ++iFrame) {
      const std::string frameName =
          image_util::intToStringZeroPad(iFrame + std::stoi(FLAGS_first), 6);
      for (const Camera& cam : rig) {
        threadPool.spawn([&, frameName, cam] { convertFrame(cam, frameName, outputFormats); });
      }
    }
    threadPool.join();

    const std::string stem = filesystem::path(FLAGS_rig).stem().string();
    const std::string rigFn = folly::sformat("{}/{}_fused.json", FLAGS_bin, stem);
    const std::vector<std::string> comments = {};
    const int doubleNumDigits = 10;
    Camera::saveRig(rigFn, rig, comments, doubleNumDigits);
  }

  if (!FLAGS_fused.empty()) {
    fuse(rig, outputFormats);
  }

  return EXIT_SUCCESS;
}
