/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <glog/logging.h>

#include <folly/json.h>
#include <folly/lang/Bits.h>

#include "source/isp/ColorspaceConversion.h"
#include "source/isp/Filter.h"
#include "source/isp/MonotonicTable.h"
#include "source/util/CvUtil.h"
#include "source/util/MathUtil.h"
#include "source/util/SystemUtil.h"

namespace fb360_dep {

enum class DemosaicFilter : unsigned int {
  BILINEAR = 0,
  FREQUENCY,
  EDGE_AWARE,
  CHROMA_SUPRESSED_BILINEAR,
  LAST
};

const int kToneCurveLutSize = 4096;

class CameraIsp {
 protected:
  std::string bayerPattern;
  bool isLittleEndian;
  bool isPlanar;
  std::string planeOrder;
  bool isRowMajor;
  int sensorWidth;
  int sensorHeight;
  int sensorBitsPerPixel;

  std::vector<cv::Point3f> compandingLut;
  cv::Point3f blackLevel;
  cv::Point3f clampMin;
  cv::Point3f clampMax;
  std::vector<cv::Point3f> vignetteRollOffH;
  std::vector<cv::Point3f> vignetteRollOffV;

  int stuckPixelThreshold;
  float stuckPixelDarknessThreshold;
  int stuckPixelRadius;
  cv::Point3f whiteBalanceGain;
  cv::Mat_<float> ccm; // 3x3
  cv::Mat_<float> compositeCCM;
  float saturation;
  cv::Point3f gamma;
  cv::Point3f lowKeyBoost;
  cv::Point3f highKeyBoost;
  float contrast;
  cv::Point3f sharpening;
  float sharpeningSupport;
  float noiseCore;
  cv::Mat_<float> rawImage;
  bool redBayerPixel[2][2];
  bool greenBayerPixel[2][2];
  cv::Mat_<cv::Vec3f> demosaicedImage;
  uint32_t filters;
  DemosaicFilter demosaicFilter;
  int resize;
  bool disableToneCurve;
  bool toneCurveEnabled;
  std::vector<cv::Vec3f> toneCurveLut;
  math_util::BezierCurve<float, cv::Vec3f> vignetteCurveH;
  math_util::BezierCurve<float, cv::Vec3f> vignetteCurveV;

  const int width;
  const int height;
  const int maxDimension;
  const float maxD; // max diagonal distance
  const float sqrtMaxD; // max diagonal distance

  void demosaicBilinearFilter(cv::Mat_<float>& r, cv::Mat_<float>& g, cv::Mat_<float>& b) const {
    for (int i = 0; i < height; ++i) {
      const int i_1 = math_util::reflect(i - 1, height);
      const int i1 = math_util::reflect(i + 1, height);

      const bool redGreenRow =
          (redPixel(i, 0) && greenPixel(i, 1)) || (redPixel(i, 1) && greenPixel(i, 0));

      for (int j = 0; j < width; ++j) {
        const int j_1 = math_util::reflect(j - 1, width);
        const int j1 = math_util::reflect(j + 1, width);

        if (redPixel(i, j)) {
          g(i, j) = cv_util::bilerp(g(i_1, j), g(i1, j), g(i, j_1), g(i, j1), 0.5f, 0.5f);

          b(i, j) = cv_util::bilerp(b(i_1, j_1), b(i1, j_1), b(i_1, j1), b(i1, j1), 0.5f, 0.5f);

        } else if (greenPixel(i, j)) {
          if (redGreenRow) {
            b(i, j) = (b(i_1, j) + b(i1, j)) / 2.0f;

            r(i, j) = (r(i, j_1) + r(i, j1)) / 2.0f;
          } else {
            r(i, j) = (r(i_1, j) + r(i1, j)) / 2.0f;

            b(i, j) = (b(i, j_1) + b(i, j1)) / 2.0f;
          }
        } else {
          g(i, j) = cv_util::bilerp(g(i_1, j), g(i1, j), g(i, j_1), g(i, j1), 0.5f, 0.5f);

          r(i, j) = cv_util::bilerp(r(i_1, j_1), r(i1, j_1), r(i_1, j1), r(i1, j1), 0.5f, 0.5f);
        }
      }
    }
  }

  void demosaicFrequencyFilter(cv::Mat_<float>& r, cv::Mat_<float>& g, cv::Mat_<float>& b) const {
    // Green/luma 4-th order Butterworth lp filter
    const math_util::Butterworth dFilter(0.0f, 2.0f, width + height, 1.0f, 4.0);
    // Chrome cross over filter
    const math_util::Butterworth dcFilter(0.0f, 2.0f, width + height, 1.0f, 2.0f);

    //  Do a per pixel filtering in DCT space
    const int h2 = r.rows;
    const int w2 = r.cols;
    for (int i = 0; i < h2; ++i) {
      const float y = float(i) / float(h2 - 1);
      for (int j = 0; j < w2; ++j) {
        const float x = float(j) / float(w2 - 1);
        // Diagonal distance and half
        static const float kDScale = 1.2f;
        const float d = (x + y) * kDScale;
        const float kSharpen = d / 2.5f + 1.0f;
        const float gGain = 2.0f * dFilter(d) * kSharpen;
        const float rbGain = 4.0f * dFilter(d);
        g(i, j) *= gGain;

        const float kCrossoverCutoff = 3.0f;
        const float d2 = d * 2 * kCrossoverCutoff;

        // Cross over blend value
        const float alpha = dcFilter(d2);
        r(i, j) = math_util::lerp(g(i, j), r(i, j) * rbGain, alpha);
        b(i, j) = math_util::lerp(g(i, j), b(i, j) * rbGain, alpha);
      }
    }
  }

