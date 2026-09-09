 auth-failure rejection.

⏺ Write(/private/tmp/claude-501/-Users-seva-seva-ctf-crusader-dicker/73eeda75-7d4e-4231-9ee3-ba4cc1337cba/scratchpad/test_client.cpp)
Wrote 283 lines to /private/tmp/claude-501/-Users-seva-seva-ctf-crusader-dicker/73eeda75-7d4e-4231-9ee3-ba4cc1337cba/scratchpad/test_client.cpp
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <array>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <lz4frame.h>
#include <zstd.h>

#include "protocol.hpp"
#include "wire.hpp"
#include "hmac_sha256.hpp"

using namespace dicker;

static const std::string kKey = "test-secret-key";
static int g_failures = 0;

static void check(bool condition, const char* label) {
  std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", label);
  if (!condition) {
    g_failures += 1;
  }
}

static int connect_server(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static bool send_all(int fd, const uint8_t* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, data + sent, len - sent);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

static void append_u16(std::vector<uint8_t>& out, uint16_t value) {
  uint8_t buffer[2];
  write_u16_le(buffer, value);
  out.insert(out.end(), buffer, buffer + 2);
}

static void append_u64(std::vector<uint8_t>& out, uint64_t value) {
  uint8_t buffer[8];
  write_u64_le(buffer, value);
  out.insert(out.end(), buffer, buffer + 8);
}

static std::vector<uint8_t> build_handshake(CompressionAlgo algo,
                                            const std::array<uint8_t, 16
                                            const std::string& key,
                                            uint8_t flags) {
  std::vector<uint8_t> out;
  out.insert(out.end(), kMagic.begin(), kMagic.end());
  append_u16(out, kProtocolVersion);
  out.push_back(flags);
  out.push_back(static_cast<uint8_t>(algo));
  out.insert(out.end(), session_id.begin(), session_id.end());

  uint8_t mac[crypto::kSha256DigestSize];
  crypto::hmac_sha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                      out.data(), out.size(), mac);
  out.insert(out.end(), mac, mac + kHmacSize);
  return out;
}

static uint8_t read_status(int fd) {
  uint8_t reply[kHandshakeReplySize];
  size_t got = 0;
  while (got < sizeof(reply)) {
    ssize_t n = read(fd, reply + got, sizeof(reply) - got);
    if (n <= 0) {
      return 0xFF;
    }
    got += static_cast<size_t>(n);
  }
  return reply[kMagic.size()];
}

static std::vector<uint8_t> lz4_compress_unit(LZ4F_cctx* cctx, bool firs
                                              const std::string& content) {
  std::vector<uint8_t> out;
  size_t bound = LZ4F_compressBound(content.size(), nullptr) + 64;
  std::vector<uint8_t> scratch(bound);

  if (first) {
    LZ4F_preferences_t prefs;
    std::memset(&prefs, 0, sizeof(prefs));
    prefs.frameInfo.blockMode = LZ4F_blockLinked;
    size_t n = LZ4F_compressBegin(cctx, scratch.data(), scratch.size(),
    out.insert(out.end(), scratch.data(), scratch.data() + n);
  }

  size_t n = LZ4F_compressUpdate(cctx, scratch.data(), scratch.size(),
                                 content.data(), content.size(), nullptr);
  out.insert(out.end(), scratch.data(), scratch.data() + n);

  n = LZ4F_flush(cctx, scratch.data(), scratch.size(), nullptr);
  out.insert(out.end(), scratch.data(), scratch.data() + n);
  return out;
}

static std::vector<uint8_t> zstd_compress_unit(ZSTD_CCtx* cctx, const std::string& content) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> scratch(ZSTD_CStreamOutSize());
  ZSTD_inBuffer in{content.data(), content.size(), 0};
  size_t remaining = 0;
  do {
    ZSTD_outBuffer chunk{scratch.data(), scratch.size(), 0};
    remaining = ZSTD_compressStream2(cctx, &chunk, &in, ZSTD_e_flush);
    out.insert(out.end(), scratch.data(), scratch.data() + chunk.pos);
  } while (in.pos < in.size || remaining > 0);
  return out;
}

static void send_unit(std::vector<uint8_t>& stream, UnitType type, const
                      uint64_t orig_len, const std::vector<uint8_t>& payload,
                      bool is_chunk, uint64_t offset) {
  stream.push_back(static_cast<uint8_t>(type));
  append_u16(stream, static_cast<uint16_t>(path.size()));
  stream.insert(stream.end(), path.begin(), path.end());
  append_u64(stream, orig_len);
  append_u64(stream, payload.size());
  if (is_chunk) {
    append_u64(stream, offset);
  }
  stream.insert(stream.end(), payload.begin(), payload.end());
}

static std::string read_file(const std::string& path) {
  std::string content;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    return content;
  }
  char buffer[4096];
  size_t n;
  while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    content.append(buffer, n);
  }
  std::fclose(file);
  return content;
}

static std::string make_repetitive(const std::string& seed, int repeats) {
  std::string result;
  for (int i = 0; i < repeats; ++i) {
    result += seed;
  }
  return result;
}

