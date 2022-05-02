/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "source/util/CvUtil.h"
#include "source/util/FilesystemUtil.h"
#include "source/util/ImageTypes.h"
#include "source/util/ImageUtil.h"
#include "source/util/SystemUtil.h"
#include "source/util/ThreadPool.h"

namespace fb360_dep {
namespace depth_estimation {

template <typename PixelType>
struct PyramidLevel {
  struct Src {
    cv::Mat_<PixelType> color;
    cv::Mat_<float> variance;
    cv::Mat_<bool> foregroundMask;
    cv::Mat_<bool> foregroundMaskDilated;
  };

  struct Dst {
    cv::Mat_<PixelType> color;
    cv::Mat_<float> disparity;
    cv::Mat_<bool> mismatchedDisparityMask;
    cv::Mat_<float> cost;
    cv::Mat_<float> confidence;
    cv::Mat_<int> overlap;

    cv::Mat_<bool> fovMask;
    cv::Mat_<bool> foregroundMask;
    cv::Mat_<float> backgroundDisparity;
  };

  struct Proj {
    cv::Mat_<cv::Vec2f> projWarp;
    cv::Mat_<cv::Vec2f> projWarpInv;
    cv::Mat_<PixelType> projColor;
    cv::Mat_<PixelType> projColorBias;
  };

  // if first frame is 000039, frameIdx = 0, frameName = 000039
  int frameIdx;
  std::string frameName;
  int numFrames;

  int level;
  int numLevels;
  cv::Size sizeLevel;
  std::map<int, cv::Size> levelSizes;

  Camera::Rig rigSrc;
  Camera::Rig rigDst;
  std::vector<int> dst2srcIdxs;

 public:
  int findDstIdx(const std::string& dstId) const {
    for (int i = 0; i < int(rigDst.size()); ++i) {
      if (rigDst[i].id == dstId) {
        return i;
      }
    }
    CHECK(false) << "Cannot find dst idx for ID: " << dstId;
  }

  std::vector<Src> srcs;
  std::vector<Dst> dsts;
  std::vector<Proj> projs;

  filesystem::path srcColorsPath; // in case we want to load full-size images
  int widthFullSize;
  int heightFullSize;
  float varNoiseFullSize;
  float varNoiseFloor;
  float varHighThresh;
  bool hasForegroundMasks;

  filesystem::path outputDir;

  int numThreads;

  PyramidLevel(
      const int frameIdxIn,
      const std::string& frameNameIn,
      const int numFramesIn,
      const int levelIn,
      const int numLevelsIn,
      const std::map<int, cv::Size>& levelSizesIn,
      const Camera::Rig& rigSrcIn,
      const Camera::Rig& rigDstIn,
      const std::vector<int>& dst2srcIdxsIn,
      const std::vector<cv::Mat_<PixelType>>& srcColorsIn,
      const std::vector<cv::Mat_<bool>>& srcForegroundMasksIn,
      const std::vector<cv::Mat_<bool>>& dstFovMasksIn,
      const std::vector<cv::Mat_<float>>& dstBackgroundDisparitiesIn,
      const int widthFullSizeIn,
      const int heightFullSizeIn,
      const std::string& color,
      const float varNoiseFloor,
      const float varHighThresh,
      const bool useForegroundMasks,
      const std::string& outputRoot,
      const int threads)
      : frameIdx(frameIdxIn),
        frameName(frameNameIn),
        numFrames(numFramesIn),
        level(levelIn),
        numLevels(numLevelsIn),
        levelSizes(levelSizesIn),
        rigSrc(rigSrcIn),
        rigDst(rigDstIn),
        dst2srcIdxs(dst2srcIdxsIn),
        srcColorsPath(color),
        widthFullSize(widthFullSizeIn),
        heightFullSize(heightFullSizeIn),
        varNoiseFullSize(varNoiseFloor),
        varHighThresh(varHighThresh),
        hasForegroundMasks(useForegroundMasks),
        outputDir(outputRoot),
        numThreads(threads) {
    sizeLevel = levelSizes[level];

    checkParams();

    const int numSrcs = rigSrc.size();
    const int numDsts = rigDst.size();

    CHECK_EQ(srcColorsIn.size(), numSrcs);
    CHECK_EQ(srcForegroundMasksIn.size(), numSrcs);
    CHECK_EQ(dstFovMasksIn.size(), numDsts);
    CHECK_EQ(dstBackgroundDisparitiesIn.size(), numDsts);

    for (int srcIdx = 0; srcIdx < numSrcs; ++srcIdx) {
      Src src;
      src.color = srcColorsIn[srcIdx];
      src.foregroundMask = srcForegroundMasksIn[srcIdx];

      // Dilated mask is used to find pixels 8-connected with the foreground
      src.foregroundMaskDilated = cv_util::dilate(src.foregroundMask);
      srcs.push_back(src);
    }

    for (int dstIdx = 0; dstIdx < numDsts; ++dstIdx) {
      Dst dst;
      dst.color = srcColor(dst2srcIdxs[dstIdx]); // OpenCV: reference count
      dst.foregroundMask = srcForegroundMask(dst2srcIdxs[dstIdx]);
      dst.fovMask = dstFovMasksIn[dstIdx];
      dst.backgroundDisparity = dstBackgroundDisparitiesIn[dstIdx];
      dsts.push_back(dst);
    }

    projs.resize(numDsts * numSrcs);

    createLevelMats();
    computeVariances();
  }

