const std = @import("std");
const builtin = @import("builtin");

const minimum_zig_version: std.SemanticVersion = .{ .major = 0, .minor = 16, .patch = 0 };

comptime {
  if (builtin.zig_version.order(minimum_zig_version) == .lt) {
    @compileError("dicker needs zig 0.16.0 or newer, found " ++ builtin.zig_version_string);
  }
}

const client_targets = [_][]const u8{
  "x86_64-macos",
  "aarch64-macos",
  "x86_64-linux-musl",
  "aarch64-linux-musl",
  "x86_64-windows-gnu",
  "aarch64-windows-gnu",
};

const server_targets = [_][]const u8{
  "x86_64-macos",
  "aarch64-macos",
  "x86_64-linux-musl",
  "aarch64-linux-musl",
};

const client_cxx_flags = [_][]const u8{ "-std=c++20", "-Wall", "-ffunction-sections", "-fdata-sections" };
const server_cxx_flags = [_][]const u8{ "-std=c++20", "-Wall", "-ffunction-sections", "-fdata-sections" };
const vendor_c_flags = [_][]const u8{ "-w", "-ffunction-sections", "-fdata-sections" };

const Deps = struct {
  muypoco: *std.Build.Dependency,
  zstd: *std.Build.Dependency,
  lz4: *std.Build.Dependency,
  wolfssl: *std.Build.Dependency,
  blake3: *std.Build.Dependency,
};

const WolfConfig = enum {
  x86_64,
  aarch64_apple,
  aarch64_linux,
  portable,
};

const wolfssl_common_defines = [_][]const u8{
  "WOLFSSL_HAVE_ATOMIC_H",
  "HAVE_C___ATOMIC=1",
  "HAVE_THREAD_LS",
  "NO_DO178",
  "WOLFSSL_ASN_TEMPLATE",
  "ERROR_QUEUE_PER_THREAD",
  "TFM_TIMING_RESISTANT",
  "ECC_TIMING_RESISTANT",
  "WC_RSA_BLINDING",
  "WOLFSSL_USE_ALIGN",
  "WOLFSSL_SHA224",
  "WOLFSSL_SHA512",
  "WOLFSSL_SHA384",
  "HAVE_HKDF",
  "NO_DSA",
  "HAVE_ECC",
  "TFM_ECC256",
  "ECC_SHAMIR",
  "ECC_MIN_KEY_SZ=224",
  "HAVE_CURVE25519",
  "NO_OLD_TLS",
  "WC_RSA_PSS",
  "WOLFSSL_PSS_LONG_SALT",
  "WOLFSSL_SHA3",
  "WOLFSSL_NO_SHAKE128",
  "WOLFSSL_NO_SHAKE256",
  "HAVE_POLY1305",
  "HAVE_CHACHA",
  "HAVE_HASHDRBG",
  "HAVE_TLS_EXTENSIONS",
  "HAVE_SNI",
  "HAVE_ALPN",
  "HAVE_SUPPORTED_CURVES",
  "HAVE_FFDHE_2048",
  "WOLFSSL_TLS13",
  "HAVE_EXTENDED_MASTER",
  "NO_RC4",
  "HAVE_ENCRYPT_THEN_MAC",
  "NO_PSK",
  "NO_MD4",
  "WOLFSSL_SP_MATH_ALL",
  "WC_NO_ASYNC_THREADING",
  "HAVE_DH_DEFAULT_PARAMS",
  "NO_DES3",
  "NO_DES3_TLS_SUITES",
  "GCM_TABLE_4BIT",
  "HAVE_AESGCM",
  "HAVE_SERVER_RENEGOTIATION_INFO",
  "HAVE___UINT128_T=1",
  "WOLFSSL_TEST_STATIC_BUILD",
  "HAVE_WC_INTROSPECTION",
};

const wolfssl_x86_64_defines = [_][]const u8{
  "WOLFSSL_X86_64_BUILD",
  "WOLFSSL_AESNI",
  "USE_INTEL_SPEEDUP",
  "WOLFSSL_ASN_PRINT",
  "WOLFSSL_BASE64_ENCODE",
  "WOLFSSL_SP_X86_64",
};