int main(int argc, char** argv) {
  int port = argc > 1 ? std::atoi(argv[1]) : 5959;
  std::string root = argc > 2 ? argv[2] : "/tmp/dicker-test-root";

  std::array<uint8_t, 16> session_lz4;
  session_lz4.fill(0x11);
  std::string hex_lz4(32, '1');

  std::array<uint8_t, 16> session_zstd;
  session_zstd.fill(0x22);
  std::string hex_zstd(32, '2');

  std::string content_a = make_repetitive("the quick brown fox jumps over the lazy dog\n", 40);
  std::string content_b = make_repetitive("the quick brown fox jumps ovetail\n";

  std::printf("Test 1: lz4 two files, cross-file stream\n");
  {
    int fd = connect_server(port);
    check(fd >= 0, "connect");
    auto handshake = build_handshake(CompressionAlgo::Lz4, session_lz4,
    send_all(fd, handshake.data(), handshake.size());
    check(read_status(fd) == 0, "handshake ok");

    LZ4F_cctx* cctx = nullptr;
    LZ4F_createCompressionContext(&cctx, LZ4F_VERSION);
    std::vector<uint8_t> stream;
    auto pa = lz4_compress_unit(cctx, true, content_a);
    send_unit(stream, UnitType::File, "dir/a.txt", content_a.size(), pa,
    auto pb = lz4_compress_unit(cctx, false, content_b);
    send_unit(stream, UnitType::File, "dir/b.txt", content_b.size(), pb,
    stream.push_back(static_cast<uint8_t>(UnitType::End));
    send_all(fd, stream.data(), stream.size());
    LZ4F_freeCompressionContext(cctx);
    close(fd);
    usleep(200000);

    check(read_file(root + "/" + hex_lz4 + "/dir/a.txt") == content_a, "a.txt matches");
    check(read_file(root + "/" + hex_lz4 + "/dir/b.txt") == content_b, "
  }

  std::printf("Test 2: zstd two files, cross-file stream\n");
  {
    int fd = connect_server(port);
    check(fd >= 0, "connect");
    auto handshake = build_handshake(CompressionAlgo::Zstd, session_zstd, kKey, 0);
    send_all(fd, handshake.data(), handshake.size());
    check(read_status(fd) == 0, "handshake ok");

    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3);
    std::vector<uint8_t> stream;
    auto pa = zstd_compress_unit(cctx, content_a);
    send_unit(stream, UnitType::File, "z/a.txt", content_a.size(), pa, false, 0);
    auto pb = zstd_compress_unit(cctx, content_b);
    send_unit(stream, UnitType::File, "z/b.txt", content_b.size(), pb, false, 0);
    stream.push_back(static_cast<uint8_t>(UnitType::End));
    send_all(fd, stream.data(), stream.size());
    ZSTD_freeCCtx(cctx);
    close(fd);
    usleep(200000);

    check(read_file(root + "/" + hex_zstd + "/z/a.txt") == content_a, "a
    check(read_file(root + "/" + hex_zstd + "/z/b.txt") == content_b, "b.txt matches");
  }

  std::printf("Test 3: lz4 chunks written out of order via pwrite\n");
  {
    std::array<uint8_t, 16> session_chunk;
    session_chunk.fill(0x33);
    std::string hex_chunk(32, '3');

    std::string first_half = make_repetitive("AAAA", 64);
    std::string second_half = make_repetitive("BBBB", 64);
    std::string expected = first_half + second_half;

    int fd = connect_server(port);
    check(fd >= 0, "connect");
    auto handshake = build_handshake(CompressionAlgo::Lz4, session_chunk
    send_all(fd, handshake.data(), handshake.size());
    check(read_status(fd) == 0, "handshake ok");

    LZ4F_cctx* cctx = nullptr;
    LZ4F_createCompressionContext(&cctx, LZ4F_VERSION);
    std::vector<uint8_t> stream;
    auto p_second = lz4_compress_unit(cctx, true, second_half);
    send_unit(stream, UnitType::Chunk, "big.bin", second_half.size(), p_ize());
    auto p_first = lz4_compress_unit(cctx, false, first_half);
    send_unit(stream, UnitType::Chunk, "big.bin", first_half.size(), p_f
    stream.push_back(static_cast<uint8_t>(UnitType::End));
    send_all(fd, stream.data(), stream.size());
    LZ4F_freeCompressionContext(cctx);
    close(fd);
    usleep(200000);

    check(read_file(root + "/" + hex_chunk + "/big.bin") == expected, "reassembled big.bin matches");
  }

  std::printf("Test 4: wrong key rejected\n");
  {
    std::array<uint8_t, 16> session_bad;
    session_bad.fill(0x44);
    int fd = connect_server(port);
    check(fd >= 0, "connect");
    auto handshake = build_handshake(CompressionAlgo::Lz4, session_bad,
    send_all(fd, handshake.data(), handshake.size());
    check(read_status(fd) == 3, "auth failed status");
    close(fd);
  }

  std::printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FA
  return g_failures == 0 ? 0 : 1;
