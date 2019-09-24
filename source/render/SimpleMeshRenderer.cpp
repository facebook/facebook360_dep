/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

const char* kUsage = R"(
  - Reads a set of disparity (and optionally color) images for a rig and renders a fused version.
  It can either output images in a specified format or do a real-time on-screen rendering.

  For the latter:

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
    ./SimpleMeshRenderer \
    --first=000000 \
    --last=000000 \
    --rig=/path/to/rigs/rig.json \
    --color=/path/to/video/color \
    --disparity=/path/to/output/disparity \
    --output=/path/to/output/meshes \
    --format=cubecolor
)";

#include <future>
#include <set>
#include <vector>

#include <boost/algorithm/string/join.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/depth_estimation/DerpUtil.h"

#include "source/gpu/GlUtil.h"
#include "source/gpu/GlfwUtil.h"
#include "source/util/Camera.h"

#include "source/util/CvUtil.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"
#include "CanopyScene.h"
#include "DisparityColor.h"

using namespace fb360_dep;
using namespace fb360_dep::image_util;
using namespace fb360_dep::cv_util;
using namespace fb360_dep::depth_estimation;

// Keep the list below in alphabetical order
#define Format(X)           \
  X(cubecolor, "cubecolor") \
  X(cubedisp, "cubedisp")   \
  X(eqrcolor, "eqrcolor")   \
  X(eqrdisp, "eqrdisp")     \
  X(lr180, "lr180")         \
  X(snapcolor, "snapcolor") \
  X(snapdisp, "snapdisp")   \
  X(tb3dof, "tb3dof")       \
  X(tbstereo, "tbstereo")

enum struct Format {
#define ENUM_VALUE(name, str) name,
  Format(ENUM_VALUE)
#undef ENUM_VALUE
};

const std::string formatsArr[]{
#define NAME(name, str) str,
    Format(NAME)
#undef NAME
};

const std::set<std::string> formats(std::begin(formatsArr), std::end(formatsArr));

DEFINE_string(cameras, "", "comma-separated cameras to render (empty for all)");
DEFINE_string(color, "", "path to input color images (required)");
DEFINE_string(disparity, "", "path to disparity images (required)");
DEFINE_string(background, "", "path to optional background image");
DEFINE_string(background_equirect, "", "path to optional background equirect image");
DEFINE_string(file_type, "png", "Supports any image type allowed in OpenCV");
DEFINE_string(first, "000000", "first frame to process (lexical)");
DEFINE_string(forward, "-1.0 0.0 0.0", "forward for rendering");
DEFINE_int32(height, -1, "height of the rendering (pixels), default is width / 2");
DEFINE_double(horizontal_fov, 90, "horizontal field of view for rendering (degrees)");
DEFINE_bool(ignore_alpha_blend, false, "ignore alpha blend (useful if rendering single camera)");
DEFINE_string(last, "000000", "last frame to process (lexical) (ignored if on-screen rendering)");
DEFINE_string(output, "", "path to output directory");
DEFINE_string(position, "0.0 0.0 0.0", "position to render from (m)");
DEFINE_string(rig, "", "path to camera rig .json (required)");
DEFINE_string(up, "0.0 0.0 1.0", "up for rendering");
DEFINE_int32(width, 3072, "width of the rendering (pixels)");

const std::string formatsCsv =
    folly::sformat("{} (empty = on-screen rendering)", boost::algorithm::join(formats, ", "));
DEFINE_string(format, "", formatsCsv.c_str());

static const float kNearZ = 0.1f; // meters
void verifyInputs(const Camera::Rig& rig) {
  CHECK_NE(FLAGS_disparity, "");
  CHECK_NE(FLAGS_first, "");

  // On-screen rendering only renders the first frame
  if (!FLAGS_format.empty()) {
    CHECK_NE(FLAGS_last, "");
  }

  verifyImagePaths(FLAGS_disparity, rig, FLAGS_first, FLAGS_last);

  if (!FLAGS_color.empty()) {
    verifyImagePaths(FLAGS_color, rig, FLAGS_first, FLAGS_last);
  }

  CHECK_GT(FLAGS_width, 0);
  CHECK_EQ(FLAGS_width % 2, 0) << "width must be a multiple of 2";
  if (FLAGS_height == -1) {
    FLAGS_height = FLAGS_width / 2;
  }

  // Make sure format is valid
  if (!FLAGS_format.empty()) {
    CHECK(formats.find(FLAGS_format) != formats.end()) << "Invalid format: " << FLAGS_format;
  }

  // If a format needs color we need colors to be provided
  const std::set<std::string> formatsAllColor = {formatsArr[int(Format::eqrcolor)],
                                                 formatsArr[int(Format::cubecolor)],
                                                 formatsArr[int(Format::tbstereo)],
                                                 formatsArr[int(Format::lr180)],
                                                 formatsArr[int(Format::snapcolor)]};
  if (formatsAllColor.find(FLAGS_format) != formatsAllColor.end()) {
    CHECK_NE(FLAGS_color, "") << FLAGS_format << " needs --color to be set";
  }
}