  void demosaicEdgeAware(cv::Mat_<float>& red, cv::Mat_<float>& green, cv::Mat_<float>& blue)
      const {
    // Horizontal and vertical green values
    cv::Mat_<float> gV(height, width);
    cv::Mat_<float> gH(height, width);

    // And their first and second order derivatives
    cv::Mat_<float> dV(height, width);
    cv::Mat_<float> dH(height, width);

    // Compute green gradients
    for (int i = 0; i < height; ++i) {
      const int i_1 = math_util::reflect(i - 1, height);
      const int i1 = math_util::reflect(i + 1, height);
      const int i_2 = math_util::reflect(i - 2, height);
      const int i2 = math_util::reflect(i + 2, height);

      for (int j = 0; j < width; ++j) {
        const int j_1 = math_util::reflect(j - 1, width);
        const int j1 = math_util::reflect(j + 1, width);
        const int j_2 = math_util::reflect(j - 2, width);
        const int j2 = math_util::reflect(j + 2, width);
        if (greenPixel(i, j)) {
          gV(i, j) = green(i, j);
          gH(i, j) = green(i, j);

          dV(i, j) =
              (fabsf(green(i2, j) - green(i, j)) + fabsf(green(i, j) - green(i_2, j))) / 2.0f;

          dH(i, j) =
              (fabsf(green(i, j2) - green(i, j)) + fabsf(green(i, j) - green(i, j_2))) / 2.0f;
        } else {
          gV(i, j) = (green(i_1, j) + green(i1, j)) / 2.0f;
          gH(i, j) = (green(i, j_1) + green(i, j1)) / 2.0f;
          dV(i, j) = (fabsf(green(i_1, j) - green(i1, j))) / 2.0f;
          dH(i, j) = (fabsf(green(i, j_1) - green(i, j1))) / 2.0f;

          cv::Mat_<float>& ch = redPixel(i, j) ? red : blue;
          gV(i, j) += (2.0f * ch(i, j) - ch(i_2, j) - ch(i2, j)) / 4.0f;
          gH(i, j) += (2.0f * ch(i, j) - ch(i, j_2) - ch(i, j2)) / 4.0f;
          dV(i, j) += fabsf(-2.0f * ch(i, j) + ch(i_2, j) + ch(i2, j)) / 2.0f;
          dH(i, j) += fabsf(-2.0f * ch(i, j) + ch(i, j_2) + ch(i, j2)) / 2.0f;
        }
      }
    }
    const int w = 4;
    const int diameter = 2 * w + 1;
    const int diameterSquared = math_util::square(diameter);

    for (int i = 0; i < height; ++i) {
      for (int j = 0; j < width; ++j) {
        // Homogenity test
        int hCount = 0;
        for (int l = -w; l <= w; ++l) {
          const int il = math_util::reflect(i + l, height);
          for (int k = -w; k <= w; ++k) {
            const int jk = math_util::reflect(j + k, width);
            hCount += (dH(il, jk) <= dV(il, jk));
          }
        }
        green(i, j) = math_util::lerp(gV(i, j), gH(i, j), float(hCount) / diameterSquared);
      }
    }
    demosaicChromaSuppressed(red, green, blue);
  }

  void demosaicGreenBilinear(cv::Mat_<float>& red, cv::Mat_<float>& green, cv::Mat_<float>& blue)
      const {
    for (int i = 0; i < height; ++i) {
      const int i_1 = math_util::reflect(i - 1, height);
      const int i1 = math_util::reflect(i + 1, height);

      for (int j = 0; j < width; ++j) {
        const int j_1 = math_util::reflect(j - 1, width);
        const int j1 = math_util::reflect(j + 1, width);

        if (redPixel(i, j)) {
          green(i, j) =
              cv_util::bilerp(green(i_1, j), green(i1, j), green(i, j_1), green(i, j1), 0.5f, 0.5f);

        } else if (!greenPixel(i, j)) {
          green(i, j) =
              cv_util::bilerp(green(i_1, j), green(i1, j), green(i, j_1), green(i, j1), 0.5f, 0.5f);
        }
      }
    }
    demosaicChromaSuppressed(red, green, blue);
  }

  void demosaicChromaSuppressed(cv::Mat_<float>& red, cv::Mat_<float>& green, cv::Mat_<float>& blue)
      const {
    // compute r-b
    cv::Mat_<float> redMinusGreen(height, width);
    cv::Mat_<float> blueMinusGreen(height, width);

    for (int i = 0; i < height; ++i) {
      for (int j = 0; j < width; ++j) {
        if (redPixel(i, j)) {
          redMinusGreen(i, j) = red(i, j) - green(i, j);
        } else if (!greenPixel(i, j)) {
          blueMinusGreen(i, j) = blue(i, j) - green(i, j);
        }
      }
    }
    // Now use a constant hue based red/blue bilinear interpolation
    for (int i = 0; i < height; ++i) {
      const int i_1 = math_util::reflect(i - 1, height);
      const int i1 = math_util::reflect(i + 1, height);
      const int i_2 = math_util::reflect(i - 2, height);
      const int i2 = math_util::reflect(i + 2, height);

      const bool redGreenRow =
          (redPixel(i, 0) && greenPixel(i, 1)) || (redPixel(i, 1) && greenPixel(i, 0));

      for (int j = 0; j < width; ++j) {
        const int j_1 = math_util::reflect(j - 1, width);
        const int j1 = math_util::reflect(j + 1, width);
        const int j_2 = math_util::reflect(j - 2, width);
        const int j2 = math_util::reflect(j + 2, width);

        if (redPixel(i, j)) {
          blue(i, j) = (blueMinusGreen(i_1, j_1) + blueMinusGreen(i1, j_1) +
                        blueMinusGreen(i_1, j1) + blueMinusGreen(i1, j1)) /
                  4.0f +
              green(i, j);

          red(i, j) = (redMinusGreen(i, j) + redMinusGreen(i_2, j) + redMinusGreen(i2, j) +
                       redMinusGreen(i, j_2) + redMinusGreen(i, j2)) /
                  5.0f +
              green(i, j);
        } else if (greenPixel(i, j)) {
          cv::Mat_<float>& diffCh1 = redGreenRow ? blueMinusGreen : redMinusGreen;
          cv::Mat_<float>& diffCh2 = redGreenRow ? redMinusGreen : blueMinusGreen;

          cv::Mat_<float>& ch1 = redGreenRow ? blue : red;
          cv::Mat_<float>& ch2 = redGreenRow ? red : blue;

          ch1(i, j) = (diffCh1(i_1, j_2) + diffCh1(i_1, j) + diffCh1(i_1, j2) + diffCh1(i1, j_2) +
                       diffCh1(i1, j2) + diffCh1(i1, j2)) /
                  6.0f +
              green(i, j);

          ch2(i, j) = (diffCh2(i_2, j_1) + diffCh2(i, j_1) + diffCh2(i2, j_1) + diffCh2(i_2, j1) +
                       diffCh2(i, j1) + diffCh2(i2, j1)) /
                  6.0f +
              green(i, j);
        } else {
          red(i, j) = (redMinusGreen(i_1, j_1) + redMinusGreen(i1, j_1) + redMinusGreen(i_1, j1) +
                       redMinusGreen(i1, j1)) /
                  4.0f +
              green(i, j);

          blue(i, j) = (blueMinusGreen(i, j) + blueMinusGreen(i_2, j) + blueMinusGreen(i2, j) +
                        blueMinusGreen(i, j_2) + blueMinusGreen(i, j2)) /
                  5.0f +
              green(i, j);
        }
      }
    }
  }

