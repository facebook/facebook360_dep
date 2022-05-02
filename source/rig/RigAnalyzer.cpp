/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <iostream>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/util/Camera.h"
#include "source/util/SystemUtil.h"

using namespace fb360_dep;

const std::string kUsageMessage = R"(
   - Miscellaneous analysis utilities for a rig. Various output formats are supported to
   visualize the rig setup (e.g. equirect projection).

   - Example:
     ./RigAnalyzer \
     --rig=/path/to/rigs/rig.json \
     --output_equirect=/path/to/output/equirect.png
 )";

DEFINE_double(custom, -1, "custom angle away from north");
DEFINE_double(discard_poles, 0, "degrees from poles to ignore");
DEFINE_string(eulers, "", "create from eulers file");
DEFINE_double(min_distance, 0.50, "min distance to test");
DEFINE_double(
    overlap_distance,
    Camera::kNearInfinity,
    "distance to visualize equirect overlap, default is INF");
DEFINE_bool(one_based_indexing, false, "enable to index cameras starting at 1 instead of 0");
DEFINE_string(output_camera, "", "path to output camera .ppm file");
DEFINE_string(output_camera_id, "", "output camera id");
DEFINE_string(output_cross_section, "", "path to output cross section .ppm file");
DEFINE_string(output_equirect, "", "path to output equirect .ppm file");
DEFINE_string(output_obj, "", "path to output rig .obj file");
DEFINE_string(output_rig, "", "path to output rig .json file");
DEFINE_bool(perturb_cameras, false, "");
DEFINE_double(perturb_focals, 0, "pertub focals");
DEFINE_double(perturb_positions, 0, "perturb positions (cm)");
DEFINE_double(perturb_principals, 0, "pertub principals (pixels)");
DEFINE_double(perturb_rotations, 0, "perturb rotations (radians)");
DEFINE_int32(perturb_seed, 1, "seed for perturb cameras. Default: 1, same as no seed");
DEFINE_double(radius, 0, "change rig radius");
DEFINE_string(
    rearrange,
    "",
    "create specific arrangement (ballcam24, tetra, ring4, cube, carbon0, carbon1, diamond)");
DEFINE_string(revolve, "", "create from angle file");
DEFINE_string(rig, "", "path to rig .json file (required)");
DEFINE_string(rotate, "", "rotate rig by euler angles");
DEFINE_string(rotate_cam_z, "", "rotate camera to align with z");
DEFINE_int32(sample_count, 100000, "number of samples");
DEFINE_double(scale_resolution, 1, "scale camera resolutions");
DEFINE_bool(show_timing, false, "visualize time as well as spatial overlap");
DEFINE_bool(z_is_down, false, "modify rig from y-is-up to z-is-down");
DEFINE_bool(z_is_up, false, "modify rig from y-is-up to z-is-up");
DEFINE_double(scale_rig, 1, "scale rig space, e.g., by 1e-2 to convert from cm to m");

// generates a fairly uniform distribution that is very regular
std::vector<Camera::Vector3> getFibonacciUnits(int count) {
  std::vector<Camera::Vector3> points;
  for (int i = 0; i < count; ++i) {
    Camera::Real y = (i + 0.5) / count * 2 - 1;
    Camera::Real r = sqrt(1 - y * y);
    Camera::Real phi = (1 + sqrt(5)) / 2;
    Camera::Real roty = i / phi * 2 * M_PI;
    points.emplace_back(sin(roty) * r, y, cos(roty) * r);
  }
  return points;
}

// discard points that are within radians of the z axis
// samples are assumed to be unit vectors
std::vector<Camera::Vector3> discardPoles(
    const std::vector<Camera::Vector3>& samples,
    const Camera::Real radians) {
  const Camera::Real threshold = cos(radians);
  std::vector<Camera::Vector3> result;
  for (const Camera::Vector3& sample : samples) {
    if (std::abs(sample.z()) < threshold) {
      result.emplace_back(sample);
    }
  }

  return result;
}

Camera::Matrix3 rotationMatrixFromEulers(const Camera::Vector3& euler, bool xyz = true) {
  Eigen::AngleAxis<Camera::Real> x(euler.x(), Camera::Vector3::UnitX());
  Eigen::AngleAxis<Camera::Real> y(euler.y(), Camera::Vector3::UnitY());
  Eigen::AngleAxis<Camera::Real> z(euler.z(), Camera::Vector3::UnitZ());
  return (xyz ? z * y * x : y * x * z).toRotationMatrix();
}

