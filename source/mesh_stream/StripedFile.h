/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include <vector>

#include <glog/logging.h>

#include "source/mesh_stream/AsyncFile.h"

namespace fb360_dep {

static const uint64_t kStripeSize = 512 * 1024;

// a StripedFile is N files ("disks") posing as a single, logical file
// each sub-file holds every N "stripe" of the logical file
// and each stripe is 512kB

// to initiate a read from a striped file, you might:
//   PendingRead* request = stripedFile.readBegin(dst.data(), offset, size);
// note: offset must be stripe-aligned, dst and size must be page-aligned

// to complete a read, you must then:
//   stripedFile.readEnd(request);
// note: this operation is blocking

struct StripedFile {
  StripedFile() {}

  StripedFile(const std::vector<std::string>& diskNames) {
    // open the disks
    for (const std::string& diskName : diskNames) {
      disks.emplace_back(diskName);
    }
  }

  ~StripedFile() {
    // close the disks
    for (const AsyncFile& disk : disks) {
      disk.close();
    }
  }

  using PendingRead = std::vector<AsyncFile::PendingRead>;

  PendingRead* readBegin(uint8_t* dst, uint64_t offset, uint64_t size) const {
    // check alignment constraints
    CHECK(align(offset, kStripeSize) == offset);
    // create vector to store PendingReads
    const uint64_t stripeCount = align(size, kStripeSize) / kStripeSize;
    const bool kPerStripe = false; // one read per stripe or one read per disk?
    const uint64_t readCount = kPerStripe ? stripeCount : disks.size();
    PendingRead* result = new PendingRead(readCount);
    // for each disk, compute local disk offset and memory segments
    std::vector<uint64_t> offsets(disks.size(), UINT64_MAX);
    std::vector<std::vector<AsyncFile::Segment>> segments(disks.size());
    for (int stripe = 0; stripe < int(stripeCount); ++stripe) {
      uint64_t local, disk;
      calcStripe(local, disk, offset);
      AsyncFile::Segment segment = {dst, std::min(size, kStripeSize)};
      if (kPerStripe) {
        // kick off a separate read for every stripe, don't coalesce per disk
        disks[disk].readBegin((*result)[stripe], {segment}, local);
      } else {
        offsets[disk] = std::min(local, offsets[disk]);
        segments[disk].push_back(segment);
      }
      dst += kStripeSize;
      offset += kStripeSize;
      size -= kStripeSize;
    }
    if (!kPerStripe) {
      // kick off one read per disk
      for (int read = 0; read < int(disks.size()); ++read) {
        CHECK_NE(offsets[read], UINT64_MAX);
        disks[read].readBegin((*result)[read], segments[read], offsets[read]);
      }
    }
    return result;
  }

  static void readEnd(PendingRead* request) {
    for (AsyncFile::PendingRead& read : *request) {
      AsyncFile::readEnd(read);
    }
    delete request;
  }

  // compute the disk and local offset from global offset
  static void
  calcStripe(uint64_t& local, uint64_t& disk, const uint64_t global, const uint64_t diskCount) {
    uint64_t stripe = global / kStripeSize;
    local = (stripe / diskCount) * kStripeSize;
    disk = stripe % diskCount;
  }

  void calcStripe(uint64_t& local, uint64_t& disk, const uint64_t global) const {
    calcStripe(local, disk, global, disks.size());
  }

  std::vector<AsyncFile> disks;
};

} // namespace fb360_dep