  void checkParams() {
    CHECK_GT(numLevels, 0);
    CHECK_GE(frameIdx, 0);
    CHECK_GT(rigSrc.size(), 0);
    CHECK_GT(rigDst.size(), 0);

    // Assuming all dst cameras have the same resolution
    for (int iDst = 1; iDst < int(rigDst.size()); ++iDst) {
      CHECK_EQ(rigDst[iDst].resolution, rigDst[0].resolution);
    }

    // Assuming all src cameras have the same resolution
    for (int iSrc = 1; iSrc < int(rigSrc.size()); ++iSrc) {
      CHECK_EQ(rigSrc[iSrc].resolution, rigSrc[0].resolution);
    }
  }

  template <typename T>
  void createIfEmpty(cv::Mat_<T>& mat, const cv::Size& size, const T& val) {
    if (mat.empty()) {
      mat.create(size);
      mat.setTo(val);
    } else {
      CHECK_EQ(mat.size(), size);
    }
  }

  template <typename T>
  void createOrReleaseMat(
      cv::Mat_<T>& mat,
      const bool createOrRelease, // true = create, false = release
      const cv::Size& size,
      const T& val) {
    if (createOrRelease) {
      createIfEmpty(mat, size, val);
    } else {
      mat.release();
    }
  }

  // createOrRelease: true = create, false = release
  void createOrReleaseLevelMats(const bool createOrRelease) {
    const float zeroF = 0.0f;
    const bool zeroM = false;
    CHECK_EQ(srcs.size(), rigSrc.size());
    for (int srcIdx = 0; srcIdx < int(rigSrc.size()); ++srcIdx) {
      createOrReleaseMat(srcVariance(srcIdx), createOrRelease, sizeLevel, zeroF);
    }

    CHECK_EQ(dsts.size(), rigDst.size());
    for (int dstIdx = 0; dstIdx < int(rigDst.size()); ++dstIdx) {
      createOrReleaseMat(dstDisparity(dstIdx), createOrRelease, sizeLevel, zeroF);
      createOrReleaseMat(dstMismatchedDisparityMask(dstIdx), createOrRelease, sizeLevel, zeroM);
      createOrReleaseMat(dstFovMask(dstIdx), createOrRelease, sizeLevel, zeroM);
      createOrReleaseMat(dstCost(dstIdx), createOrRelease, sizeLevel, zeroF);
      createOrReleaseMat(dstConfidence(dstIdx), createOrRelease, sizeLevel, zeroF);
    }
  }

  void createLevelMats() {
    createOrReleaseLevelMats(true);
  }

  void computeVariances() {
    // Variance noise is multiplied by the square of the scale
    const float scale = float(sizeLevel.width) / heightFullSize;
    const float scaleVar = math_util::square(scale);
    varNoiseFloor = std::max(varNoiseFullSize * scaleVar, depth_estimation::kMinVar);

    ThreadPool threadPool(numThreads);
    for (int srcIdx = 0; srcIdx < int(rigSrc.size()); ++srcIdx) {
      threadPool.spawn([&, srcIdx] {
        // Variance will be used during cost computation, random proposals and
        // disparity mismatch handling
        srcVariance(srcIdx) = computeImageVariance(srcColor(srcIdx));
      });
    }
    threadPool.join();
  }

  cv::Mat_<PixelType>& srcColor(const int srcId) {
    return srcs[srcId].color;
  }

  const cv::Mat_<PixelType>& srcColor(const int srcId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->srcColor(srcId);
  }

  cv::Mat_<PixelType>& dstColor(const int dstId) {
    return dsts[dstId].color;
  }

  const cv::Mat_<PixelType>& dstColor(const int dstId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstColor(dstId);
  }

  cv::Mat_<float>& dstDisparity(const int dstId) {
    return dsts[dstId].disparity;
  }

  const cv::Mat_<float>& dstDisparity(const int dstId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstDisparity(dstId);
  }

  cv::Mat_<bool>& dstMismatchedDisparityMask(const int dstId) {
    return dsts[dstId].mismatchedDisparityMask;
  }