Camera::Rig
makeRigFromEulers(const Camera& model, const std::vector<Camera::Vector3>& eulers, bool xyz) {
  // Note/instruction:
  // - place camera at 0,0,1
  // - pointing in direction 0,0,1 in landscape
  // - i.e. sensor's wide direction is the 1,0,0 direction
  // - and up is 0,1,0
  // if xyz is false:
  // - rotate around the z axis by the third number
  // - then rotate around the x axis by the first number
  // - finally rotate around the y axis by the second number
  // - *please* note the order: z, x, y
  // if xyz is true:
  // - rotate around the x axis by the first number
  // - then rotate around the y axis by the second number
  // - finally rotate around the z axis by the third number
  Camera::Rig result;
  for (Camera::Vector3 euler : eulers) {
    euler *= M_PI / 180;
    Camera::Matrix3 xform = rotationMatrixFromEulers(euler, xyz);

    Camera camera = model;
    camera.setRotation(xform.col(2), xform.col(1), -xform.col(0));
    camera.position = model.position.norm() * camera.forward();
    camera.id = "cam" + std::to_string(result.size() + (FLAGS_one_based_indexing ? 1 : 0));
    result.emplace_back(camera);
  }
  return result;
}

Camera::Rig revolveRig(const Camera::Rig& rig, const std::vector<Camera::Vector3>& eulers) {
  Camera::Rig result;
  for (int frame = 0; frame < int(eulers.size()); ++frame) {
    const Camera::Vector3& euler = eulers[frame];
    Eigen::AngleAxis<Camera::Real> x(euler.x(), Camera::Vector3::UnitX());
    Eigen::AngleAxis<Camera::Real> y(euler.y(), Camera::Vector3::UnitY());
    Eigen::AngleAxis<Camera::Real> z(euler.z(), Camera::Vector3::UnitZ());
    Camera::Matrix3 xform = (z * y * x).toRotationMatrix();
    for (Camera camera : rig) {
      // transform rotation and position
      Camera::Vector3 forward = camera.forward();
      Camera::Vector3 up = camera.up();
      Camera::Vector3 right = camera.right();
      camera.setRotation(xform * forward, xform * up, xform * right);
      camera.position = xform * camera.position;
      if (eulers.size() > 1) {
        camera.id.append('_' + std::to_string(frame));
      }
      result.push_back(camera);
    }
  }
  return result;
}

Camera::Rig makeBallcam24(const Camera& model) {
  return makeRigFromEulers(
      model,
      {{22.998, -36.1543, 132.267},    {-2.89381, -156.601, 168.482},
       {-50.2907, -68.7384, 139.028},  {-80.2662, 172.721, 113.889},
       {57.5173, 87.6811, 161.596},    {6.46204, 162.32, 70.7419},
       {21.8577, 118.439, 114.195},    {77.4316, -95.0674, -100.379},
       {-20.2739, 41.1554, -135.466},  {-38.2009, 172.776, -171.825},
       {-0.841465, -110.909, 57.8619}, {-39.8563, -128.178, 46.3619},
       {-54.3882, 8.6561, -13.3586},   {24.3104, 51.5133, -20.0308},
       {35.7198, -82.6713, 160.228},   {-48.4447, 85.1941, 93.5637},
       {48.4425, 165.464, 19.7297},    {-3.41527, 84.0526, 56.5226},
       {-20.5666, -24.4286, 14.2745},  {35.8214, -139.006, -27.4138},
       {-8.22831, -69.3313, -46.6214}, {51.5282, 4.18718, -133.303},
       {6.61383, 8.24745, -72.7674},   {-22.4038, 126.995, 13.7087}},
      false);
}

Camera::Rig makeTetraTilted(const Camera& model) {
  // === arrangement 1280: mean = 2.14859, quality = 1.64213, stddev = 0.554116, min = 1, max = 3
  return makeRigFromEulers(
      model,
      {{-35.2644, 45, -65.1818},
       {-35.2644, -135, -137.834},
       {35.2644, -45, -45.0048},
       {35.2644, 135, -104.664}},
      false);
}