static Eigen::Vector3f decodeVector(const std::string& flag) {
  Eigen::Vector3f result;
  std::istringstream s(flag);
  s >> result.x() >> result.y() >> result.z();
  CHECK(s) << "Unexpected flag " << flag;
  return result;
}

static std::string encodeVector(const Eigen::Vector3f& vector) {
  return folly::sformat("'{} {} {}'", vector.x(), vector.y(), vector.z());
}

void save(const filesystem::path& path, const cv::Mat_<cv::Vec4f>& result) {
  filesystem::create_directories(path.parent_path());
  cv::Mat out;
  if (FLAGS_file_type == "jpg") {
    out = cv_util::convertImage<cv::Vec3b>(result);
  } else if (FLAGS_file_type == "exr") {
    out = cv_util::convertImage<cv::Vec3f>(result);
  } else {
    out = cv_util::convertImage<cv::Vec3w>(result);
  }
  cv_util::imwriteExceptionOnFail(path, out);
}

std::vector<cv::Mat_<cv::Vec4f>>
loadColors(const Camera::Rig& rig, const std::string& frameName, const cv::Size& dummySize) {
  // To make the code flow simpler we generate dummy images when no color directory is provided so
  // we can reuse canopy scenes later on
  std::vector<cv::Mat_<cv::Vec4f>> colors;
  if (!FLAGS_color.empty()) {
    colors = loadImages<cv::Vec4f>(FLAGS_color, rig, frameName);
  } else {
    const cv::Mat_<cv::Vec4f> dummy(dummySize, 0);
    colors.assign(rig.size(), dummy);
  }
  return colors;
}

const std::vector<cv::Mat_<cv::Vec4f>> loadDisparitiesAsColors(
    const Camera::Rig& rig,
    const std::vector<cv::Mat_<float>>& disparities,
    bool& needDisparitiesAsColors) {
  // To make the code flow simpler we generate dummy images when no disparities as colors are needed
  // so we can reuse canopy scenes later on
  // Disparities as colors are needed when:
  // - On-screen rendering of disparities (1)
  // - Off-screen rendering shows disparities (2)

  // (1)
  const bool onscreenDisparities = FLAGS_format.empty() && FLAGS_color.empty();

  // (2)
  const std::set<std::string> formatsWithDisp = {formatsArr[int(Format::eqrdisp)],
                                                 formatsArr[int(Format::cubedisp)],
                                                 formatsArr[int(Format::snapdisp)],
                                                 formatsArr[int(Format::tb3dof)]};
  const bool offscreenDisparities = formatsWithDisp.find(FLAGS_format) != formatsWithDisp.end();

  needDisparitiesAsColors = onscreenDisparities || offscreenDisparities;
  std::vector<cv::Mat_<cv::Vec4f>> disparitiesAsColors;
  if (needDisparitiesAsColors) {
    const Eigen::Vector3f position = decodeVector(FLAGS_position);
    disparitiesAsColors = disparityColors(rig, disparities, position, metersToGrayscale);
  } else {
    const cv::Size& dummySize = disparities[0].size();
    const cv::Mat_<cv::Vec4f> dummy(dummySize, 0);
    disparitiesAsColors.assign(rig.size(), dummy);
  }
  return disparitiesAsColors;
}

class SimpleMeshWindow : public GlWindow {
 public:
  std::shared_ptr<CanopyScene> sceneColor;
  std::shared_ptr<CanopyScene> sceneDisp;

 protected:
  void report() {
    std::cerr << folly::sformat(
                     "--position {} --forward {} --up {} --horizontal_fov {}",
                     encodeVector(transform.inverse().translation()),
                     encodeVector(-transform.linear().row(2)),
                     encodeVector(transform.linear().row(1)),
                     FLAGS_horizontal_fov)
              << std::endl;
  }