  template <class T>
  void resizeInput(const cv::Mat_<T>& inputImage) {
    int bitsPerPixel = 8 * sizeof(T);
    int maxPixelValue = (1 << bitsPerPixel) - 1;
    const float areaRecip = 1.0f / (maxPixelValue * float(math_util::square(resize)));
    const int r = resize > 1 ? 2 : 1;

    for (int i = 0; i < height; ++i) {
      for (int j = 0; j < width; ++j) {
        float sum = 0.0f;
        for (int k = 0; k < resize; ++k) {
          const int ip = i * resize + k * 2;
          const int ipp = math_util::reflect(ip + (i % r), inputImage.rows);
          for (int l = 0; l < resize; ++l) {
            const int jp = j * resize + l * 2;
            const int jpp = math_util::reflect(jp + (j % r), inputImage.cols);
            sum += float(inputImage(ipp, jpp));
          }
        }
        rawImage(i, j) = sum * areaRecip;
      }
    }
  }

  std::array<int, 4> getPlaneOrderToBayerOrder() const {
    bool foundFirstG = false;
    std::array<int, 4> map;
    for (int i = 0; i < 4; ++i) {
      map[i] = foundFirstG ? planeOrder.rfind(bayerPattern[i]) : planeOrder.find(bayerPattern[i]);
      foundFirstG |= bayerPattern[i] == 'G';
    }
    return map;
  }

  // Used to build up the low and high key parts of the tone curve
  inline float bezier(float a, float b, float c, float d, float t) {
    // Four point DeCasteljau's Algorithm
    return math_util::lerp(
        math_util::lerp(math_util::lerp(a, b, t), math_util::lerp(b, c, t), t),
        math_util::lerp(math_util::lerp(b, c, t), math_util::lerp(c, d, t), t),
        t);
  }

  inline float highKey(float highKeyBoost, float x) {
    const float a = 0.5f;
    const float b = math_util::clamp(0.6666f, 0.0f, 1.0f);
    const float c = math_util::clamp(0.8333f + highKeyBoost, 0.0f, 1.0f);
    const float d = 1.0f;
    return x > 0.5f ? bezier(a, b, c, d, (x - 0.5f) * 2.0f) : 0;
  }

  inline float lowKey(float lowKeyBoost, float x) {
    const float a = 0.0f;
    const float b = math_util::clamp(0.1666f + lowKeyBoost, 0.0f, 1.0f);
    const float c = math_util::clamp(0.3333f, 0.0f, 1.0f);
    const float d = 0.5f;
    return x <= 0.5f ? bezier(a, b, c, d, x * 2.0f) : 0;
  }

  // Build the composite tone curve map from [0,1]^3 to [0,1]^3
  void buildToneCurveLut() {
    toneCurveLut.clear();
    const float dx = 1.0f / float(kToneCurveLutSize - 1);

    // Contrast angle constants
    const float angle = M_PI * 0.25f * contrast;
    const float slope = tanf(angle);
    const float bias = 0.5f * (1.0f - slope);

    for (int i = 0; i < kToneCurveLutSize; ++i) {
      const float x = dx * i;
      if (!toneCurveEnabled) {
        // Just a linear ramp ==> no-op
        const float y = x;
        toneCurveLut.push_back(cv::Vec3f(y, y, y));
      } else {
        // Apply gamma correction
        float r = powf(x, gamma.x);
        float g = powf(x, gamma.y);
        float b = powf(x, gamma.z);
        // Then low/high key boost
        r = lowKey(lowKeyBoost.x, r) + highKey(highKeyBoost.x, r);
        g = lowKey(lowKeyBoost.y, g) + highKey(highKeyBoost.y, g);
        b = lowKey(lowKeyBoost.z, b) + highKey(highKeyBoost.z, b);

        // Then contrast
        r = math_util::clamp((slope * r + bias), 0.0f, 1.0f);
        g = math_util::clamp((slope * g + bias), 0.0f, 1.0f);
        b = math_util::clamp((slope * b + bias), 0.0f, 1.0f);

        // Place it in the table.
        toneCurveLut.push_back(cv::Vec3f(r, g, b));
      }
    }
  }

  void getPoint(folly::dynamic& f, cv::Point3f& p) const {
    if (f.isNull()) {
      return;
    }

    p = cv::Point3f();
    if (f[0] != nullptr) {
      p.x = f[0].getDouble();
    } else {
      VLOG(1) << "Bad or missing x point coordinate." << std::endl;
    }

    if (f[1] != nullptr) {
      p.y = f[1].getDouble();
    } else {
      VLOG(1) << "Bad or missing y point coordinate." << std::endl;
    }

    if (f[2] != nullptr) {
      p.z = f[2].getDouble();
    } else {
      VLOG(1) << "Bad or missing z point coordinate." << std::endl;
    }
  }

  void getCoordList(folly::dynamic& list, std::vector<cv::Point3f>& coords) const {
    if (list.isNull()) {
      return;
    }

    coords = std::vector<cv::Point3f>();
    for (auto l : list) {
      cv::Point3f p;
      getPoint(l, p);
      coords.push_back(p);
    }
  }

  void getMatrix(folly::dynamic& list, cv::Mat_<float>& m) const {
    if (list.isNull()) {
      return;
    }
    int rows = 0;
    int cols = 0;

    for (auto i : list) {
      ++rows;
      cols = 0;
      for (auto j : i) {
        ++cols;
      }
    }

    m = cv::Mat_<float>(rows, cols);

    for (int i = 0; i < rows; ++i) {
      for (int j = 0; j < cols; ++j) {
        m(i, j) = list[i][j].asDouble();
      }
    }
  }