Camera::Rig makeCarbon0(const Camera& model) {
  // Lens: Izugar MKX22, 220 degree fov, 10 mm image diameter
  // Sensor: Sony IMX264, 2448 x 2048 x 3.45 um
  // === arrangement 789: mean = 3.22289, quality = 2.89455, stddev = 0.368871
  return makeRigFromEulers(
      model,
      {{-35.2644, 3.89537e-15, 112.232},
       {-35.2644, 120, -67.3096},
       {-35.2644, -120, 155.867},
       {35.2644, 180, 21.9328},
       {35.2644, -60, 14.0236},
       {35.2644, 60, 66.2737}},
      false);
}

Camera::Rig makeCarbon1(const Camera& model) {
  // Lens: Entaniya M12, 220 degree fov, 5.1 mm image diameter
  // Sensor: Sony IMX377, 4000 x 3000 x 1.55 um
  // === arrangement 449: mean = 3.93396, quality = 3.36985, stddev = 0.602308, min = 3, max = 5
  return makeRigFromEulers(
      model,
      {{-35.2644, 1.94768e-15, 133.504},
       {-35.2644, 120, -179.989},
       {-35.2644, -120, -134.51},
       {35.2644, 180, 89.7419},
       {35.2644, -60, 43.7899},
       {35.2644, 60, -45.1612}},
      false);
}

Camera::Rig makeTetra(const Camera& model, double angle) {
  Camera::Real a = angle == -1 ? acos(-1 / 3.0) * 180 / M_PI : angle;
  // note: default is almost 110
  // note: 140 is the angle at which everything below 3 is above the horizon
  return makeRigFromEulers(model, {{a, 0, 0}, {a, 0, 120}, {a, 0, -120}, {0, 0, 0}}, true);
}

Camera::Rig makeCube(const Camera& model, double angle) {
  Camera::Real a = (angle == -1 ? 90 : angle);
  return makeRigFromEulers(
      model, {{a, 0, 0}, {a, 0, 90}, {a, 0, 180}, {a, 0, 270}, {0, 0, 0}, {180, 0, 0}}, true);
}

Camera::Rig makeDiamond(const Camera& model, double angle) {
  Camera::Real a = (angle == -1 ? 90 : angle);
  return makeRigFromEulers(
      model, {{a, 0, 0}, {a, 0, 120}, {a, 0, 240}, {0, 0, 0}, {180, 0, 0}}, true);
}

Camera::Rig makeRing4(const Camera& model, double angle) {
  Camera::Real a = (angle == -1 ? 90 : angle);
  return makeRigFromEulers(model, {{a, 0, 0}, {a, 0, 90}, {a, 0, 180}, {a, 0, 270}}, true);
}

Camera::Rig
makeNamedArrangement(const std::string& name, const Camera& model, const double custom) {
  if (name == "ballcam24") {
    return makeBallcam24(model);
  } else if (name == "tetra") {
    return makeTetra(model, custom);
  } else if (name == "tetratilted") {
    return makeTetraTilted(model);
  } else if (name == "ring4") {
    return makeRing4(model, custom);
  } else if (name == "cube") {
    return makeCube(model, custom);
  } else if (name == "carbon0") {
    return makeCarbon0(model);
  } else if (name == "carbon1") {
    return makeCarbon1(model);
  }
  CHECK_EQ(name, "diamond") << "unknown arrangement";
  return makeDiamond(model, custom);
}

std::string getHistogram(const Eigen::VectorXd& coverages) {
  std::string result;
  int last = coverages.maxCoeff();
  for (int i = 0; i <= last; ++i) {
    int count = (coverages.array() == i).count();
    result += "h[" + std::to_string(i) + "] = " + std::to_string(count) + ", ";
  }
  return result;
}

static void
writeVertexObj(std::ofstream& file, const Camera::Vector3& color, const Camera::Vector3& position) {
  const Camera::Real kScale = 1000; // CAD wants mm, json is meters
  file << "v";
  for (int i = 0; i < int(position.size()); ++i) {
    file << " " << kScale * position[i];
  }
  for (int i = 0; i < int(color.size()); ++i) {
    file << " " << color[i];
  }
  file << "\n";
}

