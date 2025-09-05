/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "source/util/RawUtil.h"

#include <fmt/format.h>
#include <opencv2/core.hpp>

#include <folly/FileUtil.h>
#include <folly/Format.h>

#include "source/isp/CameraIsp.h"
#include "source/isp/DngTags.h"

namespace fb360_dep {

const DemosaicFilter kDefaultDemosaicFilterForRawToRgb = DemosaicFilter::BILINEAR;

static filesystem::path defaultCameraIspConfigFilenameForImageFilename(
    const filesystem::path& rawImageFilename) {
  CHECK(rawImageFilename.has_filename());
  return rawImageFilename.parent_path() / kDefaultIspConfigFilename;
}

template <typename T>
std::vector<T> readRawImage(const filesystem::path& rawImageFilename, const CameraIsp& cameraIsp) {
  std::ifstream rawImageFile(rawImageFilename.string(), std::ios::in | std::ios::binary);
  CHECK(rawImageFile) << "could not open raw image file: " << rawImageFilename << std::endl;
  int numPixels = cameraIsp.getSensorWidth() * cameraIsp.getSensorHeight();
  std::vector<T> rawImage = std::vector<T>(numPixels);
  rawImageFile.read((char*)rawImage.data(), numPixels * sizeof(T));
  CHECK(rawImageFile.good()) << "unexpected end of file: " << rawImageFilename << std::endl;

  return rawImage;
}

std::unique_ptr<CameraIsp> cameraIspFromConfigFileWithOptions(
    const filesystem::path& configFilename,
    int pow2DownscaleFactor,
    DemosaicFilter demosaicFilter,
    bool applyToneCurve) {
  std::string json;
  folly::readFile(configFilename.string().c_str(), json);
  CHECK(!json.empty()) << "could not read JSON file: " << configFilename << std::endl;

  auto cameraIsp = std::make_unique<CameraIsp>(json);
  cameraIsp->setResize(pow2DownscaleFactor);
  cameraIsp->setDemosaicFilter(demosaicFilter);
  cameraIsp->setToneCurveEnabled(applyToneCurve);

  return cameraIsp;
}

template <typename T>
cv::Mat_<cv::Vec<T, 3>> rawToRgb(const std::vector<T>& rawImage, CameraIsp& cameraIsp) {
  cameraIsp.loadImageFromSensor(rawImage);
  return cameraIsp.getImage<T>();
}

template cv::Mat_<cv::Vec<uint8_t, 3>> rawToRgb(const std::vector<uint8_t>&, CameraIsp&);
template cv::Mat_<cv::Vec<uint16_t, 3>> rawToRgb(const std::vector<uint16_t>&, CameraIsp&);

template <typename T>
cv::Mat_<cv::Vec<T, 3>> rawToRgb(
    const filesystem::path& rawImageFilename,
    const filesystem::path& ispConfigFilename,
    int pow2DownscaleFactor,
    DemosaicFilter demosaicFilter,
    bool applyToneCurve) {
  // - load and setup ISP config
  CHECK_EQ(rawImageFilename.extension(), ".raw");
  filesystem::path ispConfigFilenameToLoad = ispConfigFilename.empty()
      ? defaultCameraIspConfigFilenameForImageFilename(rawImageFilename)
      : ispConfigFilename;
  std::unique_ptr<CameraIsp> cameraIsp = cameraIspFromConfigFileWithOptions(
      ispConfigFilenameToLoad, pow2DownscaleFactor, demosaicFilter, applyToneCurve);

  // - load raw image data
  std::vector<T> rawImage = readRawImage<T>(rawImageFilename, *cameraIsp);

  // - process the image
  return rawToRgb(rawImage, *cameraIsp);
}

template cv::Mat_<cv::Vec<uint8_t, 3>>
rawToRgb(const filesystem::path&, const filesystem::path&, int, DemosaicFilter, bool);
template cv::Mat_<cv::Vec<uint16_t, 3>>
rawToRgb(const filesystem::path&, const filesystem::path&, int, DemosaicFilter, bool);

cv::Mat rawToRgb(
    const boost::filesystem::path& rawImageFilename,
    const boost::filesystem::path& ispConfigFilename,
    int pow2DownscaleFactor,
    DemosaicFilter demosaicFilter,
    bool applyToneCurve) {
  // - load and setup ISP config
  CHECK_EQ(rawImageFilename.extension(), ".raw");
  filesystem::path ispConfigFilenameToLoad = ispConfigFilename.empty()
      ? defaultCameraIspConfigFilenameForImageFilename(rawImageFilename)
      : ispConfigFilename;
  std::unique_ptr<CameraIsp> cameraIsp = cameraIspFromConfigFileWithOptions(
      ispConfigFilenameToLoad, pow2DownscaleFactor, demosaicFilter, applyToneCurve);

  // - load raw image data and process
  int bitsPerPixel = cameraIsp->getSensorBitsPerPixel();
  CHECK(bitsPerPixel == 8 || bitsPerPixel == 16) << "Unsupported precision" << std::endl;
  if (bitsPerPixel == 8) {
    return rawToRgb<uint8_t>(readRawImage<uint8_t>(rawImageFilename, *cameraIsp), *cameraIsp);
  } else { // bitsPerPixel == 16
    return rawToRgb<uint16_t>(readRawImage<uint16_t>(rawImageFilename, *cameraIsp), *cameraIsp);
  }
}

void writeIfd(
    const uint16_t tag,
    const uint16_t type,
    const uint32_t count,
    const uint32_t offset,
    const uint32_t offsetInc,
    uint32_t& dOffset,
    FILE* fDng) {
  // Write out a tiff directory entry aka an ifd
  fwrite(&tag, sizeof(tag), 1, fDng);
  fwrite(&type, sizeof(type), 1, fDng);
  fwrite(&count, sizeof(count), 1, fDng);
  fwrite(&offset, sizeof(offset), 1, fDng);

  dOffset += offsetInc;
}

template <typename T>
bool writeDng(
    const filesystem::path& rawImageFilename,
    const filesystem::path& outputFilename,
    CameraIsp& cameraIsp) {
  LOG(INFO) << fmt::format("Writing: {}", outputFilename.string()) << std::endl;
  // - sanity check
  CHECK_EQ(rawImageFilename.extension(), ".raw");
  int outputBitsPerPixel = 8 * sizeof(T);

  LOG_IF(WARNING, outputBitsPerPixel != cameraIsp.getSensorBitsPerPixel())
      << outputBitsPerPixel << "-bit output precision != " << cameraIsp.getSensorBitsPerPixel()
      << "-bit input precision" << std::endl;

  // - load ISP config and read raw image
  switch (cameraIsp.getSensorBitsPerPixel()) {
    case 8: {
      std::vector<uint8_t> rawImage = readRawImage<uint8_t>(rawImageFilename, cameraIsp);
      cameraIsp.loadImageFromSensor(rawImage);
      break;
    }
    case 16: {
      std::vector<uint16_t> rawImage = readRawImage<uint16_t>(rawImageFilename, cameraIsp);
      cameraIsp.loadImageFromSensor(rawImage);
      break;
    }
    default:
      CHECK(false) << "Unsupported output precision" << std::endl;
  };

  // The step below involves an internal conversion to and from float32, which, for uint16
  // input+output type, can result in off-by-one differences between pixels in the original image
  // and the written DNG
  cv::Mat_<T> preprocessedRawImage = cameraIsp.getRawImage<T>();

  FILE* fDng = fopen(outputFilename.string().c_str(), "w");
  if (fDng == nullptr) {
    LOG(ERROR) << fmt::format("Failed to open file: {}", outputFilename.string()) << std::endl;
    return false;
  }

  uint32_t width = preprocessedRawImage.cols;
  uint32_t height = preprocessedRawImage.rows;

  // TIFF data layout calculations (64k strip for 16bit data)
  uint16_t rowsPerStrip = (32 * 1024) / width;
  uint16_t stripsPerImg = (height / rowsPerStrip);
  if (height % rowsPerStrip != 0) {
    ++stripsPerImg;
  }

  const std::string cameraSoftware("RawToRgb");

  // Write the TIFF file header
  const char byteOrder[3] = "II";
  fwrite(byteOrder, sizeof(char), 2, fDng);

  const uint16_t version = 42;
  fwrite(&version, sizeof(uint16_t), 1, fDng);

  const uint32_t Idf0Offset = 0x00000008;
  fwrite(&Idf0Offset, sizeof(unsigned), 1, fDng);

  const uint16_t ifdCount = 42;
  fwrite(&ifdCount, sizeof(uint16_t), 1, fDng);

  // Map ISP cfa pattern code to DNG's
  uint32_t cfaFilter;

  switch (cameraIsp.getFilters()) {
    case 0x94949494:
      cfaFilter = 0x02010100;
      break;
    case 0x16161616:
      cfaFilter = 0x00010102;
      break;
    case 0x49494949:
      cfaFilter = 0x01000201;
      break;
    case 0x61616161:
      cfaFilter = 0x01020001;
      break;
    default:
      LOG(ERROR) << "Unknown bayer-pattern found while writing DNG file" << std::endl;
      fclose(fDng);
      return false;
  }

  // Write the tags
  const uint32_t kIfdEntrySize = sizeof(uint16_t) * 2 + sizeof(uint32_t) * 2;
  uint32_t dOffset = 10 + ifdCount * kIfdEntrySize + 4;

  writeIfd(kTiffTagNewSubFileType, kTiffTypeLONG, 1, 0, 0, dOffset, fDng);
  writeIfd(kTiffTagImageWidth, kTiffTypeLONG, 1, width, 0, dOffset, fDng);
  writeIfd(kTiffTagImageLength, kTiffTypeLONG, 1, height, 0, dOffset, fDng);
  writeIfd(kTiffTagBitsPerSample, kTiffTypeSHORT, 1, outputBitsPerPixel, 0, dOffset, fDng);
  writeIfd(kTiffTagCompression, kTiffTypeSHORT, 1, 1, 0, dOffset, fDng);
  writeIfd(kTiffTagPhotometricInterpretation, kTiffTypeSHORT, 1, 32803, 0, dOffset, fDng);
  writeIfd(
      kTiffTagStripOffsets, kTiffTypeLONG, stripsPerImg, dOffset, stripsPerImg * 4, dOffset, fDng);
  writeIfd(kTiffTagOrientation, kTiffTypeSHORT, 1, 1, 0, dOffset, fDng);
  writeIfd(kTiffTagSamplesPerPixel, kTiffTypeSHORT, 1, 1, 0, dOffset, fDng);
  writeIfd(kTiffTagRowsPerStrip, kTiffTypeSHORT, 1, rowsPerStrip, 0, dOffset, fDng);
  writeIfd(
      kTiffTagStripByteCounts,
      kTiffTypeSHORT,
      stripsPerImg,
      dOffset,
      stripsPerImg * 2,
      dOffset,
      fDng);
  writeIfd(kTiffTagPlanarConfiguration, kTiffTypeSHORT, 1, 1, 0, dOffset, fDng);
  writeIfd(kTiffTagResolutionUnit, kTiffTypeSHORT, 1, 2, 0, dOffset, fDng);
  writeIfd(
      kTiffTagSoftware,
      kTiffTypeASCII,
      cameraSoftware.size() + 1,
      dOffset,
      cameraSoftware.size() + 1,
      dOffset,
      fDng);
  writeIfd(kTiffTagDateTime, kTiffTypeASCII, 20, dOffset, 20, dOffset, fDng);
  writeIfd(kTiffEpTagCFARepeatPatternDim, kTiffTypeSHORT, 2, 0x00020002, 0, dOffset, fDng);
  writeIfd(kTiffEpTagCFAPattern, kTiffTypeBYTE, 4, cfaFilter, 0, dOffset, fDng);
  writeIfd(kDngTagDNGVersion, kTiffTypeBYTE, 4, 0x00000301, 0, dOffset, fDng);
  writeIfd(kDngTagDNGBackwardVersion, kTiffTypeBYTE, 4, 0x00000101, 0, dOffset, fDng);
  writeIfd(kDngTagCFAPlaneColor, kTiffTypeBYTE, 3, 0x00020100, 0, dOffset, fDng);
  writeIfd(kDngTagCFALayout, kTiffTypeSHORT, 1, 1, 0, dOffset, fDng);
  writeIfd(kDngTagBlackLevelRepeatDim, kTiffTypeSHORT, 2, 0x00020002, 0, dOffset, fDng);
  writeIfd(kDngTagBlackLevel, kTiffTypeSHORT, 4, dOffset, 8, dOffset, fDng);
  writeIfd(kDngTagWhiteLevel, kTiffTypeLONG, 1, (1 << outputBitsPerPixel) - 1, 0, dOffset, fDng);
  writeIfd(kDngTagDefaultScale, kTiffTypeRATIONAL, 2, dOffset, 16, dOffset, fDng);
  writeIfd(kDngTagDefaultCropOrigin, kTiffTypeSHORT, 2, 0, 0, dOffset, fDng);
  writeIfd(kDngTagDefaultCropSize, kTiffTypeSHORT, 2, (height << 16) | width, 0, dOffset, fDng);
  writeIfd(kDngTagColorMatrix1, kTiffTypeSRATIONAL, 9, dOffset, 9 * 8, dOffset, fDng);
  writeIfd(kDngTagAnalogBalance, kTiffTypeRATIONAL, 3, dOffset, 3 * 8, dOffset, fDng);
  writeIfd(kDngTagAsShotNeutral, kTiffTypeRATIONAL, 3, dOffset, 3 * 8, dOffset, fDng);
  writeIfd(kDngTagBaselineExposure, kTiffTypeSRATIONAL, 1, dOffset, 8, dOffset, fDng);
  writeIfd(kDngTagBaselineSharpness, kTiffTypeRATIONAL, 1, dOffset, 8, dOffset, fDng);
  writeIfd(kDngTagBayerGreenSplit, kTiffTypeLONG, 1, 0, 0, dOffset, fDng);
  writeIfd(kDngTagLinearResponseLimit, kTiffTypeRATIONAL, 1, dOffset, 8, dOffset, fDng);
  writeIfd(kDngTagLensInfo, kTiffTypeRATIONAL, 4, dOffset, 32, dOffset, fDng);
  writeIfd(kDngTagAntiAliasStrength, kTiffTypeRATIONAL, 1, dOffset, 8, dOffset, fDng);
  writeIfd(kDngTagCalibrationIlluminant1, kTiffTypeSHORT, 1, 23, 0, dOffset, fDng);
  writeIfd(kDngTagBestQualityScale, kTiffTypeRATIONAL, 1, dOffset, 8, dOffset, fDng);

  // Now write data fields
  uint32_t firstIFD = 0x00000000;

  fwrite(&firstIFD, sizeof(uint32_t), 1, fDng);

  std::vector<uint32_t> stripOff;
  stripOff.push_back(dOffset); // where image data will be written...
  for (int16_t s = 1; s < stripsPerImg; ++s) {
    stripOff.push_back(stripOff[s - 1] + (rowsPerStrip * width) * 2);
  }

  fwrite(&stripOff[0], sizeof(unsigned), stripsPerImg, fDng);

  std::vector<uint16_t> stripCnt;
  int nRowsLeft = height;
  uint32_t t = 0;
  while (nRowsLeft > 0) {
    if ((unsigned)nRowsLeft > rowsPerStrip) {
      stripCnt.push_back(rowsPerStrip * width * 2);
    } else {
      stripCnt.push_back(nRowsLeft * width * 2);
    }
    ++t;
    nRowsLeft -= rowsPerStrip;
  }

  fwrite(&stripCnt[0], sizeof(uint16_t), stripsPerImg, fDng);
  fwrite(cameraSoftware.c_str(), sizeof(char), cameraSoftware.size() + 1, fDng);

  char szDateTime[72];
  time_t time = 0; // Need to get this from the camera meta data.
  struct tm* tlocal = localtime(&time);
  snprintf(
      szDateTime,
      72,
      "%04d-%02d-%02d %02d:%02d:%02d",
      tlocal->tm_year + 1900,
      tlocal->tm_mon,
      tlocal->tm_mday,
      tlocal->tm_hour,
      tlocal->tm_min,
      tlocal->tm_sec);
  szDateTime[71] = '\0';

  fwrite(szDateTime, sizeof(char), 20, fDng);

  // Conversion to XYZ - Bradford adapted using D50 reference white point
  cv::Mat_<float> sRgbToXyzD50(3, 3);
  sRgbToXyzD50(0, 0) = 0.4360747;
  sRgbToXyzD50(0, 1) = 0.3850649;
  sRgbToXyzD50(0, 2) = 0.1430804;

  sRgbToXyzD50(1, 0) = 0.2225045;
  sRgbToXyzD50(1, 1) = 0.7168786;
  sRgbToXyzD50(1, 2) = 0.0606169;

  sRgbToXyzD50(2, 0) = 0.0139322;
  sRgbToXyzD50(2, 1) = 0.0971045;
  sRgbToXyzD50(2, 2) = 0.7141733;

  cv::Mat_<float> ccm = cameraIsp.getCCM();
  cv::Mat_<float> camToXyz = ccm * sRgbToXyzD50;
  cv::Mat_<float> xyzToCam;
  invert(camToXyz, xyzToCam);

  uint16_t uBlackLevel[4]; // {G, R, B, G}
  cv::Point3f bl = cameraIsp.getBlackLevel() * ((1 << outputBitsPerPixel) - 1);
  uBlackLevel[0] = bl.y;
  uBlackLevel[3] = bl.y;
  uBlackLevel[1] = bl.x;
  uBlackLevel[2] = bl.z;
  fwrite(uBlackLevel, sizeof(uint16_t), 4, fDng);

  uint32_t defaultScale[4];
  defaultScale[0] = 1;
  defaultScale[2] = 1;
  defaultScale[1] = 1;
  defaultScale[3] = 1;

  fwrite(defaultScale, sizeof(unsigned), 4, fDng);

  int colorMatrix[18];
  for (int i = 0; i < 9; ++i) {
    const int x = i % 3;
    const int y = i / 3;
    colorMatrix[2 * i] = xyzToCam(y, x) * (1 << 28);
    colorMatrix[2 * i + 1] = (1 << 28);
  }
  fwrite(colorMatrix, sizeof(int), 18, fDng);

  uint32_t analogBalance[6];
  analogBalance[0] = 256;
  analogBalance[1] = 256;
  analogBalance[2] = 256;
  analogBalance[3] = 256;
  analogBalance[4] = 256;
  analogBalance[5] = 256;

  fwrite(analogBalance, sizeof(unsigned), 6, fDng);

  // kDngTagAsShotNeutral
  cv::Point3f whitePoint = cameraIsp.getWhiteBalanceGain();
  uint32_t asShotNeutral[6];
  float minChannel = std::min(std::min(whitePoint.x, whitePoint.y), whitePoint.z);
  asShotNeutral[0] = (minChannel / whitePoint.x) * float(1 << 28);
  asShotNeutral[1] = 1 << 28;
  asShotNeutral[2] = (minChannel / whitePoint.y) * float(1 << 28);
  asShotNeutral[3] = 1 << 28;
  asShotNeutral[4] = (minChannel / whitePoint.z) * float(1 << 28);
  asShotNeutral[5] = 1 << 28;

  fwrite(asShotNeutral, sizeof(uint32_t), 6, fDng);

  int32_t baseExposure[2];

  baseExposure[0] = -log2f(1.0f / minChannel) * (1 << 28);
  baseExposure[1] = (1 << 28);

  fwrite(baseExposure, sizeof(unsigned), 2, fDng);

  uint32_t baseSharp[2];

  baseSharp[0] = 1;
  baseSharp[1] = 1;

  fwrite(baseSharp, sizeof(unsigned), 2, fDng);

  uint32_t linearLimit[2];
  linearLimit[0] = 1;
  linearLimit[1] = 1; // 1.0 = sensor is linear

  fwrite(linearLimit, sizeof(unsigned), 2, fDng);

  uint32_t lensInfo[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  fwrite(lensInfo, sizeof(unsigned), 8, fDng);

  uint32_t antiAlias[2];
  antiAlias[0] = 0;
  antiAlias[1] = 1; // Turn off antiAliasStrength

  fwrite(antiAlias, sizeof(unsigned), 2, fDng);

  uint32_t bestScale[2];
  bestScale[0] = 1;
  bestScale[1] = 1; // use 1:1 scaling

  fwrite(bestScale, sizeof(unsigned), 2, fDng);

  // Raw image data
  const size_t status = fwrite(preprocessedRawImage.data, sizeof(T), width * height, fDng);

  if (status != width * height) {
    LOG(ERROR) << "DNG write error: image data" << std::endl;
  }

  fclose(fDng);
  return true;
}

bool writeDng(
    const filesystem::path& rawImageFilename,
    const filesystem::path& outputFilename,
    const filesystem::path& ispConfigFilename) {
  filesystem::path ispConfigFilenameToLoad = ispConfigFilename.empty()
      ? defaultCameraIspConfigFilenameForImageFilename(rawImageFilename)
      : ispConfigFilename;
  std::unique_ptr<CameraIsp> cameraIsp =
      cameraIspFromConfigFileWithOptions(ispConfigFilenameToLoad);
  switch (cameraIsp->getSensorBitsPerPixel()) {
    case 8:
      return writeDng<uint8_t>(rawImageFilename, outputFilename, *cameraIsp);
    case 16:
      return writeDng<uint16_t>(rawImageFilename, outputFilename, *cameraIsp);
    default:
      LOG(ERROR) << "Unsupported precision in raw file" << std::endl;
      return false;
  };
}

template <typename T>
bool writeDng(
    const filesystem::path& rawImageFilename,
    const filesystem::path& outputFilename,
    const filesystem::path& ispConfigFilename) {
  filesystem::path ispConfigFilenameToLoad = ispConfigFilename.empty()
      ? defaultCameraIspConfigFilenameForImageFilename(rawImageFilename)
      : ispConfigFilename;
  std::unique_ptr<CameraIsp> cameraIsp =
      cameraIspFromConfigFileWithOptions(ispConfigFilenameToLoad);
  return writeDng<T>(rawImageFilename, outputFilename, *cameraIsp);
}

template bool
writeDng<uint8_t>(const filesystem::path&, const filesystem::path&, const filesystem::path&);
template bool
writeDng<uint16_t>(const filesystem::path&, const filesystem::path&, const filesystem::path&);

}; // namespace fb360_dep