 public:
  CameraIsp(const std::string jsonInput)
      : demosaicFilter(DemosaicFilter::EDGE_AWARE),
        resize(1),
        toneCurveEnabled(true),
        width(0),
        height(0),
        maxDimension(0),
        maxD(0),
        sqrtMaxD(0) {
    // Set the default values and override them from the json file
    compandingLut.push_back(cv::Point3f(0.0f, 0.0f, 0.0f));
    compandingLut.push_back(cv::Point3f(1.0f, 1.0f, 1.0f));
    blackLevel = cv::Point3f(0.0f, 0.0f, 0.0f);
    clampMin = cv::Point3f(0.0f, 0.0f, 0.0f);
    clampMax = cv::Point3f(1.0f, 1.0f, 1.0f);
    stuckPixelThreshold = 0;
    stuckPixelDarknessThreshold = 0;
    stuckPixelRadius = 0;
    vignetteRollOffH.push_back(cv::Vec3f(1.0f, 1.0f, 1.0f));
    vignetteRollOffV.push_back(cv::Vec3f(1.0f, 1.0f, 1.0f));
    whiteBalanceGain = cv::Point3f(1.0f, 1.0f, 1.0f);
    ccm = cv::Mat::eye(3, 3, CV_32F);
    saturation = 1.0f;
    contrast = 1.0f;
    sharpening = cv::Point3f(0.0f, 0.0f, 0.0f);
    sharpeningSupport = 10.0f / 2048.0f; // Approx filter support is 10 pixels
    noiseCore = 1000.0f;
    gamma = cv::Point3f(1.0f, 1.0f, 1.0f);
    lowKeyBoost = cv::Point3f(0.0f, 0.0f, 0.0f);
    highKeyBoost = cv::Point3f(0.0f, 0.0f, 0.0f);

    bayerPattern = "GBRG";
    sensorWidth = width;
    sensorHeight = height;
    sensorBitsPerPixel = 16;
    isPlanar = false;
    planeOrder = "";
    isLittleEndian = false;
    isRowMajor = true;

    folly::dynamic config = folly::parseJson(jsonInput);
    if (config["CameraIsp"] != nullptr) {
      folly::dynamic cameraConfig = config["CameraIsp"];

      sensorBitsPerPixel = cameraConfig.getDefault("bitsPerPixel", sensorBitsPerPixel).asInt();
      sensorWidth = cameraConfig.getDefault("width", sensorWidth).asInt();
      sensorHeight = cameraConfig.getDefault("height", sensorHeight).asInt();
      isLittleEndian = cameraConfig.getDefault("isLittleEndian", isLittleEndian).asBool();
      isRowMajor = cameraConfig.getDefault("isRowMajor", isRowMajor).asBool();

      bayerPattern = cameraConfig.getDefault("bayerPattern", bayerPattern).asString();
      boost::to_upper(bayerPattern);
      CHECK_EQ(bayerPattern.size(), 4);
      planeOrder = cameraConfig.getDefault("planeOrder", planeOrder).asString();
      boost::to_upper(planeOrder);
      isPlanar = !planeOrder.empty();
      CHECK(!isPlanar || planeOrder.size() == 4);

      getCoordList(cameraConfig["compandingLut"], compandingLut);
      getPoint(cameraConfig["blackLevel"], blackLevel);
      getPoint(cameraConfig["clampMin"], clampMin);
      getPoint(cameraConfig["clampMax"], clampMax);
      stuckPixelThreshold =
          cameraConfig.getDefault("stuckPixelThreshold", stuckPixelThreshold).asInt();
      CHECK_GE(stuckPixelThreshold, 0);
      stuckPixelDarknessThreshold =
          cameraConfig.getDefault("stuckPixelDarknessThreshold", stuckPixelDarknessThreshold)
              .asDouble();
      stuckPixelRadius = cameraConfig.getDefault("stuckPixelRadius", stuckPixelRadius).asInt();
      getCoordList(cameraConfig["vignetteRollOffH"], vignetteRollOffH);
      getCoordList(cameraConfig["vignetteRollOffV"], vignetteRollOffV);
      getPoint(cameraConfig["whiteBalanceGain"], whiteBalanceGain);
      getMatrix(cameraConfig["ccm"], ccm);
      saturation = cameraConfig.getDefault("saturation", saturation).asDouble();
      getPoint(cameraConfig["gamma"], gamma);
      getPoint(cameraConfig["lowKeyBoost"], lowKeyBoost);
      getPoint(cameraConfig["highKeyBoost"], highKeyBoost);
      contrast = cameraConfig.getDefault("contrast", contrast).asDouble();
      getPoint(cameraConfig["sharpening"], sharpening);
      sharpeningSupport =
          cameraConfig.getDefault("sharpeningSupport", sharpeningSupport).asDouble();
      noiseCore = cameraConfig.getDefault("noiseCore", noiseCore).asDouble();
    } else {
      VLOG(1) << "Missing \"CameraIsp\" in config; using default values.\n";
    }

    setup();
  }

  virtual ~CameraIsp() {}

  void setup() {
    // Build the bayer pattern tables
    if (bayerPattern.find("RGGB") != std::string::npos) {
      filters = 0x94949494;
      redBayerPixel[0][0] = true;
      redBayerPixel[0][1] = false;
      redBayerPixel[1][0] = false;
      redBayerPixel[1][1] = false;

      greenBayerPixel[0][0] = false;
      greenBayerPixel[0][1] = true;
      greenBayerPixel[1][0] = true;
      greenBayerPixel[1][1] = false;
    } else if (bayerPattern.find("GRBG") != std::string::npos) {
      filters = 0x61616161;
      redBayerPixel[0][0] = false;
      redBayerPixel[0][1] = true;
      redBayerPixel[1][0] = false;
      redBayerPixel[1][1] = false;

      greenBayerPixel[0][0] = true;
      greenBayerPixel[0][1] = false;
      greenBayerPixel[1][0] = false;
      greenBayerPixel[1][1] = true;
    } else if (bayerPattern.find("GBRG") != std::string::npos) {
      filters = 0x49494949;
      redBayerPixel[0][0] = false;
      redBayerPixel[0][1] = false;
      redBayerPixel[1][0] = true;
      redBayerPixel[1][1] = false;

      greenBayerPixel[0][0] = true;
      greenBayerPixel[0][1] = false;
      greenBayerPixel[1][0] = false;
      greenBayerPixel[1][1] = true;
    } else if (bayerPattern.find("BGGR") != std::string::npos) {
      filters = 0x16161616;
      redBayerPixel[0][0] = false;
      redBayerPixel[0][1] = false;
      redBayerPixel[1][0] = false;
      redBayerPixel[1][1] = true;

      greenBayerPixel[0][0] = false;
      greenBayerPixel[0][1] = true;
      greenBayerPixel[1][0] = true;
      greenBayerPixel[1][1] = false;
    }

    vignetteCurveH.clearPoints();
    for (auto p : vignetteRollOffH) {
      vignetteCurveH.addPoint(p);
    }

    vignetteCurveV.clearPoints();
    for (auto p : vignetteRollOffV) {
      vignetteCurveV.addPoint(p);
    }

    // If saturation is unit this satMat will be the identity matrix.
    cv::Mat_<float> satMat = cv::Mat::zeros(3, 3, CV_32F);
    satMat(0, 0) = 1.0f;
    satMat(1, 1) = saturation;
    satMat(2, 2) = saturation;

    // Move into yuv scale by the saturation and move back
    satMat = color::yuv2rgb * satMat * color::rgb2yuv;

    transpose(ccm, compositeCCM);
    compositeCCM *= satMat;

    // The stage following the CCM maps tone curve lut to 12bits so we
    // scale the pixel by the lut size here once instead of doing it
    // for every pixel.
    compositeCCM *= float(kToneCurveLutSize - 1);

    // Build the tone curve table
    buildToneCurveLut();
  }

