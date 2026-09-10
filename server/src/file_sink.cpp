#include "file_sink.hpp"

#include <algorithm>
#include <cctype>

namespace dicker {

  namespace {

    constexpr std::size_t kMaxSingleWrite = 1u << 30;
    constexpr int kFileMode = 0644;

    struct FsRequest {
      uv_fs_t request{};

      FsRequest() = default;
      ~FsRequest() {
        uv_fs_req_cleanup(&request);
      }

      FsRequest(const FsRequest&) = delete;
      FsRequest& operator=(const FsRequest&) = delete;

      uv_fs_t* get() {
        return &request;
      }
    };

  }

  bool write_fd_all(uv_loop_t* loop, uv_file fd, const std::uint8_t* data, std::size_t len, std::int64_t offset) {
    std::size_t written = 0;
    while (written < len) {
      std::size_t remaining = len - written;
      std::size_t take = remaining < kMaxSingleWrite ? remaining : kMaxSingleWrite;
      char* base = const_cast<char*>(reinterpret_cast<const char*>(data + written));
      uv_buf_t buffer = uv_buf_init(base, static_cast<unsigned int>(take));
      std::int64_t write_offset = offset < 0 ? -1 : offset + static_cast<std::int64_t>(written);

      FsRequest request;
      int result = uv_fs_write(loop, request.get(), fd, &buffer, 1, write_offset, nullptr);
      if (result <= 0) {
        return false;
      }
      written += static_cast<std::size_t>(result);
    }
    return true;
  }

  bool SequentialWriter::write(const std::uint8_t* data, std::size_t len) {
    if (!write_fd_all(loop, fd, data, len, offset)) {
      return false;
    }
    offset += static_cast<std::int64_t>(len);
    return true;
  }

  void SequentialWriter::close() {
    if (fd >= 0) {
      FsRequest request;
      uv_fs_close(loop, request.get(), fd, nullptr);
      fd = -1;
    }
  }

  bool OffsetWriter::write_at(std::uint64_t offset, const std::uint8_t* data, std::size_t len) {
    return write_fd_all(loop, file->fd, data, len, static_cast<std::int64_t>(offset));
  }

  FileSink::FileSink(uv_loop_t* loop, std::filesystem::path session_root)
    : loop_(loop),
      session_root_(std::move(session_root)) {
  }

  FileSink::~FileSink() {
    close_all();
  }

  bool FileSink::resolve_safe_path(const std::string& relative_path, std::filesystem::path& out) {
    std::string normalized = relative_path;
    for (char& character : normalized) {
      if (character == '\\') {
        character = '/';
      }
    }

    // Every incoming path is treated as relative to the session dir: an absolute
    // path /a/b/c (or a Windows C:\a\b from the client) maps to <session>/a/b/c.
    // Strip a leading drive letter and any leading slashes, then confine. The
    // ".." rejection and the prefix check below are what keep it from escaping.
    std::size_t start = 0;
    if (normalized.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(normalized[0])) && normalized[1] == ':') {
      start = 2;
    }
    while (start < normalized.size() && normalized[start] == '/') {
      start += 1;
    }
    normalized.erase(0, start);

    std::filesystem::path candidate(normalized);

    std::filesystem::path lexical = candidate.lexically_normal();
    for (const std::filesystem::path& part : lexical) {
      if (part == "..") {
        return false;
      }
    }
    if (lexical.empty() || lexical == ".") {
      return false;
    }

    std::filesystem::path root_normal = session_root_.lexically_normal();
    std::filesystem::path full = (root_normal / lexical).lexically_normal();

    auto mismatch = std::mismatch(root_normal.begin(), root_normal.end(), full.begin(), full.end());
    if (mismatch.first != root_normal.end()) {
      return false;
    }

    out = full;
    return true;
  }

  bool FileSink::ensure_parent_directory(const std::filesystem::path& path) {
    std::filesystem::path parent = path.parent_path();
    if (parent.empty()) {
      return true;
    }
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (!error) {
      return true;
    }
    return std::filesystem::exists(parent);
  }

  std::shared_ptr<ChunkFile> FileSink::acquire_chunk_file(const std::filesystem::path& path) {
    std::string key = path.string();
    std::lock_guard<std::mutex> lock(registry_mutex_);

    auto existing = chunk_files_.find(key);
    if (existing != chunk_files_.end()) {
      return existing->second;
    }

    if (!ensure_parent_directory(path)) {
      return nullptr;
    }

    FsRequest open_request;
    int fd = uv_fs_open(loop_, open_request.get(), key.c_str(),
                        UV_FS_O_WRONLY | UV_FS_O_CREAT, kFileMode, nullptr);
    if (fd < 0) {
      return nullptr;
    }

    auto file = std::make_shared<ChunkFile>();
    file->fd = fd;
    chunk_files_.emplace(key, file);
    return file;
  }

  SequentialWriter FileSink::open_sequential(const std::string& relative_path) {
    std::filesystem::path full;
    if (!resolve_safe_path(relative_path, full)) {
      return SequentialWriter{};
    }
    if (!ensure_parent_directory(full)) {
      return SequentialWriter{};
    }

    FsRequest open_request;
    int fd = uv_fs_open(loop_, open_request.get(), full.string().c_str(),
                        UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, kFileMode, nullptr);
    if (fd < 0) {
      return SequentialWriter{};
    }

    SequentialWriter writer;
    writer.loop = loop_;
    writer.fd = fd;
    writer.offset = 0;
    return writer;
  }

  OffsetWriter FileSink::open_offset(const std::string& relative_path) {
    std::filesystem::path full;
    if (!resolve_safe_path(relative_path, full)) {
      return OffsetWriter{};
    }

    std::shared_ptr<ChunkFile> file = acquire_chunk_file(full);
    if (!file) {
      return OffsetWriter{};
    }

    OffsetWriter writer;
    writer.loop = loop_;
    writer.file = file;
    return writer;
  }

  void FileSink::close_all() {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    for (auto& entry : chunk_files_) {
      FsRequest fsync_request;
      uv_fs_fsync(loop_, fsync_request.get(), entry.second->fd, nullptr);
      FsRequest close_request;
      uv_fs_close(loop_, close_request.get(), entry.second->fd, nullptr);
    }
    chunk_files_.clear();
  }

}