  // forward and up assumed to be orthogonal and normalized
  Eigen::Affine3f forwardUp(const Eigen::Vector3f& forward, const Eigen::Vector3f& up) {
    Eigen::Affine3f result;
    result.linear().row(2) = -forward;
    result.linear().row(1) = up;
    result.linear().row(0) = up.cross(-forward);
    result.translation().setZero();
    const Camera::Real tol = 0.001;
    CHECK(result.linear().isUnitary(tol)) << forward << "/" << up << " not unitary";
    return result;
  }

  // forward and up just have to be non-parallel
  Eigen::Affine3f posForwardUp(
      const Eigen::Vector3f& position,
      const Eigen::Vector3f& forward,
      const Eigen::Vector3f& up) {
    Eigen::Vector3f right = up.cross(-forward);
    Eigen::Affine3f result = forwardUp(forward.normalized(), right.cross(forward).normalized());
    result.translation() = result * -position;
    return result;
  }

  cv::Mat_<cv::Vec4f> alphaBlend(const cv::Mat_<cv::Vec4f>& fore, const cv::Mat_<cv::Vec4f>& back) {
    CHECK_EQ(fore.rows, back.rows);
    CHECK_EQ(fore.cols, back.cols);
    cv::Mat_<cv::Vec4f> result(fore.rows, fore.cols);
    for (int y = 0; y < result.rows; ++y) {
      for (int x = 0; x < result.cols; ++x) {
        float alpha = fore(y, x)[3];
        if (std::isnan(alpha)) {
          result(y, x) = back(y, x); // just swap in background
        } else {
          result(y, x) = alpha * fore(y, x) + (1 - alpha) * back(y, x);
          result(y, x)[3] = alpha + (1 - alpha) * back(y, x)[3];
        }
      }
    }
    return result;
  }