  // Helper functions
  inline bool redPixel(const int i, const int j) const {
    return redBayerPixel[i % 2][j % 2];
  }

  inline bool greenPixel(const int i, const int j) const {
    return greenBayerPixel[i % 2][j % 2];
  }

  inline bool bluePixel(const int i, const int j) const {
    return !(greenBayerPixel[i % 2][j % 2] || redBayerPixel[i % 2][j % 2]);
  }

  inline int getChannelNumber(const int i, const int j) {
    return redPixel(i, j) ? 0 : greenPixel(i, j) ? 1 : 2;
  }

  inline cv::Vec3f curveHAtPixel(const int x) {
    return vignetteCurveH(float(x) / float(maxDimension));
  }

  inline cv::Vec3f curveVAtPixel(const int x) {
    return vignetteCurveV(float(x) / float(maxDimension));
  }

  void dumpConfigFile(const std::string configFileName) {
    std::ofstream ofs(configFileName.c_str(), std::ios::out);
    if (ofs) {
      ofs.precision(3);
      ofs << std::fixed;
      ofs << "{\n";
      ofs << "   \"CameraIsp\" : {\n";
      ofs << "        \"serial\" : 0,\n";
      ofs << "        \"name\" : \"RED Helium\",\n";
      ofs << "        \"compandingLut\" :  [";
      for (int i = 0; i < int(compandingLut.size()); ++i) {
        if (i > 0) {
          ofs << "                            ";
        }
        ofs << "[" << compandingLut[i].x << ", " << compandingLut[i].y << ", " << compandingLut[i].z
            << "]";
        if (i < int(compandingLut.size()) - 1) {
          ofs << ",\n";
        }
      }
      ofs << "],\n";
      ofs << "        \"blackLevel\" : [" << blackLevel.x << ", " << blackLevel.y << ", "
          << blackLevel.z << "],\n";
      ofs << "        \"clampMin\" : [" << clampMin.x << ", " << clampMin.y << ", " << clampMin.z
          << "],\n";
      ofs << "        \"clampMax\" : [" << clampMax.x << ", " << clampMax.y << ", " << clampMax.z
          << "],\n";
      ofs << "        \"vignetteRollOffH\" :  [";
      for (int i = 0; i < int(vignetteRollOffH.size()); ++i) {
        if (i > 0) {
          ofs << "                               ";
        }
        ofs << "[" << vignetteRollOffH[i].x << ", " << vignetteRollOffH[i].y << ", "
            << vignetteRollOffH[i].z << "]";
        if (i < int(vignetteRollOffH.size()) - 1) {
          ofs << ",\n";
        }
      }
      ofs << "],\n";
      ofs << "        \"vignetteRollOffV\" :  [";
      for (int i = 0; i < int(vignetteRollOffV.size()); ++i) {
        if (i > 0) {
          ofs << "                               ";
        }
        ofs << "[" << vignetteRollOffV[i].x << ", " << vignetteRollOffV[i].y << ", "
            << vignetteRollOffV[i].z << "]";
        if (i < int(vignetteRollOffV.size()) - 1) {
          ofs << ",\n";
        }
      }
      ofs << "],\n";
      ofs << "        \"whiteBalanceGain\" : [" << whiteBalanceGain.x << "," << whiteBalanceGain.y
          << "," << whiteBalanceGain.z << "],\n";
      ofs << "        \"stuckPixelThreshold\" : " << stuckPixelThreshold << ",\n";
      ofs << "        \"stuckPixelDarknessThreshold\" : " << stuckPixelDarknessThreshold << ",\n";
      ofs << "        \"stuckPixelRadius\" : " << stuckPixelRadius << ",\n";
      ofs.precision(5);
      ofs << std::fixed;
      ofs << "        \"ccm\" : [[" << ccm(0, 0) << ", " << ccm(0, 1) << ", " << ccm(0, 2)
          << "],\n";
      ofs << "                 [" << ccm(1, 0) << ", " << ccm(1, 1) << ", " << ccm(1, 2) << "],\n";
      ofs << "                 [" << ccm(2, 0) << ", " << ccm(2, 1) << ", " << ccm(2, 2) << "]],\n";
      ofs.precision(3);
      ofs << std::fixed;
      ofs << "        \"sharpening\" : [" << sharpening.x << ", " << sharpening.y << ", "
          << sharpening.z << "],\n";
      ofs << "        \"saturation\" : " << saturation << ",\n";
      ofs << "        \"contrast\" : " << contrast << ",\n";
      ofs << "        \"lowKeyBoost\" : [" << lowKeyBoost.x << ", " << lowKeyBoost.y << ", "
          << lowKeyBoost.z << "],\n";
      ofs << "        \"highKeyBoost\" : [" << highKeyBoost.x << ", " << highKeyBoost.y << ", "
          << highKeyBoost.z << "],\n";
      ofs << "        \"gamma\" : [" << gamma.x << ", " << gamma.y << ", " << gamma.z << "],\n";
      ofs << "        \"bayerPattern\" : \"" << bayerPattern << "\",\n";
      ofs << "        \"isLittleEndian\" : " << isLittleEndian << ",\n";
      ofs << "        \"isPlanar\" : " << isPlanar << ",\n";
      ofs << "        \"isRowMajor\" : " << isRowMajor << ",\n";
      ofs << "        \"width\" : " << sensorWidth << ",\n";
      ofs << "        \"height\" : " << sensorHeight << ",\n";
      ofs << "        \"bitsPerPixel\" : " << sensorBitsPerPixel << ",\n";
      ofs << "        \"planeOrder\" : \"" << planeOrder << "\"\n";
      ofs << "    }\n";
      ofs << "}\n";
    } else {
      CHECK(false) << "unable to open output ISP config file: " << configFileName;
    }
  }