const wolfssl_aarch64_defines = [_][]const u8{
  "WOLFSSL_ARMASM",
  "WOLFSSL_ARMASM_INLINE",
  "WOLFSSL_NO_HASH_RAW",
  "WOLFSSL_ASN_PRINT",
  "WOLFSSL_SP_ARM64",
  "WOLFSSL_AARCH64_BUILD",
};

const wolfssl_aarch64_apple_defines = [_][]const u8{
  "WOLFSSL_ARMASM_CRYPTO_SHA512",
  "WOLFSSL_ARMASM_CRYPTO_SHA3",
};

const wolfssl_aarch64_linux_defines = [_][]const u8{
  "WOLFSSL_AARCH64_NO_SQRMLSH",
};

const wolfssl_portable_defines = [_][]const u8{
  "TFM_NO_ASM",
  "WOLFSSL_NO_ASM",
  "NO_CHACHA_ASM",
};

const wolfssl_tls_sources = [_][]const u8{
  "src/internal.c",
  "src/keys.c",
  "src/ssl.c",
  "src/tls.c",
  "src/tls13.c",
  "src/wolfio.c",
};

const wolfcrypt_sources = [_][]const u8{
  "wolfcrypt/src/aes.c",
  "wolfcrypt/src/asn.c",
  "wolfcrypt/src/chacha.c",
  "wolfcrypt/src/chacha20_poly1305.c",
  "wolfcrypt/src/coding.c",
  "wolfcrypt/src/cpuid.c",
  "wolfcrypt/src/curve25519.c",
  "wolfcrypt/src/dh.c",
  "wolfcrypt/src/ecc.c",
  "wolfcrypt/src/error.c",
  "wolfcrypt/src/hash.c",
  "wolfcrypt/src/hmac.c",
  "wolfcrypt/src/kdf.c",
  "wolfcrypt/src/logging.c",
  "wolfcrypt/src/md5.c",
  "wolfcrypt/src/memory.c",
  "wolfcrypt/src/pkcs12.c",
  "wolfcrypt/src/poly1305.c",
  "wolfcrypt/src/pwdbased.c",
  "wolfcrypt/src/random.c",
  "wolfcrypt/src/rsa.c",
  "wolfcrypt/src/sha.c",
  "wolfcrypt/src/sha256.c",
  "wolfcrypt/src/sha3.c",
  "wolfcrypt/src/signature.c",
  "wolfcrypt/src/sp_int.c",
  "wolfcrypt/src/wc_encrypt.c",
  "wolfcrypt/src/wc_lms.c",
  "wolfcrypt/src/wc_lms_impl.c",
  "wolfcrypt/src/wc_port.c",
  "wolfcrypt/src/wc_xmss.c",
  "wolfcrypt/src/wc_xmss_impl.c",
  "wolfcrypt/src/wolfmath.c",
};

const wolfcrypt_x86_64_sources = [_][]const u8{
  "wolfcrypt/src/sha512.c",
};

const wolfcrypt_x86_64_assembly = [_][]const u8{
  "wolfcrypt/src/aes_asm.S",
  "wolfcrypt/src/aes_gcm_asm.S",
  "wolfcrypt/src/aes_xts_asm.S",
  "wolfcrypt/src/chacha_asm.S",
  "wolfcrypt/src/fe_x25519_asm.S",
  "wolfcrypt/src/poly1305_asm.S",
  "wolfcrypt/src/sha256_asm.S",
  "wolfcrypt/src/sha3_asm.S",
  "wolfcrypt/src/sha512_asm.S",
};

