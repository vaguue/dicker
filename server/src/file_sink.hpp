#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <uv.h>

namespace dicker {

  struct ChunkFile {
    uv_file fd;
  };

  bool write_fd_all(uv_loop_t* loop, uv_file fd, const std::uint8_t* data, std::size_t len, std::int64_t offset);

  struct SequentialWriter {
    uv_loop_t* loop = nullptr;
    uv_file fd = -1;
    std::int64_t offset = 0;

    bool valid() const {
      return fd >= 0;
    }
    bool write(const std::uint8_t* data, std::size_t len);
    void close();
  };

  struct OffsetWriter {
    uv_loop_t* loop = nullptr;
    std::shared_ptr<ChunkFile> file;

    bool valid() const {
      return file != nullptr;
    }
    bool write_at(std::uint64_t offset, const std::uint8_t* data, std::size_t len);
  };

  struct FileSink {
    FileSink(uv_loop_t* loop, std::filesystem::path session_root);
    ~FileSink();

    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;

    SequentialWriter open_sequential(const std::string& relative_path);
    OffsetWriter open_offset(const std::string& relative_path);
    void close_all();

    bool resolve_safe_path(const std::string& relative_path, std::filesystem::path& out);
    bool ensure_parent_directory(const std::filesystem::path& path);
    std::shared_ptr<ChunkFile> acquire_chunk_file(const std::filesystem::path& path);

    uv_loop_t* loop_;
    std::filesystem::path session_root_;
    std::mutex registry_mutex_;
    std::unordered_map<std::string, std::shared_ptr<ChunkFile>> chunk_files_;
  };

}