  /* Load sensorWidth x sensorHeight image from raw data, interleaving, byte swapping, and
     re-indexing if necessary for host and OpenCV compatibility. */
  template <typename T>
  void loadImageFromSensor(const std::vector<T>& inputImage) {
    // - sanity check config
    CHECK_NE(sensorWidth, 0);
    CHECK_NE(sensorHeight, 0);
    CHECK_EQ(sensorWidth % 2, 0);
    CHECK_EQ(sensorHeight % 2, 0);
    CHECK_EQ(inputImage.size(), sensorWidth * sensorHeight);
    CHECK_EQ(sensorBitsPerPixel, 8 * sizeof(T));

    cv::Mat_<T> intermediateImage(sensorHeight, sensorWidth);

    // - load directly if there's nothing to do
    if (!isPlanar && isLittleEndian == folly::kIsLittleEndian && isRowMajor) {
      memcpy(intermediateImage.data, inputImage.data(), sensorWidth * sensorHeight * sizeof(T));
      loadImage(intermediateImage);
      return;
    }

    // - setup problem dimensions and channel mappings
    int planeWidth = sensorWidth / 2;
    int planeHeight = sensorHeight / 2;
    int numPixelsPerPlane = planeWidth * planeHeight;
    std::array<int, 4> inputChannelOrderToBayerOrder = {
        {0, 1, 2, 3}}; // no-op for interleaved input
    if (isPlanar) {
      inputChannelOrderToBayerOrder = getPlaneOrderToBayerOrder();
    }

    // - inner loop helpers to byte swap, interleave, and re-index as needed
    //   - converts from linear index to a representation-independent (channel, row, column)
    //     pixel index
    auto pixelIndexFromInputLinearIndex = [=, this](int linearIndex) {
      int channel, row, col;
      if (isPlanar) {
        channel = linearIndex / numPixelsPerPlane;
        int linearIndexInPlane = linearIndex - channel * numPixelsPerPlane;
        std::tie(row, col) =
            math_util::linearToMatrixIndex(linearIndexInPlane, planeHeight, planeWidth, isRowMajor);
      } else {
        std::tie(row, col) =
            math_util::linearToMatrixIndex(linearIndex, sensorHeight, sensorWidth, isRowMajor);
        // Bayer pattern is always specified in row major, so compute the channel index accordingly
        channel = math_util::matrixToLinearIndex(std::make_tuple(row % 2, col % 2), 2, 2);
        row /= 2;
        col /= 2;
      }
      return std::make_tuple(channel, row, col);
    };

    //   - converts representation-independent (channel, row, column) pixel index to (row, column)
    //     matrix index in interleaved image
    auto outputInterleavedRawIndexFromPixelIndex = [=](std::tuple<int, int, int> pixelIndex) {
      int channel, row, col, bayerRow, bayerCol;
      std::tie(channel, row, col) = pixelIndex;
      channel = inputChannelOrderToBayerOrder[channel];
      std::tie(bayerRow, bayerCol) = math_util::linearToMatrixIndex(channel, 2, 2);
      return std::make_pair(2 * row + bayerRow, 2 * col + bayerCol);
    };

    auto byteSwap = isLittleEndian ? folly::Endian::little<T> : folly::Endian::big<T>;

    // - reorder and load image
    for (int k = 0; k < sensorWidth * sensorHeight; ++k) {
      auto pixelIndex = pixelIndexFromInputLinearIndex(k);
      auto rawIndex = outputInterleavedRawIndexFromPixelIndex(pixelIndex);
      intermediateImage(std::get<0>(rawIndex), std::get<1>(rawIndex)) = byteSwap(inputImage[k]);
    }
    loadImage(intermediateImage);
  }

  /* Load a native byte order, interleaved, and arbitrarily sized OpenCV raw image */
  virtual void loadImage(const cv::Mat& inputImage) {
    setDimensions(inputImage.cols, inputImage.rows);
    rawImage = cv::Mat::zeros(height, width, CV_32F);

    // Copy and convert to float
    uint8_t depth = inputImage.type() & CV_MAT_DEPTH_MASK;
    // Match the input bits per pixel overriding what is in the config file.
    if (depth == CV_8U) {
      resizeInput<uint8_t>(inputImage);
    } else if (depth == CV_16U) {
      resizeInput<uint16_t>(inputImage);
    } else {
      CHECK(false) << "input is larger that 16 bits per pixel";
    }
  }

  template <typename T>
  cv::Mat_<T> getRawImage() const {
    CHECK(!rawImage.empty());
    return cv_util::convertTo<T>(rawImage);
  }

  void setDemosaicedImage(const cv::Mat_<cv::Vec3f>& demosaicedImage) {
    this->demosaicedImage = demosaicedImage;
  }

  cv::Mat_<cv::Vec3f> getDemosaicedImage() const {
    return demosaicedImage;
  }

  void addBlackLevelOffset(const int offset) {
    blackLevel.x += offset;
    blackLevel.y += offset;
    blackLevel.z += offset;
  }

  void setBlackLevel(const cv::Point3f& blackLevel) {
    this->blackLevel = blackLevel;
  }

  cv::Point3f getBlackLevel() const {
    return blackLevel;
  }

  void setClampMin(const cv::Point3f& clampMin) {
    this->clampMin = clampMin;
  }

  cv::Point3f getClampMin() const {
    return clampMin;
  }

  void setClampMax(const cv::Point3f& clampMax) {
    this->clampMax = clampMax;
  }

  cv::Point3f getClampMax() const {
    return clampMax;
  }

  void setVignetteRollOffH(const std::vector<cv::Point3f>& vignetteRollOffH) {
    this->vignetteRollOffH = vignetteRollOffH;
  }

  std::vector<cv::Point3f> getVignetteRollOffH() const {
    return vignetteRollOffH;
  }

  void setVignetteRollOffV(const std::vector<cv::Point3f>& vignetteRollOffV) {
    this->vignetteRollOffV = vignetteRollOffV;
  }

  std::vector<cv::Point3f> getVignetteRollOffV() const {
    return vignetteRollOffV;
  }

  void setCCM(const cv::Mat_<float>& ccm) {
    this->ccm = ccm;
  }

  cv::Mat_<float> getCCM() const {
    return ccm;
  }

  uint32_t getFilters() const {
    return filters;
  }

  void setWhiteBalance(const cv::Point3f whiteBalanceGain) {
    this->whiteBalanceGain = whiteBalanceGain;
  }