const wolfcrypt_aarch64_sources = [_][]const u8{
  "wolfcrypt/src/port/arm/armv8-aes.c",
  "wolfcrypt/src/port/arm/armv8-32-aes-asm_c.c",
  "wolfcrypt/src/port/arm/armv8-sha256.c",
  "wolfcrypt/src/port/arm/armv8-32-sha256-asm_c.c",
  "wolfcrypt/src/port/arm/armv8-sha512.c",
  "wolfcrypt/src/port/arm/armv8-sha512-asm_c.c",
  "wolfcrypt/src/port/arm/armv8-32-sha512-asm_c.c",
  "wolfcrypt/src/port/arm/armv8-sha3-asm_c.c",
  "wolfcrypt/src/port/arm/armv8-32-sha3-asm_c.c",
  "wolfcrypt/src/port/arm/armv8-poly1305.c",
  "wolfcrypt/src/port/arm/armv8-32-poly1305-asm_c.c",
  "wolfcrypt/src/port/arm/armv8-chacha.c",
  "wolfcrypt/src/port/arm/armv8-32-chacha-asm_c.c",
  "wolfcrypt/src/port/arm/armv8-curve25519_c.c",
};

const wolfcrypt_portable_sources = [_][]const u8{
  "wolfcrypt/src/sha512.c",
  "wolfcrypt/src/fe_operations.c",
};

const lz4_sources = [_][]const u8{
  "lib/lz4.c",
  "lib/lz4hc.c",
  "lib/lz4frame.c",
  "lib/xxhash.c",
};

const libuv_common_sources = [_][]const u8{
  "fs-poll",
  "idna",
  "inet",
  "random",
  "strscpy",
  "strtok",
  "thread-common",
  "threadpool",
  "timer",
  "uv-common",
  "uv-data-getter-setters",
  "version",
};

const libuv_unix_sources = [_][]const u8{
  "async",
  "core",
  "dl",
  "fs",
  "getaddrinfo",
  "getnameinfo",
  "loop-watcher",
  "loop",
  "pipe",
  "poll",
  "process",
  "random-devurandom",
  "signal",
  "stream",
  "tcp",
  "thread",
  "tty",
  "udp",
};

const libuv_linux_sources = [_][]const u8{
  "proctitle",
  "linux",
  "procfs-exepath",
  "random-getrandom",
  "random-sysctl-linux",
};

const libuv_darwin_sources = [_][]const u8{
  "proctitle",
  "bsd-ifaddrs",
  "kqueue",
  "random-getentropy",
  "darwin-proctitle",
  "darwin",
  "fsevents",
};

const server_sources = [_][]const u8{
  "server/server.cxx",
  "server/connection.cxx",
  "server/tls_layer.cxx",
  "server/byte_channel.cxx",
  "server/file_sink.cxx",
  "server/handshake.cxx",
  "server/decompressor.cxx",
  "server/lz4_decompressor.cxx",
  "server/zstd_decompressor.cxx",
  "server/db.cxx",
  "server/http.cxx",
  "server/archive.cxx",
  "server/router.cxx",
  "server/http_context.cxx",
  "server/stream.cxx",
};

pub fn build(b: *std.Build) void {
  const target = b.standardTargetOptions(.{});
  const debug = b.option(bool, "debug", "dev build: keep symbols, no optimization/strip") orelse false;
  const default_optimize: std.builtin.OptimizeMode = if (debug) .Debug else .ReleaseFast;
  const optimize = b.option(std.builtin.OptimizeMode, "optimize", "Optimization mode (default ReleaseFast, Debug with -Ddebug)") orelse default_optimize;
  const is_root = b.pkg_hash.len == 0;
  const with_server = b.option(bool, "server", "Build the dicker-server library (needs libuv); on by default for this repo") orelse is_root;

  const deps: Deps = .{
    .muypoco = b.dependency("muypoco", .{}),
    .zstd = b.dependency("zstd", .{}),
    .lz4 = b.dependency("lz4", .{}),
    .wolfssl = b.dependency("wolfssl", .{}),
    .blake3 = b.dependency("blake3", .{}),
  };

  const client_library = clientLibrary(b, deps, target, optimize);
  installLibrary(b, client_library, "../client/out");

  const server_library = if (with_server) serverLibrary(b, client_library, target, optimize) else null;

  if (server_library) |library| {
    installLibrary(b, library, "../server/out");
  }

  if (!is_root) {
    return;
  }

  const client_step = b.step("client", "Build the client binaries (client/*.cxx)");
  const server_step = b.step("server", "Build the server binaries (dicker-server, dicker-dbtool)");
  const release_step = b.step("release", "Cross-build the full matrix into client/out and server/out");

  addClientArtifacts(b, client_library, target, optimize, client_step, true);
  b.getInstallStep().dependOn(client_step);

  if (server_library) |library| {
    addServerArtifacts(b, library, target, optimize, server_step, true);
    b.getInstallStep().dependOn(server_step);
  }

  for (client_targets) |triple| {
    const resolved = resolveTriple(b, triple);
    addClientArtifacts(b, clientLibrary(b, deps, resolved, .ReleaseFast), resolved, .ReleaseFast, release_step, false);
  }
  for (server_targets) |triple| {
    const resolved = resolveTriple(b, triple);
    const library = serverLibrary(b, clientLibrary(b, deps, resolved, .ReleaseFast), resolved, .ReleaseFast) orelse continue;
    addServerArtifacts(b, library, resolved, .ReleaseFast, release_step, false);
  }
}