static void writeFaceObj(
    std::ofstream& file,
    const Camera::Vector3& color,
    const std::vector<Camera::Vector3>& positions) {
  // write each vertex
  for (int i = 0; i < int(positions.size()); ++i) {
    writeVertexObj(file, color, positions[i]);
  }
  // write the face in both orders to avoid backface culling
  for (int order = 0; order < 2; ++order) {
    file << "f";
    for (int i = 0; i < int(positions.size()); ++i) {
      int index = order ? -int(positions.size()) + i : -1 - i;
      file << " " << index;
    }
    file << "\n";
  }
}

static void writeArrowObj(
    std::ofstream& file,
    const Camera::Vector3& color,
    const Camera::Vector3& base,
    const Camera::Vector3& dir,
    const Camera::Vector3& t0,
    const Camera::Vector3& t1,
    const Camera::Real length = 0.01,
    const Camera::Real radius = 0.001) {
  writeFaceObj(file, color, {base + length * dir, base + radius * t0, base - radius * t0});
  writeFaceObj(file, color, {base + length * dir, base + radius * t1, base - radius * t1});
}

static void writeCameraObj(
    std::ofstream& file,
    const Camera::Vector3& p,
    const Camera::Vector3& f,
    const Camera::Vector3& r,
    const Camera::Vector3& u) {
  // a camera is represented by white forward, green right and blue up arrows
  writeArrowObj(file, {1, 1, 1}, p, f, r, u, 0.02);
  writeArrowObj(file, {0, 1, 0}, p, r, u, f);
  writeArrowObj(file, {0, 0, 1}, p, u, f, r);
}

void saveRigObj(const std::string& filename, const Camera::Rig& rig) {
  std::ofstream file(filename);
  for (int i = 0; i < int(rig.size()); ++i) {
    const Camera::Vector3& p = rig[i].position;
    const Camera::Vector3& f = rig[i].forward();
    const Camera::Vector3& r = rig[i].right();
    const Camera::Vector3& u = rig[i].up();
    writeCameraObj(file, p, f, r, u);
    for (int tri = 0; tri < i; ++tri) { // output the index as i triangles
      const double kSize = 0.002;
      const Camera::Vector3 v = p - kSize * tri * r;
      writeFaceObj(file, {1, 0, 0}, {v, v - kSize * r, v - kSize * u});
    }
  }
  // draw arrow from {0,0,-1} to {0,0,0}
  writeArrowObj(file, {1, 1, 0}, {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, 1.0, 0.01);
}

void saveCamera(const std::string& filename, const std::string& camId, const Camera::Rig& rig) {
  for (const Camera& cam : rig) {
    if (cam.id != camId) {
      continue;
    }
    const int kDimX = cam.resolution.x();
    const int kDimY = cam.resolution.y();
    std::ofstream file(filename, std::ios::binary);
    file << "P2" << std::endl;
    file << kDimX << " " << kDimY << std::endl;
    file << rig.size() << std::endl;
    for (int y = 0; y < kDimY; ++y) {
      for (int x = 0; x < kDimX; ++x) {
        const Camera::Vector2 p(x + 0.5, y + 0.5);
        int count = 0;
        if (!cam.isOutsideImageCircle(p)) {
          const Camera::Vector3 pWorld = cam.rig(p, FLAGS_overlap_distance);
          for (const Camera& camera : rig) {
            if (camera.sees(pWorld)) {
              ++count;
            }
          }
        }
        file << count << " ";
      }
      file << std::endl;
    }
  }
}

void saveEquirect(const std::string& filename, const Camera::Rig& rig) {
  const int kPixelsPerDegree = 5;
  const int kDimX = 360 * kPixelsPerDegree;
  const int kDimY = 180 * kPixelsPerDegree;
  std::ofstream file(filename, std::ios::binary);
  file << "P2" << std::endl;
  file << kDimX << " " << kDimY << std::endl;
  if (FLAGS_show_timing) {
    file << 256 << std::endl;
  } else {
    file << rig.size() << std::endl;
  }
  double holes = 0;
  double maxMin = 0;
  double aveMin = 0;
  for (int y = 0; y < kDimY; ++y) {
    // latitude goes from pi/2 down to -pi/2
    Camera::Real lat = M_PI / 2 - (y + 0.5) / kDimY * M_PI;
    for (int x = 0; x < kDimX; ++x) {
      // lon goes from -pi up to pi
      Camera::Real lon = -M_PI + (x + 0.5) / kDimX * 2 * M_PI;
      Camera::Vector3 direction(cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat));
      std::vector<float> timing(rig.size());
      int count = 0;
      for (const Camera& camera : rig) {
        Camera::Vector2 p;
        if (camera.sees(direction * FLAGS_overlap_distance, p)) {
          // Normalized timing distance assuming all the cameras'
          // pixel clocks are synced and single line activated/reset rolling
          // shutter.
          timing[count] = p.y() / camera.resolution.y();
          // Count this camera
          ++count;
        }
      }
      double minTimingDiff = 1.0;
      double maxTimingDiff = 0.0;
      for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
          const double timingDiff = std::abs(timing[i] - timing[j]);
          minTimingDiff = std::min(minTimingDiff, timingDiff);
          maxTimingDiff = std::max(maxTimingDiff, timingDiff);
        }
      }
      maxMin = std::max(maxMin, minTimingDiff);
      aveMin += minTimingDiff;

      if (FLAGS_show_timing) {
        const int timeWeightedCount = int((1.0 - minTimingDiff) * 255.0);
        file << timeWeightedCount << " ";
      } else {
        file << count << " ";
      }
      holes += (0 == count) ? 1 : 0;
    }
    file << std::endl;
  }
  const float kFrameRate = 60.0f; // 60 fps
  const float kFrameTime = 1000.0f / kFrameRate; // in milliseconds
  LOG(INFO) << "Holes found (in pixels) = " << holes;
  LOG(INFO) << "Max of min timing distance = " << kFrameTime * maxMin << "ms";
  LOG(INFO) << "Ave of min timing distance = " << kFrameTime * aveMin / (kDimX * kDimY) << "ms";
}