  cv::Point3f getWhiteBalanceGain() const {
    return whiteBalanceGain;
  }

  void setGamma(const cv::Point3f gamma) {
    this->gamma = gamma;
  }

  cv::Point3f getGamma() const {
    return gamma;
  }

  void setToneCurveEnabled(bool toneCurveEnabled) {
    if (this->toneCurveEnabled != toneCurveEnabled) {
      this->toneCurveEnabled = toneCurveEnabled;
      buildToneCurveLut();
    }
  }

  void setDemosaicFilter(DemosaicFilter demosaicFilter) {
    CHECK(DemosaicFilter::BILINEAR <= demosaicFilter && demosaicFilter < DemosaicFilter::LAST)
        << "expecting Demosaic filter in [0," << int(DemosaicFilter::LAST) - 1 << "]";
    this->demosaicFilter = demosaicFilter;
  }

  void setResize(const int resize) {
    CHECK(resize == 1 || resize == 2 || resize == 4 || resize == 8)
        << "expecting a resize value of 1, 2, 4, or 8. got " << resize;
    this->resize = resize;
  }

  // Set the white point of the camera/scene
  void whiteBalance(const bool clampOutput = true) {
    for (int i = 0; i < height; ++i) {
      for (int j = 0; j < width; ++j) {
        if (redPixel(i, j)) {
          rawImage(i, j) *= whiteBalanceGain.x;
        } else if (greenPixel(i, j)) {
          rawImage(i, j) *= whiteBalanceGain.y;
        } else {
          rawImage(i, j) *= whiteBalanceGain.z;
        }

        if (clampOutput) {
          rawImage(i, j) = math_util::clamp(rawImage(i, j), 0.0f, 1.0f);
        }
      }
    }
  }

  void removeStuckPixels() {
    if (stuckPixelRadius > 0) {
      struct Pval {
        float val;
        int i;
        int j;

        Pval(float v) : val(v) {}
        Pval(const Pval& other) = default;

        bool operator<(const Pval& p) const {
          return val < p.val;
        }

        Pval& operator=(const Pval& p) {
          val = p.val;
          i = p.i;
          j = p.j;
          return *this;
        }
      };

      std::vector<Pval> region;

      for (int i = 0; i < height; ++i) {
        // Traverse boustrophedonically
        const bool evenScanLine = (i % 2) == 0;
        const int jStart = evenScanLine ? 0 : width - 1;
        const int jEnd = evenScanLine ? width - 1 : 0;
        const int jStep = evenScanLine ? 1 : -1;

        for (int j = jStart; j != jEnd; j += jStep) {
          const bool thisPixelRed = redPixel(i, j);
          const bool thisPixelGreen = greenPixel(i, j);
          const bool thisPixelBlue = bluePixel(i, j);

          region.clear();

          float mean = 0.0f;
          for (int y = -stuckPixelRadius; y <= stuckPixelRadius; y++) {
            const int ip = math_util::reflect(i + y, height);
            for (int x = -stuckPixelRadius; x <= stuckPixelRadius; x++) {
              const int jp = math_util::reflect(j + x, width);
              Pval p(rawImage(ip, jp));
              p.i = ip;
              p.j = jp;
              if (redPixel(ip, jp) && thisPixelRed) {
                mean += p.val;
                region.push_back(p);
              } else if (greenPixel(ip, jp) && thisPixelGreen) {
                mean += p.val;
                region.push_back(p);
              } else if (bluePixel(ip, jp) && thisPixelBlue) {
                mean += p.val;
                region.push_back(p);
              }
            }
          }
          mean /= float(region.size());

          // Only deal with dark regions
          if (mean < stuckPixelDarknessThreshold) {
            sort(region.begin(), region.end());

            // See if the middle pixel is an outlier
            for (int k = int(region.size()) - 1; k >= int(region.size()) - stuckPixelThreshold;
                 --k) {
              if (region[k].i == i && region[k].j == j) {
                // Replace the pixel pixel with the median of the region.
                rawImage(i, j) = region[region.size() / 2].val;
                goto l0;
              }
            }
          }
        l0:
          continue;
        }
      }
    }
  }

  void blackLevelAdjust() {
    const float br = blackLevel.x;
    const float bg = blackLevel.y;
    const float bb = blackLevel.z;
    const float sr = 1.0f / (1.0f - br);
    const float sg = 1.0f / (1.0f - bg);
    const float sb = 1.0f / (1.0f - bb);
    for (int i = 0; i < height; ++i) {
      for (int j = 0; j < width; j++) {
        if (rawImage(i, j) < 1.0f) {
          if (redPixel(i, j)) {
            rawImage(i, j) = (rawImage(i, j) - br) * sr;
          } else if (greenPixel(i, j)) {
            rawImage(i, j) = (rawImage(i, j) - bg) * sg;
          } else {
            rawImage(i, j) = (rawImage(i, j) - bb) * sb;
          }
        }
      }
    }
  }

  void clampAndStretch() {
    for (int i = 0; i < height; ++i) {
      for (int j = 0; j < width; j++) {
        const float clampMinVal =
            redPixel(i, j) ? clampMin.x : (greenPixel(i, j) ? clampMin.y : clampMin.z);
        const float clampMaxVal =
            redPixel(i, j) ? clampMax.x : (greenPixel(i, j) ? clampMax.y : clampMax.z);
        const float v = math_util::clamp(rawImage(i, j), clampMinVal, clampMaxVal);
        rawImage(i, j) = (v - clampMinVal) / (clampMaxVal - clampMinVal);
      }
    }
  }

  void antiVignette() {
    for (int i = 0; i < height; ++i) {
      const cv::Vec3f vV = curveVAtPixel(i);
      for (int j = 0; j < width; j++) {
        const cv::Vec3f vH = curveHAtPixel(j);
        int ch = getChannelNumber(i, j);
        rawImage(i, j) *= vH[ch] * vV[ch];
      }
    }
  }

  uint32_t nextPowerOf2(const uint32_t i) {
    uint32_t p = 1;
    while (i > p) {
      p <<= 1;
    }
    return p;
  }