fn installLibrary(b: *std.Build, library: *std.Build.Step.Compile, out_dir: []const u8) void {
  const install = b.addInstallArtifact(library, .{
    .dest_dir = .{ .override = .{ .custom = out_dir } },
    .h_dir = .{ .override = .{ .custom = b.fmt("{s}/include", .{out_dir}) } },
  });
  b.getInstallStep().dependOn(&install.step);
}

fn withCryptoExtensions(b: *std.Build, target: std.Build.ResolvedTarget) std.Build.ResolvedTarget {
  var query = target.query;

  switch (wolfConfig(target)) {
    .x86_64 => {
      query.cpu_features_add.addFeatureSet(std.Target.x86.featureSet(&.{ .aes, .pclmul, .sse4_2 }));
    },
    .aarch64_linux => {
      query.cpu_features_add.addFeatureSet(std.Target.aarch64.featureSet(&.{ .aes, .sha2 }));
    },
    else => {
      return target;
    },
  }

  return b.resolveTargetQuery(query);
}

fn clientLibrary(b: *std.Build, deps: Deps, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) *std.Build.Step.Compile {
  const module = b.createModule(.{ .target = withCryptoExtensions(b, target), .optimize = optimize, .link_libc = true });

  addZstd(module, deps.zstd);
  addLz4(module, deps.lz4);
  addBlake3(b, module, deps.blake3, target);

  const wolfssl_options = addWolfssl(b, module, deps.wolfssl, target);
  const library = b.addLibrary(.{ .name = "dicker", .root_module = module, .linkage = .static });

  library.installHeadersDirectory(b.path("client"), "dicker", .{ .include_extensions = &.{".h"} });
  library.installHeadersDirectory(b.path("shared"), "dicker", .{ .include_extensions = &.{".h"} });
  library.installHeadersDirectory(deps.muypoco.namedLazyPath("include"), "", .{});
  library.installHeader(deps.zstd.path("lib/zstd.h"), "zstd.h");
  library.installHeader(deps.zstd.path("lib/zstd_errors.h"), "zstd_errors.h");
  library.installHeader(deps.lz4.path("lib/lz4.h"), "lz4.h");
  library.installHeader(deps.lz4.path("lib/lz4hc.h"), "lz4hc.h");
  library.installHeader(deps.lz4.path("lib/lz4frame.h"), "lz4frame.h");
  library.installHeader(deps.blake3.path("c/blake3.h"), "blake3.h");
  library.installHeadersDirectory(deps.wolfssl.path("wolfssl"), "wolfssl", .{ .include_extensions = &.{".h"} });
  library.installHeader(wolfssl_options, "wolfssl/options.h");

  return library;
}

