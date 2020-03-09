/**
 * Copyright 2004-present Facebook. All Rights Reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifdef WIN32
#define METHOD 0 // ReadFileScatter
#else
#define METHOD 1 // async pread
// #define METHOD 2 // sync pread
#endif

#if METHOD == 0
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <future>
#include <mutex>
#endif

#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifndef GLOG_NO_ABBREVIATED_SEVERITIES
#define GLOG_NO_ABBREVIATED_SEVERITIES
#endif
#include <glog/logging.h>

namespace fb360_dep {

const uint64_t kPageSize = 4096;

uint64_t align(uint64_t offset, uint64_t alignment) {
  return (offset + alignment - 1) & ~(alignment - 1);
}

uint8_t* align(uint8_t* p, uint64_t alignment) {
  return reinterpret_cast<uint8_t*>(align(reinterpret_cast<uint64_t>(p), alignment));
}

struct AsyncFile {
#if METHOD == 0

  HANDLE handle;

  struct Segment {
    // mimic posix iovec struct https://linux.die.net/man/2/readv
    uint8_t* iov_base;
    size_t iov_len;
  };

  struct PendingRead {
    HANDLE handle;
    OVERLAPPED overlapped;
  };

  AsyncFile(const std::string& filename) {
    handle = CreateFileA(
        filename.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING,
        nullptr);
    CHECK_NE(handle, INVALID_HANDLE_VALUE) << "error opening " << filename;
    activityLog().addFile(handle, filename);
  }

  void close() const {
    CloseHandle(handle);
  }

  void readBegin(PendingRead& pending, const std::vector<Segment>& segments, uint64_t offset)
      const {
    pending.handle = handle;
    pending.overlapped.Offset = offset & 0xFFFFFFFF;
    pending.overlapped.OffsetHigh = offset >> 32;
    pending.overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    pending.overlapped.Internal = 0;
    pending.overlapped.InternalHigh = 0;
    // compute the total number of bytes
    uint64_t total = 0;
    for (const auto& segment : segments) {
      uint8_t* dst = segment.iov_base;
      uint64_t size = segment.iov_len;
      CHECK(align(dst, kPageSize) == dst);
      CHECK(align(size, kPageSize) == size);
      total += segment.iov_len;
    }
    // create a FILE_SEGMENT_ELEMENT per page plus an extra for NULL
    std::vector<FILE_SEGMENT_ELEMENT> pages(total / kPageSize + 1);
    size_t pageIndex = 0;
    for (const auto& segment : segments) {
      uint8_t* dst = segment.iov_base;
      uint64_t size = segment.iov_len;
      for (uint64_t i = 0; i < size / kPageSize; ++i) {
        pages[pageIndex].Buffer = dst;
        ++pageIndex;
        dst += kPageSize;
      }
    }
    CHECK_EQ(pageIndex, pages.size() - 1);
    pages.back().Buffer = nullptr;
    // kick off the read
    const BOOL ok = ReadFileScatter(
        pending.handle, pages.data(), static_cast<DWORD>(total), nullptr, &pending.overlapped);
    if (!ok) {
      CHECK_EQ(GetLastError(), ERROR_IO_PENDING);
    }
    activityLog().event(handle, offset, 0);
  }

  static uint64_t readEnd(PendingRead& pending) {
    uint64_t offset = pending.overlapped.OffsetHigh;
    offset = offset << 32 | pending.overlapped.Offset;
    activityLog().event(pending.handle, offset, 1);
    DWORD transferred;
    const BOOL ok = GetOverlappedResult(pending.handle, &pending.overlapped, &transferred, TRUE);
    CHECK(ok) << "file read error = " << GetLastError();
    CloseHandle(pending.overlapped.hEvent);
    activityLog().event(pending.handle, offset, 2);
    return transferred;
  }

#else // METHOD == 0

  using HANDLE = int;
  HANDLE handle;

  using Segment = iovec;

  using PendingRead = std::future<ssize_t>;

  AsyncFile(const std::string& filename) {
    handle = open(filename.c_str(), O_RDONLY);
    CHECK_NE(handle, -1) << "error opening " << filename;
  }

  void close() const {
    ::close(handle);
  }

  ssize_t readSegments(const std::vector<Segment>& segments, uint64_t offset) const {
    ssize_t total = 0;
    for (const Segment& segment : segments) {
      ssize_t result = pread(handle, segment.iov_base, segment.iov_len, offset + total);
      CHECK_NE(result, -1) << "file read error " << errno;
      total += result;
    }
    return total;
  }

  void readBegin(PendingRead& pending, const std::vector<Segment>& segments, uint64_t offset)
      const {
#if defined(__APPLE__) || defined(__linux__)
    // apple doesn't support preadv and linux crashes
    pending = std::async(&AsyncFile::readSegments, this, segments, offset);
#else // __APPLE__
    pending = std::async(preadv, handle, segments.data(), segments.size(), offset);
#endif
  }

  static uint64_t readEnd(PendingRead& pending) {
    ssize_t result;
    result = pending.get();
    CHECK_NE(result, -1) << "file read error = " << errno;
    return result;
  }

#endif // METHOD == 0

  struct ActivityLog {
    ~ActivityLog() {
      dump("activitylog.tsv");
    }

    // keep a decoder ring to map filehandles to filenames
    std::map<HANDLE, std::string> filenames;
    // a request is identified by filehandle, offset
    using Request = std::pair<HANDLE, uint64_t>;
    // the events associated with a request are just 3 timestamps
    static const size_t kEventCount = 3; // begin, get, done
    using Clock = std::chrono::high_resolution_clock;
    using Events = std::array<Clock::time_point, kEventCount>;
    // live requests are stored in a map, completed requests in a vector
    std::map<Request, Events> live;
    std::vector<std::pair<Request, Events>> completed;
    // protect updates with a mutex
    std::mutex mutex;

    // call to populate filehandle -> filename decoder ring
    void addFile(HANDLE filehandle, const std::string& filename) {
      std::lock_guard<std::mutex> guard(mutex);
      filenames[filehandle] = filename;
    }

    // call when event occurs
    void event(HANDLE filehandle, uint64_t offset, size_t index) {
      CHECK_LT(index, kEventCount);
      Request request(filehandle, offset);
      Clock::time_point now = Clock::now();
      std::lock_guard<std::mutex> guard(mutex);
      live[request][index] = now;
      if (index + 1 == kEventCount) { // last event, transfer from live to log
        completed.emplace_back(request, live[request]);
        live.erase(request);
      }
    }

    void dump(const std::string& filename) {
      std::ofstream file(filename);
      for (const auto& entry : completed) {
        file << "\"" << filenames[entry.first.first] << "\"";
        file << "\t" << entry.first.second;
        for (const auto& timestamp : entry.second) {
          // offset timestamps from first timestamp
          static Clock::time_point offset = timestamp;
          std::chrono::duration<double> elapsed = timestamp - offset;
          file << "\t" << elapsed.count();
        }
        file << std::endl;
      }
    }
  };

  // stick all asyncfile activity into the same activity log
  static AsyncFile::ActivityLog& activityLog() {
    static ActivityLog singleton;
    return singleton;
  }
};

} // namespace fb360_dep