  void demosaic() {
    uint32_t h2 = demosaicFilter == DemosaicFilter::FREQUENCY ? nextPowerOf2(height) : height;
    uint32_t w2 = demosaicFilter == DemosaicFilter::FREQUENCY ? nextPowerOf2(width) : width;
    cv::Mat_<float> r(h2, w2, 0.0f);
    cv::Mat_<float> g(h2, w2, 0.0f);
    cv::Mat_<float> b(h2, w2, 0.0f);

    // Break out each plane into a separate image so we can demosaicFilter
    // them separately and then recombine them.
    for (int i = 0; i < height; ++i) {
      for (int j = 0; j < width; j++) {
        if (redPixel(i, j)) {
          r(i, j) = rawImage(i, j);
        } else if (greenPixel(i, j)) {
          g(i, j) = rawImage(i, j);
        } else {
          b(i, j) = rawImage(i, j);
        }
      }
    }

    if (demosaicFilter == DemosaicFilter::FREQUENCY) {
      // Move into the frequency domain
      std::thread t1([&] { dct(r, r); });
      std::thread t2([&] { dct(g, g); });
      std::thread t3([&] { dct(b, b); });

      t1.join();
      t2.join();
      t3.join();

      // Filter including sharpnning in the DCT domain
      demosaicFrequencyFilter(r, g, b);
#undef DEBUG_DCT
#ifndef DEBUG_DCT
      // Move back into the spatial domain
      std::thread t4([&] { idct(r, r); });
      std::thread t5([&] { idct(g, g); });
      std::thread t6([&] { idct(b, b); });

      t4.join();
      t5.join();
      t6.join();
#endif
    } else if (demosaicFilter == DemosaicFilter::BILINEAR) {
      demosaicBilinearFilter(r, g, b);
    } else if (demosaicFilter == DemosaicFilter::CHROMA_SUPRESSED_BILINEAR) {
      demosaicGreenBilinear(r, g, b);
    } else {
      demosaicEdgeAware(r, g, b);
    }

    demosaicedImage = cv::Mat::zeros(height, width, CV_32F);
    for (int i = 0; i < height; ++i) {
      for (int j = 0; j < width; j++) {
        demosaicedImage(i, j)[0] = r(i, j);
        demosaicedImage(i, j)[1] = g(i, j);
        demosaicedImage(i, j)[2] = b(i, j);
      }
    }
  }

  void colorCorrect() {
    const float kToneCurveLutRange = kToneCurveLutSize - 1;
    for (int i = 0; i < height; ++i) {
      for (int j = 0; j < width; j++) {
        cv::Vec3f p = demosaicedImage(i, j);
#ifdef DEBUG_DCT
        cv::Vec3f v(
            logf(math_util::square(p[0]) + 1.0f) * 255.0f,
            logf(math_util::square(p[1]) + 1.0f) * 255.0f,
            logf(math_util::square(p[2]) + 1.0f) * 255.0f);
#else
        cv::Vec3f v(
            toneCurveLut[math_util::clamp(
                compositeCCM(0, 0) * p[0] + compositeCCM(0, 1) * p[1] + compositeCCM(0, 2) * p[2],
                0.0f,
                kToneCurveLutRange)][0],
            toneCurveLut[math_util::clamp(
                compositeCCM(1, 0) * p[0] + compositeCCM(1, 1) * p[1] + compositeCCM(1, 2) * p[2],
                0.0f,
                kToneCurveLutRange)][1],
            toneCurveLut[math_util::clamp(
                compositeCCM(2, 0) * p[0] + compositeCCM(2, 1) * p[1] + compositeCCM(2, 2) * p[2],
                0.0f,
                kToneCurveLutRange)][2]);
#endif
        demosaicedImage(i, j) = v;
      }
    }
  }

  void sharpen() {
    if (sharpening.x != 0.0 && sharpening.y != 0.0 && sharpening.z != 0.0) {
      cv::Mat_<cv::Vec3f> lowPass(height, width);
      const isp::ReflectBoundary<int> reflectB;
      const float maxVal = 1.0f;
      isp::iirLowPass<isp::ReflectBoundary<int>, isp::ReflectBoundary<int>, cv::Vec3f>(
          demosaicedImage, sharpeningSupport, lowPass, reflectB, reflectB, maxVal);
      isp::sharpenWithIirLowPass<cv::Vec3f>(
          demosaicedImage,
          lowPass,
          1.0f + sharpening.x,
          1.0f + sharpening.y,
          1.0f + sharpening.z,
          noiseCore,
          maxVal);
    }
  }

 protected:
  // Replacable pipeline
  virtual void executePipeline(const bool swizzle) {
    // Apply the pipeline
    blackLevelAdjust();
    antiVignette();
    whiteBalance();
    clampAndStretch();
    removeStuckPixels();
    demosaic();
    colorCorrect();
    sharpen();
  }

  void setDimensions(int inputWidth, int inputHeight) {
    const_cast<int&>(width) = inputWidth / resize;
    const_cast<int&>(height) = inputHeight / resize;
    const_cast<int&>(maxDimension) = std::max(width, height);
    const_cast<float&>(maxD) = math_util::square(width) + math_util::square(height);
    const_cast<float&>(sqrtMaxD) = sqrt(maxD);
  }

 public:
  int getOutputWidth() const {
    return width;
  }

  int getOutputHeight() const {
    return height;
  }

  int getSensorWidth() const {
    return sensorWidth;
  }

  int getSensorHeight() const {
    return sensorHeight;
  }

  int getSensorBitsPerPixel() const {
    return sensorBitsPerPixel;
  }

  template <typename T>
  cv::Mat_<cv::Vec<T, 3>> getImage(const bool swizzle = true) {
    cv::Mat_<cv::Vec<T, 3>> outputImage(getOutputHeight(), getOutputWidth());
    getImage(outputImage, swizzle);
    return outputImage;
  }

  template <typename T>
  void getImage(cv::Mat_<cv::Vec<T, 3>>& outputImage, const bool swizzle = true) {
    CHECK_EQ(getOutputWidth(), outputImage.cols);
    CHECK_EQ(getOutputHeight(), outputImage.rows);

    executePipeline(swizzle);

    int outputBitsPerPixel = 8 * sizeof(T);
    float scale = (1 << outputBitsPerPixel) - 1;

    // Copy and convert to byte swizzling to BGR
    const int c0 = swizzle ? 2 : 0;
    const int c1 = swizzle ? 1 : 1;
    const int c2 = swizzle ? 0 : 2;
    for (int i = 0; i < height; ++i) {
      for (int j = 0; j < width; j++) {
        cv::Vec3f dp = scale * demosaicedImage(i, j);
        outputImage(i, j)[c0] = dp[0];
        outputImage(i, j)[c1] = dp[1];
        outputImage(i, j)[c2] = dp[2];
      }
    }
  }
}; // class CameraIsp
}; // namespace fb360_dep