fn addZstd(module: *std.Build.Module, zstd: *std.Build.Dependency) void {
  module.addIncludePath(zstd.path("lib"));
  module.addIncludePath(zstd.path("lib/common"));

  for ([_][]const u8{ "lib/common", "lib/compress", "lib/decompress" }) |dir| {
    module.addCSourceFiles(.{
      .root = zstd.path(dir),
      .files = listFiles(zstd.builder, dir, ".c"),
      .flags = &(vendor_c_flags ++ [_][]const u8{"-DZSTD_DISABLE_ASM"}),
      .language = .c,
    });
  }
}

fn addLz4(module: *std.Build.Module, lz4: *std.Build.Dependency) void {
  module.addIncludePath(lz4.path("lib"));
  module.addCSourceFiles(.{
    .root = lz4.path(""),
    .files = &lz4_sources,
    .flags = &vendor_c_flags,
    .language = .c,
  });
}

fn addBlake3(b: *std.Build, module: *std.Build.Module, blake3: *std.Build.Dependency, target: std.Build.ResolvedTarget) void {
  const result = target.result;
  var files: std.ArrayList([]const u8) = .empty;
  var flags: std.ArrayList([]const u8) = .empty;

  files.appendSlice(b.allocator, &.{ "c/blake3.c", "c/blake3_dispatch.c", "c/blake3_portable.c" }) catch @panic("OOM");
  flags.appendSlice(b.allocator, &vendor_c_flags) catch @panic("OOM");

  switch (result.cpu.arch) {
    .x86_64 => {
      const flavor = if (result.os.tag == .windows) "windows_gnu" else "unix";
      for ([_][]const u8{ "sse2", "sse41", "avx2", "avx512" }) |extension| {
        files.append(b.allocator, b.fmt("c/blake3_{s}_x86-64_{s}.S", .{ extension, flavor })) catch @panic("OOM");
      }
    },
    .aarch64 => {
      files.append(b.allocator, "c/blake3_neon.c") catch @panic("OOM");
      flags.append(b.allocator, "-DBLAKE3_USE_NEON=1") catch @panic("OOM");
    },
    else => {
      flags.appendSlice(b.allocator, &.{ "-DBLAKE3_NO_SSE2", "-DBLAKE3_NO_SSE41", "-DBLAKE3_NO_AVX2", "-DBLAKE3_NO_AVX512" }) catch @panic("OOM");
    },
  }

  module.addIncludePath(blake3.path("c"));
  module.addCSourceFiles(.{ .root = blake3.path(""), .files = files.items, .flags = flags.items });
}

fn wolfConfig(target: std.Build.ResolvedTarget) WolfConfig {
  const result = target.result;

  return switch (result.cpu.arch) {
    .x86_64 => if (result.os.tag == .windows) .portable else .x86_64,
    .aarch64 => switch (result.os.tag) {
      .macos, .ios => .aarch64_apple,
      .linux => .aarch64_linux,
      else => .portable,
    },
    else => .portable,
  };
}

fn wolfDefines(b: *std.Build, config: WolfConfig) []const []const u8 {
  var defines: std.ArrayList([]const u8) = .empty;

  defines.appendSlice(b.allocator, &wolfssl_common_defines) catch @panic("OOM");

  switch (config) {
    .x86_64 => {
      defines.appendSlice(b.allocator, &wolfssl_x86_64_defines) catch @panic("OOM");
    },
    .aarch64_apple => {
      defines.appendSlice(b.allocator, &wolfssl_aarch64_defines) catch @panic("OOM");
      defines.appendSlice(b.allocator, &wolfssl_aarch64_apple_defines) catch @panic("OOM");
    },
    .aarch64_linux => {
      defines.appendSlice(b.allocator, &wolfssl_aarch64_defines) catch @panic("OOM");
      defines.appendSlice(b.allocator, &wolfssl_aarch64_linux_defines) catch @panic("OOM");
    },
    .portable => {
      defines.appendSlice(b.allocator, &wolfssl_portable_defines) catch @panic("OOM");
    },
  }

  return defines.items;
}