void saveCrossSection(const std::string& filename, const Camera::Rig& rig) {
  const int kDim = 400;
  std::ofstream file(filename, std::ios::binary);
  file << "P2" << std::endl;
  file << kDim << " " << kDim << std::endl;
  file << rig.size() << std::endl;
  for (int y = 0; y < kDim; ++y) {
    for (int x = 0; x < kDim; ++x) {
      // points are distributed within +/-0.5 * (kDim, kDim)
      Camera::Vector3 point(x + 0.5 - 0.5 * kDim, y + 0.5 - 0.5 * kDim, 0);
      int count = 0;
      for (const Camera& camera : rig) {
        if (camera.sees(point)) {
          ++count;
        }
      }
      file << count << " ";
    }
    file << std::endl;
  }
}

std::vector<Camera::Vector3> readVectorFile(const std::string& filename) {
  std::vector<Camera::Vector3> result;
  std::ifstream file(filename);
  for (std::string line; getline(file, line);) {
    if (line.find("===") == 0) { // ignore lines beginning with '==='
      continue;
    }
    std::istringstream s(line);
    Camera::Vector3 angles;
    s >> angles[0] >> angles[1] >> angles[2];
    CHECK(s) << "bad line <" << line << "> in file " << filename;
    result.push_back(angles);
  }
  return result;
}

