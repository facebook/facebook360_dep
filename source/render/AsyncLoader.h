/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

#include "source/gpu/GlUtil.h"

namespace fb360_dep {

struct AsyncLoader {
  AsyncLoader() : batched(false) {}

  AsyncLoader(
      const std::vector<std::string>& filenames,
      const std::vector<size_t>& sizes,
      const bool batched = false,
      const bool threaded = true)
      : batched(batched) {
    const size_t count = filenames.size();
    CHECK_EQ(sizes.size(), count);
    std::vector<char*> buffers(count);
    for (size_t i = 0; i < count; ++i) {
      objects.push_back(createBuffer(type(i), (char*)nullptr, sizes[i]));
      char* p = static_cast<char*>(glMapBuffer(type(i), GL_WRITE_ONLY));
      CHECK(p);
      glBindBuffer(type(i), 0);
      if (batched) {
        buffers[i] = p;
      } else if (threaded) {
        threads.emplace_back(&loadFile, p, filenames[i], sizes[i]);
      } else {
        loadFile(p, filenames[i], sizes[i]);
      }
    }
    if (batched) {
      beginBatch(buffers, filenames, sizes);
    }
  }

  void wait() {
    if (batched) {
      endBatch();
    } else {
      for (std::thread& thread : threads) {
        thread.join();
      }
      threads.clear();
    }
    for (int i = 0; i < int(objects.size()); ++i) {
      glBindBuffer(type(i), objects[i]);
      glUnmapBuffer(type(i));
    }
  }

  std::vector<GLuint> objects;

 private:
  static GLenum type(const size_t i) {
    // we don't have glMapNamedBuffer is gl4, so we have to bind+map
    // is unused type faster?
    return GL_TEXTURE_BUFFER;
    // or is actual buffer type faster? (will never be executed)
    switch (i % 3) {
      case 0:
        return GL_PIXEL_UNPACK_BUFFER;
      case 1:
        return GL_ARRAY_BUFFER;
      default:; // fall through
    }
    return GL_ELEMENT_ARRAY_BUFFER;
  }

  static void loadFile(char* dst, const std::string& filename, size_t bytes) {
    const bool kUseStream = false; // slower on windows
    if (kUseStream) {
      std::ifstream file(filename, std::ios::binary);
      CHECK(file) << "couldn't open " << filename;
      file.read(dst, bytes);
      CHECK(file) << "couldn't read " << bytes << " from " << filename << std::endl;
    } else {
      FILE* file = fopen(filename.c_str(), "rb");
      CHECK(file) << "couldn't open " << filename;
      CHECK_EQ(bytes, fread(dst, 1, bytes, file)) << "file " << filename << std::endl;
      fclose(file);
    }
  }

#ifdef WIN32
  HANDLE iocp;
  size_t pending;
  std::vector<HANDLE> handles;

  void beginBatch(
      std::vector<char*>& buffers,
      const std::vector<std::string>& filenames,
      const std::vector<size_t>& sizes) {
    const uint64_t kPageSize = 4096;
    // create I/O completion port to record progress
    iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    CHECK_NE(iocp, INVALID_HANDLE_VALUE) << "error creating I/O completion port" << std::endl;
    // kick off read of each file
    pending = 0;
    handles.resize(filenames.size());
    for (size_t i = 0; i < filenames.size(); ++i) {
      CHECK_EQ(uint64_t(buffers[i]) & (kPageSize - 1), 0) << "buffer must be page aligned";
      // CHECK_EQ(sizes[i] & (kPageSize - 1), 0) << "size must be a multiple of page size";
      // open file
      HANDLE handle = CreateFileA(
          filenames[i].c_str(),
          GENERIC_READ,
          FILE_SHARE_READ,
          nullptr,
          OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING,
          nullptr);
      CHECK_NE(handle, INVALID_HANDLE_VALUE) << "error opening " << filenames[i];
      // add file to I/O completion port
      CHECK_NE(CreateIoCompletionPort(handle, iocp, 0, 0), INVALID_HANDLE_VALUE)
          << "error adding " << filenames[i] << " to  I/O completion port";
      // build segment table: one segment per per (partial) page + NULL
      std::vector<FILE_SEGMENT_ELEMENT> segments((sizes[i] + kPageSize - 1) / kPageSize + 1);
      for (int page = 0; page < segments.size() - 1; ++page) {
        segments[page].Buffer = static_cast<PVOID64>(buffers[i] + page * kPageSize);
      }
      segments.back().Buffer = NULL;
      OVERLAPPED overlapped = {0};
      BOOL ok = ReadFileScatter(
          handle,
          segments.data(),
          static_cast<ULONG>((segments.size() - 1) * kPageSize),
          nullptr,
          &overlapped);
      if (!ok) { // might just mean ERROR_IO_PENDING
        CHECK_EQ(GetLastError(), ERROR_IO_PENDING) << "error reading " << filenames[i];
        pending += sizes[i];
      }
      // save handle for later
      handles[i] = handle;
    }
  }

  // this function does not returns until all chickens have returned
  void endBatch() {
    while (pending) {
      std::vector<OVERLAPPED_ENTRY> completions(handles.size());
      DWORD count = 0;
      BOOL ok = GetQueuedCompletionStatusEx(
          iocp,
          completions.data(),
          static_cast<ULONG>(completions.size()),
          &count,
          INFINITE,
          FALSE);
      if (!ok) {
        CHECK_EQ(GetLastError(), ERROR_IO_PENDING) << "error retrieving completions";
      }
      for (int i = 0; i < count; ++i) {
        pending -= completions[i].dwNumberOfBytesTransferred;
      }
    }
    // close files and I/O completion port
    for (HANDLE& handle : handles) {
      CloseHandle(handle);
    }
    handles.clear();
    CloseHandle(iocp);
  }

#else

  void beginBatch(
      std::vector<char*>& /*buffers*/,
      const std::vector<std::string>& /*filenames*/,
      const std::vector<size_t>& /*sizes*/) {
    CHECK(0) << "implement med aio_read?";
  }

  void endBatch() {}

#endif

  std::vector<std::thread> threads;
  bool batched;
};

} // namespace fb360_dep