  void backgroundEquirect(cv::Mat_<cv::Vec4f>& fore, const cv::Mat_<cv::Vec4f>& equi) {
    const int width = fore.cols;
    const int height = fore.rows;

    // compute transform
    const float xMax = kNearZ * tan(FLAGS_horizontal_fov / 180 * M_PI / 2);
    const Eigen::Affine3f transform = posForwardUp(
        decodeVector(FLAGS_position), decodeVector(FLAGS_forward), decodeVector(FLAGS_up));
    const Eigen::Affine3f inverse = transform.inverse();
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        float alpha = fore(y, x)[3];
        if (alpha == 1) {
          continue; // short-circuit for performance
        }

        // compute the center of the pixel at the near plane
        Eigen::Vector3f pixel = {
            ((x + 0.5f) / width * 2 - 1) * xMax,
            -((y + 0.5f) / height * 2 - 1) * xMax * height / width, // image upside down, sign flip
            -kNearZ};

        // scale it to near infinity and apply the inverse transform to get world coordinates
        Eigen::Vector3f world = inverse * (Camera::kNearInfinity * pixel);
        float lon = atan2(-world.y(), -world.x()); // -x is in the middle, -y to the right
        float lat = asin(world.normalized().z());
        float equiX = (-lon / M_PI + 1) / 2 * equi.cols; // sign flip to get 360 ... 0
        float equiY = (-lat / M_PI + 0.5) * equi.rows; // sign flip because images are upside down
        cv::Vec4f back = equi(equiY, equiX); // nearest
        if (std::isnan(alpha)) {
          fore(y, x) = back; // just swap in background
        } else {
          fore(y, x) = alpha * fore(y, x) + (1 - alpha) * back;
          fore(y, x)[3] = alpha + (1 - alpha) * back[3];
        }
      }
    }
  }

 public:
  SimpleMeshWindow(const ScreenState screenState)
      : GlWindow::GlWindow(
            "Simple Mesh Renderer",
            screenState & ON_SCREEN ? 512 : 8,
            screenState & ON_SCREEN ? 512 : 8,
            false,
            8,
            screenState) {
    up = decodeVector(FLAGS_up);
  }

  void keyPress(int key, int s, int action, int mods) override {
    GlWindow::keyPress(key, s, action, mods);

    if (action != GLFW_PRESS) {
      switch (key) {
        // print usage
        case GLFW_KEY_H:
          LOG(INFO) << "\n" << kUsage;
          break;
        // print report
        case GLFW_KEY_P:
          report();
          break;
      }
    }
  }

  void display() override {
    sceneColor->render(0, projection * transform);
  }

  cv::Mat_<cv::Vec4f> snapshot(const bool isColorDisp = false) {
    // Create snapshot framebuffer
    const int width = FLAGS_width;
    const int height = FLAGS_height;
    GLuint framebuffer = createFramebuffer();
    GLuint snapshot = createFramebufferColor(width, height, GL_RGBA32F);
    glViewport(0, 0, width, height);

    // Compute projection
    const float xMax = kNearZ * tan(FLAGS_horizontal_fov / 180 * M_PI / 2);
    Eigen::Projective3f projection =
        frustum(-xMax, xMax, -xMax * height / width, xMax * height / width, kNearZ);

    // Compute transform
    const Eigen::Affine3f transform = posForwardUp(
        decodeVector(FLAGS_position), decodeVector(FLAGS_forward), decodeVector(FLAGS_up));

    // Render scene and read result
    const float kIpd = 0.0f;
    if (isColorDisp) {
      sceneDisp->render(framebuffer, projection * transform, kIpd, !FLAGS_ignore_alpha_blend);
    } else {
      sceneColor->render(framebuffer, projection * transform, kIpd, !FLAGS_ignore_alpha_blend);
    }
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    cv::Mat_<cv::Vec4f> result(height, width);
    glReadPixels(0, 0, result.cols, result.rows, GL_BGRA, GL_FLOAT, result.ptr());
    const int kVertical = 0;
    cv::flip(result, result, kVertical);

    // Clean up
    glDeleteRenderbuffers(1, &snapshot);
    glDeleteFramebuffers(1, &framebuffer);
    return result;
  }

  cv::Mat_<cv::Vec4f> generate(const cv::Mat_<cv::Vec4f>& foreground) {
    cv::Mat_<cv::Vec4f> result;
    if (FLAGS_background.empty()) {
      result = foreground;
    } else {
      result = alphaBlend(foreground, cv_util::loadImage<cv::Vec4f>(FLAGS_background));
    }
    if (!FLAGS_background_equirect.empty()) {
      backgroundEquirect(result, cv_util::loadImage<cv::Vec4f>(FLAGS_background_equirect));
    }
    return result;
  }

  cv::Mat_<cv::Vec4f>
  stereo(const int formatIdx, const int width, const Eigen::Vector3f& position) {
    // Average human IPD is 6.4cm
    const float halfIpdM = 0.032f; // left = halfIpdM, right = -halfIpdM
    const cv::Mat_<cv::Vec4f> leftEye =
        generate(sceneColor->equirect(width, position, halfIpdM, !FLAGS_ignore_alpha_blend));
    const cv::Mat_<cv::Vec4f> rightEye =
        generate(sceneColor->equirect(width, position, -halfIpdM, !FLAGS_ignore_alpha_blend));

    cv::Mat_<cv::Vec4f> outputImage;
    if (formatIdx == int(Format::tbstereo)) {
      outputImage = cv_util::stackVertical<cv::Vec4f>({leftEye, rightEye});
    } else if (formatIdx == int(Format::lr180)) {
      // Crop half the image on each eye
      const cv::Rect roi(leftEye.cols / 4, 0, leftEye.cols / 2, leftEye.rows);
      const cv::Mat_<cv::Vec4f> left = leftEye(roi);
      const cv::Mat_<cv::Vec4f> right = rightEye(roi);
      outputImage = cv_util::stackHorizontal<cv::Vec4f>({left, right});
    }
    return outputImage;
  }

  cv::Mat_<cv::Vec4f> tb3dof(const int width, const Eigen::Vector3f& position) {
    const float ipd = 0.0f;
    const cv::Mat_<cv::Vec4f> color =
        generate(sceneColor->equirect(width, position, ipd, !FLAGS_ignore_alpha_blend));
    const cv::Mat_<cv::Vec4f> disparity =
        generate(sceneDisp->equirect(width, position, ipd, !FLAGS_ignore_alpha_blend));
    return cv_util::stackVertical<cv::Vec4f>({color, disparity});
  }
};

