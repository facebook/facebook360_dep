/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/format.h>

#include "source/depth_estimation/DerpUtil.h"

#include <queue>
#include <vector>

namespace fb360_dep::depth_estimation {

class PointDistance {
 public:
  std::array<int, 2> m_point;
  double m_distance;
  PointDistance(std::array<int, 2> point, double distance) {
    m_point = point;
    m_distance = distance;
  }
};

bool operator>(const PointDistance& ptDist1, const PointDistance& ptDist2) {
  return ptDist1.m_distance > ptDist2.m_distance;
}

bool operator<(const PointDistance& ptDist1, const PointDistance& ptDist2) {
  return ptDist1.m_distance < ptDist2.m_distance;
}

// Get the world point associated with (x, y, disparity) in the disparity map,
// at the given level, using normalized camera objects.
Camera::Vector3 dstToWorldPoint(
    const Camera& camDst,
    const int x,
    const int y,
    const float disparity,
    const int dstW,
    const int dstH,
    const double shiftX,
    const double shiftY) {
  Camera::Vector2 p((x + shiftX) / dstW, (y + shiftY) / dstH);
  if (!camDst.isNormalized()) {
    p = p.cwiseProduct(camDst.resolution);
  }
  return camDst.rig(p, 1.0f / disparity);
}

// Given a world point, find the corresponding location in the source camera
// image at iLevel (or full size if iLevel < 0)
bool worldToSrcPoint(
    Camera::Vector2& pSrc,
    const Camera::Vector3& pWorld,
    const Camera& camSrc,
    const int srcW,
    const int srcH) {
  // Find point in source camera
  if (!camSrc.sees(pWorld, pSrc)) {
    return false; // outside src FOV, ignore
  }

  // De-normalize to current src level
  if (camSrc.isNormalized()) {
    pSrc.x() *= srcW;
    pSrc.y() *= srcH;
  }
  return true;
}

std::vector<int> mapSrcToDstIndexes(const Camera::Rig& rigSrc, const Camera::Rig& rigDst) {
  std::vector<int> dst2srcIdxs(rigDst.size());
  for (int dstIdx = 0; dstIdx < int(rigDst.size()); ++dstIdx) {
    const std::string& dstId = rigDst[dstIdx].id;
    for (int srcIdx = 0; srcIdx < int(rigSrc.size()); ++srcIdx) {
      const std::string& srcId = rigSrc[srcIdx].id;
      if (dstId == srcId) {
        dst2srcIdxs[dstIdx] = srcIdx;
        break;
      }
    }
  }
  return dst2srcIdxs;
}

std::vector<std::array<int, 2>> prunePingPongCandidates(
    const std::vector<std::array<int, 2>>& pingPongCandidateOffsets,
    const cv::Mat_<cv::Vec3b>& labImage,
    const std::array<int, 2>& startPoint,
    const size_t numNeighbors) {
  std::priority_queue<PointDistance> closestPointDistances;
  std::vector<std::array<int, 2>> closestPointOffsets(numNeighbors);
  const cv::Vec3b basePixel = labImage(startPoint[1], startPoint[0]);
  for (const auto& pingPongCandidateOffset : pingPongCandidateOffsets) {
    const std::array<int, 2>& curPoint = {
        {startPoint[0] + pingPongCandidateOffset[0], startPoint[1] + pingPongCandidateOffset[1]}};
    if (curPoint[0] < 0 || curPoint[0] > labImage.cols - 1 || curPoint[1] < 0 ||
        curPoint[1] > labImage.rows - 1) {
      continue;
    }

    const cv::Vec3b& curPixel = labImage(curPoint[1], curPoint[0]);
    const PointDistance curPointDistance =
        PointDistance(pingPongCandidateOffset, cv::norm(basePixel, curPixel, cv::NORM_L2));
    if (closestPointDistances.size() < numNeighbors) {
      closestPointDistances.push(curPointDistance);
    } else if (curPointDistance < closestPointDistances.top()) {
      closestPointDistances.pop();
      closestPointDistances.push(curPointDistance);
    }
  }

  for (size_t i = 0; i < numNeighbors; ++i) {
    PointDistance closestPointDistance = closestPointDistances.top();
    closestPointDistances.pop();
    closestPointOffsets[i] = closestPointDistance.m_point;
  }
  return closestPointOffsets;
}

// Compute biased and ubiased SSD
std::pair<float, float> computeSSD(
    const cv::Mat_<PixelType>& dstColor,
    const int x,
    const int y,
    const PixelType& dstBias,
    const cv::Mat_<PixelType>& dstSrcColor,
    const float xDstSrc,
    const float yDstSrc,
    const PixelType& dstSrcBias,
    const int radius) {
  const PixelTypeFloat bias =
      static_cast<PixelTypeFloat>(dstBias) - static_cast<PixelTypeFloat>(dstSrcBias);
  std::pair<float, float> ssd = {0.0f, 0.0f};
  for (int dx = -radius; dx <= radius; ++dx) {
    for (int dy = -radius; dy <= radius; ++dy) {
      const PixelTypeFloat cDst = dstColor(y + dy, x + dx);
      const PixelTypeFloat cSrc =
          cv_util::getPixelBilinear(dstSrcColor, xDstSrc + dx, yDstSrc + dy);

      const PixelTypeFloat diffBias = cDst - cSrc;
      const PixelTypeFloat diffNoBias = diffBias - bias;

      // Ignore alpha
      const PixelTypeFloat diffBiasRGB(diffBias[0], diffBias[1], diffBias[2]);
      const PixelTypeFloat diffNoBiasRGB(diffNoBias[0], diffNoBias[1], diffNoBias[2]);
      ssd.first += diffBiasRGB.dot(diffBiasRGB);
      ssd.second += diffNoBiasRGB.dot(diffNoBiasRGB);
    }
  }

  const float maxDepth = cv_util::maxPixelValue(dstSrcColor);
  const float scaleFactor = 1.0f / math_util::square(maxDepth);
  ssd.first *= scaleFactor;
  ssd.second *= scaleFactor;

  return ssd;
}

void plotDstPointInSrc(
    const Camera& camDst,
    const int x,
    const int y,
    const float disparity,
    const Camera camSrc,
    const cv::Mat_<PixelType>& srcColor,
    const cv::Mat_<PixelType>& dstColor,
    const filesystem::path& outputDir,
    const std::string& prefix) {
  const auto pWorld = dstToWorldPoint(camDst, x, y, disparity, dstColor.cols, dstColor.rows);
  Camera::Vector2 ptSrc;
  if (!worldToSrcPoint(ptSrc, pWorld, camSrc, srcColor.cols, srcColor.rows)) {
    return;
  }

  // convert so we don't modify the original
  using SaveType = cv::Vec<uint16_t, PixelType::channels>;
  cv::Mat_<SaveType> srcColorCopy = cv_util::convertTo<uint16_t>(srcColor);
  const SaveType green = cv_util::createBGR<SaveType>(0, 1, 0);
  srcColorCopy(ptSrc.y(), ptSrc.x()) = green;

  const std::string filename = fmt::format(
      "{}/{}_{}_x={}_y={}->{}_x={:.2f}_y={:.2f}.png",
      outputDir.string(),
      prefix,
      camDst.id,
      x,
      y,
      camSrc.id,
      ptSrc.x(),
      ptSrc.y());
  cv::imwrite(filename, srcColorCopy);
}

cv::Mat_<PixelType> project(
    const cv::Mat_<PixelType>& srcColor,
    const cv::Mat_<cv::Vec2f>& warpDstToSrc) {
  cv::Mat_<PixelType> dstColor(warpDstToSrc.size());
  cv::remap(srcColor, dstColor, warpDstToSrc, cv::Mat(), cv::INTER_CUBIC, cv::BORDER_CONSTANT);
  return dstColor;
}

// Color bias is just the average over a given area around each pixel
cv::Mat_<PixelType> colorBias(const cv::Mat_<PixelType>& color, const int blurRadius) {
  return cv_util::blur(color, blurRadius);
}

// Computes per-channel variance [0, 1]
// var = E[(X - mu)^2] = E[X^2] - E[X]^2
cv::Mat computeRgbVariance(const cv::Mat& image, const int windowRadius) {
  const int winDiameter = 2 * windowRadius + 1;
  const cv::Size winSize = cv::Size(winDiameter, winDiameter);

  cv::Mat imageF = cv_util::convertTo<float>(image);
  cv::Mat mean;
  cv::blur(imageF, mean, winSize);
  cv::Mat meanOfSquares;
  cv::blur(imageF.mul(imageF), meanOfSquares, winSize);
  return meanOfSquares - mean.mul(mean);
}

// Combined RGB variance [0, 1]
cv::Mat_<float> computeImageVariance(const cv::Mat& image) {
  CHECK(image.channels() == 3 || image.channels() == 4) << "Input image can only be RGB(A)";
  const int kVarWinRadius = 1;
  const cv::Mat_<cv::Vec3f> varRgb = computeRgbVariance(cv_util::removeAlpha(image), kVarWinRadius);

  cv::Mat_<float> varChannels[3];
  cv::split(varRgb, varChannels);

  return varChannels[0] * kRgbWeights[2] + varChannels[1] * kRgbWeights[1] +
      varChannels[2] * kRgbWeights[0]; // OpenCV order: BGR
}

static bool isOutsideImageCircle(
    const Camera& cam,
    const int x,
    const int y,
    const cv::Size& size,
    Camera::Vector2& p) {
  p = Camera::Vector2(x + 0.5, y + 0.5);
  if (cam.isNormalized()) {
    const Camera::Vector2 resolution(size.width, size.height);
    p = p.cwiseQuotient(resolution);
  }
  return cam.isOutsideImageCircle(p);
}

static bool
isOutsideImageCircle(const Camera& cam, const int x, const int y, const cv::Size& size) {
  Camera::Vector2 ignored;
  return isOutsideImageCircle(cam, x, y, size, ignored);
}

std::vector<cv::Mat_<bool>>
generateFovMasks(const Camera::Rig& rig, const cv::Size& size, const int threads) {
  std::vector<cv::Mat_<bool>> masks(rig.size());
  ThreadPool threadPool(threads);
  for (int i = 0; i < int(rig.size()); ++i) {
    threadPool.spawn([&, i] {
      const Camera& cam = rig[i];
      masks[i] = cv::Mat_<bool>(size);
      for (int y = 0; y < masks[i].rows; ++y) {
        for (int x = 0; x < masks[i].cols; ++x) {
          masks[i](y, x) = !isOutsideImageCircle(cam, x, y, size);
        }
      }
    });
  }
  threadPool.join();
  return masks;
}

filesystem::path getImageDir(const filesystem::path& dir, const ImageType& imageType) {
  return dir / imageTypes[int(imageType)];
}

filesystem::path
getImageDir(const filesystem::path& dir, const ImageType& imageType, const int level) {
  return fmt::format("{}/level_{}", getImageDir(dir, imageType).string(), std::to_string(level));
}

filesystem::path getImageDir(
    const filesystem::path& dir,
    const ImageType& imageType,
    const int level,
    const std::string& camId) {
  return getImageDir(dir, imageType, level) / camId;
}

filesystem::path
getImageDir(const filesystem::path& dir, const ImageType& imageType, const std::string& camId) {
  return getImageDir(dir, imageType) / camId;
}

filesystem::path genFilename(
    const filesystem::path& dir,
    const ImageType& imageType,
    const int level,
    const std::string& camId,
    const std::string& frameName,
    const std::string& extension) {
  return fmt::format(
      "{}/{}.{}", getImageDir(dir, imageType, level, camId).string(), frameName, extension);
}

void createLevelOutputDirs(
    const filesystem::path& outputDir,
    const int level,
    const Camera::Rig& rig,
    const bool saveDebugImages) {
  for (const Camera& cam : rig) {
    const std::string& id = cam.id;

    // This will only be done the first time, it's level independent
    filesystem::create_directories(getImageDir(outputDir, ImageType::disparity, id));

    if (saveDebugImages) {
      filesystem::create_directories(
          getImageDir(outputDir, ImageType::disparity_levels, level, id));
      filesystem::create_directories(getImageDir(outputDir, ImageType::cost, level, id));
      filesystem::create_directories(getImageDir(outputDir, ImageType::confidence, level, id));
      filesystem::create_directories(getImageDir(outputDir, ImageType::mismatches, level, id));
    }
  }
}

} // namespace fb360_dep::depth_estimation