  const cv::Mat_<bool>& dstMismatchedDisparityMask(const int dstId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstMismatchedDisparityMask(dstId);
  }

  cv::Mat_<bool>& dstFovMask(const int dstId) {
    return dsts[dstId].fovMask;
  }

  const cv::Mat_<bool>& dstFovMask(const int dstId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstFovMask(dstId);
  }

  cv::Mat_<float>& dstCost(const int dstId) {
    return dsts[dstId].cost;
  }

  const cv::Mat_<float>& dstCost(const int dstId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstCost(dstId);
  }

  cv::Mat_<float>& dstBackgroundDisparity(const int dstId) {
    return dsts[dstId].backgroundDisparity;
  }

  const cv::Mat_<float>& dstBackgroundDisparity(const int dstId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstBackgroundDisparity(dstId);
  }

  cv::Mat_<bool>& srcForegroundMask(const int srcId) {
    return srcs[srcId].foregroundMask;
  }

  const cv::Mat_<bool>& srcForegroundMask(const int srcId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->srcForegroundMask(srcId);
  }

  cv::Mat_<bool>& srcForegroundMaskDilated(const int srcId) {
    return srcs[srcId].foregroundMaskDilated;
  }

  const cv::Mat_<bool>& srcForegroundMaskDilated(const int srcId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->srcForegroundMaskDilated(srcId);
  }

  cv::Mat_<bool>& dstForegroundMask(const int dstId) {
    return dsts[dstId].foregroundMask;
  }

  const cv::Mat_<bool>& dstForegroundMask(const int dstId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstForegroundMask(dstId);
  }

  cv::Mat_<float>& srcVariance(const int srcId) {
    return srcs[srcId].variance;
  }

  const cv::Mat_<float>& srcVariance(const int srcId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->srcVariance(srcId);
  }

  cv::Mat_<float>& dstVariance(const int dstId) {
    return srcs[dst2srcIdxs[dstId]].variance;
  }

  const cv::Mat_<float>& dstVariance(const int dstId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstVariance(dstId);
  }

  cv::Mat_<float>& dstConfidence(const int dstId) {
    return dsts[dstId].confidence;
  }

  const cv::Mat_<float>& dstConfidence(const int dstId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstConfidence(dstId);
  }

  cv::Mat_<int>& dstOverlap(const int dstId) {
    return dsts[dstId].overlap;
  }

  const cv::Mat_<int>& dstOverlap(const int dstId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstOverlap(dstId);
  }

  // Index of src when we have all srcs for each dst
  int getDstSrcIdx(const int dstId, const int srcId) const {
    return dstId * rigSrc.size() + srcId;
  }

  int getDstSrcIdx(const int dstId) const {
    return getDstSrcIdx(dstId, dst2srcIdxs[dstId]);
  }

  cv::Mat_<cv::Vec2f>& dstProjWarp(const int dstId, const int srcId) {
    return projs[getDstSrcIdx(dstId, srcId)].projWarp;
  }

  const cv::Mat_<cv::Vec2f>& dstProjWarp(const int dstId, const int srcId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstProjWarp(dstId, srcId);
  }

  cv::Mat_<cv::Vec2f>& dstProjWarpInv(const int dstId, const int srcId) {
    return projs[getDstSrcIdx(dstId, srcId)].projWarpInv;
  }

  const cv::Mat_<cv::Vec2f>& dstProjWarpInv(const int dstId, const int srcId) const {
    return const_cast<PyramidLevel*>(this)->dstProjWarpInv(dstId, srcId);
  }

  cv::Mat_<PixelType>& dstProjColor(const int dstId, const int srcId) {
    return projs[getDstSrcIdx(dstId, srcId)].projColor;
  }

  const cv::Mat_<PixelType>& dstProjColor(const int dstId, const int srcId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstProjColor(dstId, srcId);
  }

  cv::Mat_<PixelType>& dstProjColorBias(const int dstId, const int srcId) {
    return projs[getDstSrcIdx(dstId, srcId)].projColorBias;
  }

  const cv::Mat_<PixelType>& dstProjColorBias(const int dstId, const int srcId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstProjColorBias(dstId, srcId);
  }

  cv::Mat_<PixelType>& dstProjColor(const int dstId) {
    return projs[getDstSrcIdx(dstId)].projColor;
  }

  const cv::Mat_<PixelType>& dstProjColor(const int dstId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstProjColor(dstId);
  }

  cv::Mat_<PixelType>& dstProjColorBias(const int dstId) {
    return projs[getDstSrcIdx(dstId)].projColorBias;
  }

  const cv::Mat_<PixelType>& dstProjColorBias(const int dstId) const {
    return const_cast<PyramidLevel<PixelType>*>(this)->dstProjColorBias(dstId);
  }