int main(int argc, char* argv[]) {
  system_util::initDep(argc, argv, kUsageMessage);

  CHECK_NE(FLAGS_rig, "");

  // Read the cameras
  Camera::Rig rig = Camera::loadRig(FLAGS_rig);

  // Modify rig
  if (!FLAGS_rearrange.empty()) { // clone rig[0] into named configuration
    rig = makeNamedArrangement(FLAGS_rearrange, rig[0], FLAGS_custom);
  } else if (!FLAGS_eulers.empty()) { // clone first camera according to eulers
    rig = makeRigFromEulers(rig[0], readVectorFile(FLAGS_eulers), false);
  } else if (!FLAGS_revolve.empty()) {
    rig = revolveRig(rig, readVectorFile(FLAGS_revolve));
  } else if (FLAGS_perturb_cameras) {
    std::srand(FLAGS_perturb_seed);
    Camera::perturbCameras(
        rig,
        FLAGS_perturb_positions,
        FLAGS_perturb_rotations,
        FLAGS_perturb_principals,
        FLAGS_perturb_focals);
  }

  if (!FLAGS_rotate_cam_z.empty()) {
    const Camera& zCam = Camera::findCameraById(FLAGS_rotate_cam_z, rig);
    const Camera::Vector3 z = Eigen::Vector3d::UnitZ();
    Camera::Real angle = acos(zCam.position.dot(z));
    Camera::Vector3 axis = zCam.position.cross(z);
    axis.normalize();
    Eigen::AngleAxis<Camera::Real> alignCamToZ(angle, axis);
    Camera::Matrix3 rot = alignCamToZ.toRotationMatrix();
    for (Camera& camera : rig) {
      LOG(INFO) << camera.forward();
      LOG(INFO) << rot * camera.forward();
      camera.position = rot * camera.position;
      camera.setRotation(rot * camera.forward(), rot * camera.up(), rot * camera.right());
    }
  }

  if (FLAGS_z_is_up || FLAGS_z_is_down || !FLAGS_rotate.empty()) {
    Camera::Matrix3 m;
    if (FLAGS_z_is_up) {
      m << 1, 0, 0, 0, 0, -1, 0, 1, 0;
    } else if (FLAGS_z_is_down) {
      m << 1, 0, 0, 0, 0, 1, 0, -1, 0;
    } else {
      Camera::Vector3 euler;
      std::istringstream s(FLAGS_rotate);
      s >> euler.x() >> euler.y() >> euler.z();
      CHECK(s.eof() && !s.fail()) << "bad --rotate vector " << FLAGS_rotate;
      m = rotationMatrixFromEulers(euler);
    }
    for (Camera& camera : rig) {
      camera.position = m * camera.position;
      camera.setRotation(m * camera.forward(), m * camera.up(), m * camera.right());
    }
  }

  if (FLAGS_scale_rig != 1) {
    LOG(INFO) << "scaling rig by " << FLAGS_scale_rig;
    for (Camera& camera : rig) {
      camera.position *= FLAGS_scale_rig;
    }
  }

  if (FLAGS_radius > 0) {
    for (Camera& camera : rig) {
      camera.position = FLAGS_radius * camera.position.normalized();
    }
  }

  if (FLAGS_scale_resolution != 1) {
    for (Camera& camera : rig) {
      camera = camera.rescale(FLAGS_scale_resolution * camera.resolution);
    }
  }

  // Generate the directions that we want to test
  std::vector<Camera::Vector3> samples = getFibonacciUnits(FLAGS_sample_count);
  samples = discardPoles(samples, FLAGS_discard_poles * M_PI / 180);

  // Go through N distances from min_distance to kNearInfinity
  const int kN = 20;
  for (int i = 0; i < kN; ++i) {
    Camera::Real frac = i / Camera::Real(kN);
    Camera::Real distance = FLAGS_min_distance / (1 - frac);

    // Compute coverage for each sample
    Eigen::VectorXd coverages(samples.size());
    for (int i = 0; i < int(samples.size()); ++i) {
      int coverage = 0;
      for (const Camera& camera : rig) {
        if (camera.sees(distance * samples[i])) {
          ++coverage;
        }
      }
      coverages[i] = coverage;
    }

    // Report results
    const int minC = coverages.minCoeff();
    double quality = minC + (coverages.array() >= minC + 1).count() / double(coverages.size());
    std::cout << folly::format(
                     "distance: {:.2f} quality: {:.2f} samples: {} {}",
                     distance,
                     quality,
                     ssize(coverages),
                     getHistogram(coverages))
              << std::endl;
  }

  // Write the cameras
  if (FLAGS_output_rig != "") {
    Camera::saveRig(FLAGS_output_rig, rig, {"command line:", gflags::GetArgv()});
  }

  if (FLAGS_output_obj != "") {
    saveRigObj(FLAGS_output_obj, rig);
  }

  if (FLAGS_output_equirect != "") {
    saveEquirect(FLAGS_output_equirect, rig);
  }

  if (FLAGS_output_camera != "" && FLAGS_output_camera_id != "") {
    saveCamera(FLAGS_output_camera, FLAGS_output_camera_id, rig);
  }

  if (FLAGS_output_cross_section != "") {
    saveCrossSection(FLAGS_output_cross_section, rig);
  }

  return 0;
}