int main(int argc, char* argv[]) {
  gflags::SetUsageMessage(kUsage);
  system_util::initDep(argc, argv);

  // Load and filter cameras
  CHECK_NE(FLAGS_rig, "");
  const Camera::Rig rig = filterDestinations(Camera::loadRig(FLAGS_rig), FLAGS_cameras);
  CHECK_GT(rig.size(), 0);

  verifyInputs(rig);

  const int first = std::stoi(FLAGS_first);
  const int last = std::stoi(FLAGS_last);

  // On and off screen rendering
  SimpleMeshWindow window(FLAGS_format.empty() ? GlWindow::ON_SCREEN : GlWindow::OFF_SCREEN);

  for (int iFrame = first; iFrame <= last; ++iFrame) {
    const std::string frameName = image_util::intToStringZeroPad(iFrame, 6);
    LOG(INFO) << folly::sformat("Processing frame {}...", frameName);

    // Load disparities
    const std::vector<cv::Mat_<float>> disparities = loadPfmImages(FLAGS_disparity, rig, frameName);
    CHECK_EQ(ssize(disparities), ssize(rig));

    // Load colors
    const cv::Size& dummySize = disparities[0].size();
    const std::vector<cv::Mat_<cv::Vec4f>> colors = loadColors(rig, frameName, dummySize);
    CHECK_EQ(ssize(colors), ssize(rig));

    // Disparities need to be used as colors when we want to show disparity maps
    bool needDisparitiesAsColors;
    const std::vector<cv::Mat_<cv::Vec4f>> disparitiesAsColors =
        loadDisparitiesAsColors(rig, disparities, needDisparitiesAsColors);
    CHECK_EQ(ssize(disparitiesAsColors), ssize(rig));

    if (FLAGS_format.empty()) {
      const std::shared_ptr<CanopyScene> sceneColor(new CanopyScene(
          rig, disparities, needDisparitiesAsColors ? disparitiesAsColors : colors));

      window.sceneColor = sceneColor;

      // Render loop
      window.mainLoop();

      // Leave the loop
      break;
    }

    // Update the scene
    const std::shared_ptr<CanopyScene> sceneColor(new CanopyScene(rig, disparities, colors, false));
    const std::shared_ptr<CanopyScene> sceneDisp(
        new CanopyScene(rig, disparities, disparitiesAsColors, false));

    window.sceneColor = sceneColor;
    window.sceneDisp = sceneDisp;

    auto it = std::find(formats.begin(), formats.end(), FLAGS_format);
    const int formatIdx = std::distance(formats.begin(), it);

    cv::Mat_<cv::Vec4f> outputImage;
    const Eigen::Vector3f position = decodeVector(FLAGS_position);
    const float ipdDefault = 0.0f;

    switch (formatIdx) {
      case int(Format::eqrcolor): {
        outputImage = window.generate(
            sceneColor->equirect(FLAGS_height, position, ipdDefault, !FLAGS_ignore_alpha_blend));
        break;
      }
      case int(Format::eqrdisp): {
        outputImage = window.generate(
            sceneDisp->equirect(FLAGS_height, position, ipdDefault, !FLAGS_ignore_alpha_blend));
        break;
      }
      case int(Format::cubecolor): {
        outputImage = window.generate(
            sceneColor->cubemap(FLAGS_height, position, ipdDefault, !FLAGS_ignore_alpha_blend));
        break;
      }
      case int(Format::cubedisp): {
        outputImage = window.generate(
            sceneDisp->cubemap(FLAGS_height, position, ipdDefault, !FLAGS_ignore_alpha_blend));
        break;
      }
      case int(Format::lr180):
      case int(Format::tbstereo): {
        outputImage = window.generate(window.stereo(formatIdx, FLAGS_height, position));
        break;
      }
      case int(Format::tb3dof): {
        outputImage = window.generate(window.tb3dof(FLAGS_height, position));
        break;
      }
      case int(Format::snapcolor): {
        outputImage = window.generate(window.snapshot(false));
        break;
      }
      case int(Format::snapdisp): {
        outputImage = window.generate(window.snapshot(true));
        break;
      }
      default: {
        CHECK(false) << "Invalid format " << FLAGS_format;
      }
    }

    const filesystem::path filename =
        filesystem::path(FLAGS_output) / (frameName + "." + FLAGS_file_type);
    save(filename, outputImage);
    LOG(INFO) << "File saved in " << filename;
  }
  return EXIT_SUCCESS;
}