  void saveDstImage(const int dstIdx, const ImageType imageType, const float scale = 1.0f) {
    cv::Mat dstImage;
    switch (imageType) {
      case ImageType::disparity_levels:
        dstImage = dstDisparity(dstIdx);
        break;
      case ImageType::cost:
        dstImage = dstCost(dstIdx);
        break;
      case ImageType::confidence:
        dstImage = dstConfidence(dstIdx);
        break;
      case ImageType::mismatches:
        dstImage = overlayMismatchedDstDisparityMask(dstIdx);
        break;
      default:
        CHECK(false) << "unexpected image type " << imageTypes[(int)imageType];
    }

    if (dstImage.empty()) {
      return;
    }

    cv::Mat scaledDstImage = dstImage * scale;
    if (imageType == ImageType::disparity_levels) {
      // note: disparity values will be clamped to the [0,1] range (which get scaled
      // to [0, 2^16 - 1]) and nans will be converted to zero
      scaledDstImage = cv_util::convertTo<uint16_t>(scaledDstImage);
    }

    const std::string& dstId = rigDst[dstIdx].id;
    const filesystem::path fn =
        depth_estimation::genFilename(outputDir, imageType, level, dstId, frameName, "png");
    cv_util::imwriteExceptionOnFail(fn, scaledDstImage);
  }

  void saveDebugImages() {
    for (int dstIdx = 0; dstIdx < int(rigDst.size()); ++dstIdx) {
      saveDstImage(dstIdx, ImageType::disparity_levels, 1.0f);
      saveDstImage(dstIdx, ImageType::cost, depth_estimation::kScaleCostPlot);
      saveDstImage(dstIdx, ImageType::confidence, depth_estimation::kScaleConfidencePlot);
      saveDstImage(dstIdx, ImageType::mismatches, depth_estimation::kScaleDisparityPlot);
    }
  }

  cv::Mat_<cv::Vec4f> overlayMismatchedDstDisparityMask(const int dstIdx) {
    const cv::Mat_<bool>& mask = dstMismatchedDisparityMask(dstIdx);
    const cv::Mat_<float>& disparity = dstDisparity(dstIdx);
    const cv::Mat_<bool>& fov = dstFovMask(dstIdx);

    cv::Mat_<cv::Vec4f> maskedDisparity(disparity.size(), NAN);

    for (int x = 0; x < maskedDisparity.cols; ++x) {
      for (int y = 0; y < maskedDisparity.rows; ++y) {
        if (!fov(y, x)) {
          continue;
        }
        if (mask(y, x)) {
          maskedDisparity(y, x) = cv::Vec4f(0.f, 0.f, 1.f, 1.f); // red
        } else {
          float d = disparity(y, x);
          maskedDisparity(y, x) = cv::Vec4f(d, d, d, 1.f);
        }
      }
    }

    return maskedDisparity;
  }

  void saveResults(const std::string& outputFormatsStr) {
    std::vector<std::string> outputFormatsVec;
    folly::split(",", outputFormatsStr, outputFormatsVec);
    std::unordered_set<std::string> outputFormats(outputFormatsVec.begin(), outputFormatsVec.end());

    const bool saveExr = (outputFormats.find("exr") != outputFormats.end());
    const bool savePfm = true; // always save PFM
    const bool savePng = (outputFormats.find("png") != outputFormats.end());

    if (!(saveExr || savePfm || savePng)) {
      return;
    }
    ThreadPool threadPool(numThreads);
    for (int dstIdx = 0; dstIdx < int(rigDst.size()); ++dstIdx) {
      threadPool.spawn([&, dstIdx] {
        const cv::Mat_<float>& disp = dstDisparity(dstIdx);
        const ImageType imageType = ImageType::disparity_levels;

        std::map<std::string, bool> types = {{"exr", saveExr}, {"pfm", savePfm}, {"png", savePng}};
        for (std::pair<std::string, bool> type : types) {
          if (!type.second) {
            continue;
          }
          const std::string& t = type.first;
          const std::string& dstId = rigDst[dstIdx].id;
          const filesystem::path fn =
              depth_estimation::genFilename(outputDir, imageType, level, dstId, frameName, t);
          boost::filesystem::create_directories(fn.parent_path());
          if (t == "exr") {
            cv_util::imwriteExceptionOnFail(fn, disp);
          } else if (t == "pfm") {
            cv_util::writeCvMat32FC1ToPFM(fn, disp);
          } else if (t == "png") {
            const cv::Mat_<uint16_t> disp16 = cv_util::convertTo<uint16_t>(disp);
            cv_util::imwriteExceptionOnFail(fn, disp16);
          } else {
            CHECK(false) << "Invalid type: " << t;
          }
        }
      });
    }
    threadPool.join();
  }
};
} // namespace depth_estimation
} // namespace fb360_dep