fn wolfOptionsHeader(b: *std.Build, defines: []const []const u8) []const u8 {
  var content: std.ArrayList(u8) = .empty;

  content.appendSlice(b.allocator, "#ifndef WOLFSSL_OPTIONS_H\n#define WOLFSSL_OPTIONS_H\n\n") catch @panic("OOM");

  for (defines) |define| {
    const name_end = std.mem.indexOfScalar(u8, define, '=') orelse define.len;
    const name = define[0..name_end];
    const value = if (name_end < define.len) define[name_end + 1 ..] else "";

    content.print(b.allocator, "#undef {s}\n#define {s} {s}\n\n", .{ name, name, value }) catch @panic("OOM");
  }

  content.appendSlice(b.allocator, "#endif\n") catch @panic("OOM");
  return content.items;
}

fn addWolfssl(b: *std.Build, module: *std.Build.Module, wolfssl: *std.Build.Dependency, target: std.Build.ResolvedTarget) std.Build.LazyPath {
  const config = wolfConfig(target);
  const defines = wolfDefines(b, config);

  var flags: std.ArrayList([]const u8) = .empty;
  flags.appendSlice(b.allocator, &vendor_c_flags) catch @panic("OOM");

  for (defines) |define| {
    flags.append(b.allocator, b.fmt("-D{s}", .{define})) catch @panic("OOM");
  }

  const generated = b.addWriteFiles();
  const options = generated.add("wolfssl/options.h", wolfOptionsHeader(b, defines));

  module.addIncludePath(generated.getDirectory());
  module.addIncludePath(wolfssl.path(""));

  var sources: std.ArrayList([]const u8) = .empty;
  sources.appendSlice(b.allocator, &wolfssl_tls_sources) catch @panic("OOM");
  sources.appendSlice(b.allocator, &wolfcrypt_sources) catch @panic("OOM");

  switch (config) {
    .x86_64 => {
      sources.appendSlice(b.allocator, &wolfcrypt_x86_64_sources) catch @panic("OOM");
      sources.appendSlice(b.allocator, &wolfcrypt_x86_64_assembly) catch @panic("OOM");
    },
    .aarch64_apple, .aarch64_linux => {
      sources.appendSlice(b.allocator, &wolfcrypt_aarch64_sources) catch @panic("OOM");
    },
    .portable => {
      sources.appendSlice(b.allocator, &wolfcrypt_portable_sources) catch @panic("OOM");
    },
  }

  module.addCSourceFiles(.{
    .root = wolfssl.path(""),
    .files = sources.items,
    .flags = flags.items,
  });

  return options;
}

fn libuvLibrary(b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) ?*std.Build.Step.Compile {
  const libuv = b.lazyDependency("libuv", .{}) orelse return null;
  const os = target.result.os.tag;
  const uv = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });

  uv.addIncludePath(libuv.path("include"));
  uv.addIncludePath(libuv.path("src"));

  var files: std.ArrayList([]const u8) = .empty;
  var flags: std.ArrayList([]const u8) = .empty;

  for (libuv_common_sources) |name| {
    files.append(b.allocator, b.fmt("src/{s}.c", .{name})) catch @panic("OOM");
  }
  for (libuv_unix_sources) |name| {
    files.append(b.allocator, b.fmt("src/unix/{s}.c", .{name})) catch @panic("OOM");
  }

  flags.appendSlice(b.allocator, &vendor_c_flags) catch @panic("OOM");
  flags.appendSlice(b.allocator, &.{ "-D_FILE_OFFSET_BITS=64", "-D_LARGEFILE_SOURCE" }) catch @panic("OOM");

  if (os == .linux) {
    flags.appendSlice(b.allocator, &.{ "-D_GNU_SOURCE", "-D_POSIX_C_SOURCE=200112" }) catch @panic("OOM");
    for (libuv_linux_sources) |name| {
      files.append(b.allocator, b.fmt("src/unix/{s}.c", .{name})) catch @panic("OOM");
    }
  }
  else {
    flags.appendSlice(b.allocator, &.{ "-D_DARWIN_UNLIMITED_SELECT=1", "-D_DARWIN_USE_64_BIT_INODE=1" }) catch @panic("OOM");
    for (libuv_darwin_sources) |name| {
      files.append(b.allocator, b.fmt("src/unix/{s}.c", .{name})) catch @panic("OOM");
    }
  }

  uv.addCSourceFiles(.{ .root = libuv.path(""), .files = files.items, .flags = flags.items, .language = .c });

  const library = b.addLibrary(.{ .name = "uv", .root_module = uv, .linkage = .static });
  library.installHeadersDirectory(libuv.path("include"), "", .{});
  return library;
}

fn serverLibrary(
  b: *std.Build,
  client_library: *std.Build.Step.Compile,
  target: std.Build.ResolvedTarget,
  optimize: std.builtin.OptimizeMode,
) ?*std.Build.Step.Compile {
  const uv = libuvLibrary(b, target, optimize) orelse return null;
  const module = b.createModule(.{ .target = target, .optimize = optimize, .link_libcpp = true });

  module.addCSourceFiles(.{ .files = &server_sources, .flags = &server_cxx_flags, .language = .cpp });
  module.addCSourceFiles(.{ .files = listPaths(b, "server/controllers", ".cxx"), .flags = &server_cxx_flags, .language = .cpp });
  module.addCSourceFiles(.{ .files = listPaths(b, "server/middleware", ".cxx"), .flags = &server_cxx_flags, .language = .cpp });
  module.addIncludePath(b.path("server"));
  module.addIncludePath(b.path("shared"));
  module.addIncludePath(b.path("cxx_modules"));
  module.linkLibrary(client_library);
  module.linkLibrary(uv);

  const library = b.addLibrary(.{ .name = "dicker-server", .root_module = module, .linkage = .static });

  library.installHeadersDirectory(b.path("server"), "dicker", .{ .include_extensions = &.{".h"} });
  library.installHeadersDirectory(b.path("shared"), "dicker", .{ .include_extensions = &.{".h"} });
  library.installLibraryHeaders(client_library);
  library.installLibraryHeaders(uv);

  return library;
}

fn addClientArtifacts(
  b: *std.Build,
  library: *std.Build.Step.Compile,
  target: std.Build.ResolvedTarget,
  optimize: std.builtin.OptimizeMode,
  step: *std.Build.Step,
  examples: bool,
) void {
  for (listFiles(b, "client", ".cxx")) |file| {
    const stem = std.fs.path.stem(file);

    if (!examples and std.mem.eql(u8, stem, "example")) {
      continue;
    }

    const name = if (std.mem.eql(u8, stem, "cli")) "dicker" else stem;
    const executable = clientExecutable(b, library, target, optimize, name, b.fmt("client/{s}", .{file}));

    step.dependOn(installStep(b, executable, target, "../client/out"));
  }
}

fn clientExecutable(
  b: *std.Build,
  library: *std.Build.Step.Compile,
  target: std.Build.ResolvedTarget,
  optimize: std.builtin.OptimizeMode,
  name: []const u8,
  source: []const u8,
) *std.Build.Step.Compile {
  const module = b.createModule(.{ .target = target, .optimize = optimize, .link_libcpp = true });

  module.strip = optimize != .Debug;
  module.addCSourceFiles(.{ .files = &.{source}, .flags = &client_cxx_flags, .language = .cpp });
  module.linkLibrary(library);
  linkClientSystemLibraries(b, module, target);

  return b.addExecutable(.{ .name = name, .root_module = module });
}

fn linkClientSystemLibraries(b: *std.Build, module: *std.Build.Module, target: std.Build.ResolvedTarget) void {
  switch (target.result.os.tag) {
    .windows => {
      for ([_][]const u8{ "ws2_32", "ole32", "oleaut32", "crypt32", "gdi32", "user32", "advapi32" }) |name| {
        module.linkSystemLibrary(name, .{});
      }
      if (target.result.cpu.arch == .aarch64) {
        const definition = b.graph.zig_lib_directory.join(b.allocator, &.{ "libc", "mingw", "lib64", "vssapi.def" }) catch @panic("OOM");
        const dlltool = b.addSystemCommand(&.{ b.graph.zig_exe, "dlltool", "-m", "arm64", "-d", definition, "-l" });
        const import_library = dlltool.addOutputFileArg("libvssapi.a");
        module.addLibraryPath(import_library.dirname());
      }
      module.linkSystemLibrary("vssapi", .{});
    },
    .linux => {
      for ([_][]const u8{ "dl", "rt" }) |name| {
        module.linkSystemLibrary(name, .{});
      }
    },
    else => {},
  }
}

fn addServerArtifacts(
  b: *std.Build,
  library: *std.Build.Step.Compile,
  target: std.Build.ResolvedTarget,
  optimize: std.builtin.OptimizeMode,
  step: *std.Build.Step,
  examples: bool,
) void {
  const binaries = [_][2][]const u8{
    .{ "dicker-server", "server/main.cxx" },
    .{ "dicker-dbtool", "server/dbtool.cxx" },
    .{ "example", "server/example.cxx" },
  };

  for (binaries[0 .. if (examples) binaries.len else binaries.len - 1]) |binary| {
    const module = b.createModule(.{ .target = target, .optimize = optimize, .link_libcpp = true });

    module.strip = optimize != .Debug;
    module.addCSourceFiles(.{ .files = &.{binary[1]}, .flags = &server_cxx_flags, .language = .cpp });
    module.addIncludePath(b.path("server"));
    module.addIncludePath(b.path("shared"));
    module.linkLibrary(library);
    module.linkSystemLibrary("pthread", .{});

    if (target.result.os.tag == .linux) {
      for ([_][]const u8{ "dl", "rt" }) |name| {
        module.linkSystemLibrary(name, .{});
      }
    }

    step.dependOn(installStep(b, b.addExecutable(.{ .name = binary[0], .root_module = module }), target, "../server/out"));
  }
}

fn installStep(b: *std.Build, executable: *std.Build.Step.Compile, target: std.Build.ResolvedTarget, out_dir: []const u8) *std.Build.Step {
  const extension: []const u8 = if (target.result.os.tag == .windows) ".exe" else "";
  const sub_path = if (target.query.isNative())
    b.fmt("{s}{s}", .{ executable.name, extension })
  else
    b.fmt("{s}-{s}{s}", .{ executable.name, tripleSuffix(b, target), extension });

  const install = b.addInstallArtifact(executable, .{
    .dest_dir = .{ .override = .{ .custom = out_dir } },
    .dest_sub_path = sub_path,
  });
  return &install.step;
}

fn resolveTriple(b: *std.Build, triple: []const u8) std.Build.ResolvedTarget {
  const query = std.Target.Query.parse(.{ .arch_os_abi = triple }) catch @panic("bad triple");
  return b.resolveTargetQuery(query);
}

fn tripleSuffix(b: *std.Build, target: std.Build.ResolvedTarget) []const u8 {
  if (target.query.isNative()) {
    return "host";
  }
  return target.query.zigTriple(b.allocator) catch @panic("OOM");
}

fn listFiles(b: *std.Build, dir_relative: []const u8, extension: []const u8) []const []const u8 {
  const io = b.graph.io;
  var found: std.ArrayList([]const u8) = .empty;
  var dir = b.build_root.handle.openDir(io, dir_relative, .{ .iterate = true }) catch |failure| std.debug.panic("open {s}: {}", .{ dir_relative, failure });
  defer dir.close(io);

  var it = dir.iterate();

  while (it.next(io) catch @panic("iterate")) |entry| {
    if (entry.kind != .file or !std.mem.endsWith(u8, entry.name, extension)) {
      continue;
    }
    found.append(b.allocator, b.dupe(entry.name)) catch @panic("OOM");
  }
  return found.items;
}

fn listPaths(b: *std.Build, dir_relative: []const u8, extension: []const u8) []const []const u8 {
  var paths: std.ArrayList([]const u8) = .empty;

  for (listFiles(b, dir_relative, extension)) |name| {
    paths.append(b.allocator, b.fmt("{s}/{s}", .{ dir_relative, name })) catch @panic("OOM");
  }
  return paths.items;
}
