const std = @import("std");

const WOLFSSL_VER = "5.7.4";

const client_targets = [_][]const u8{
    "x86_64-macos",       "aarch64-macos",       "x86_64-linux-musl", "aarch64-linux-musl",
    "x86_64-windows-gnu", "aarch64-windows-gnu",
};
const server_targets = [_][]const u8{
    "x86_64-macos", "aarch64-macos", "x86_64-linux-musl", "aarch64-linux-musl",
};

const ClientBin = struct { base: []const u8, name: []const u8, in_default: bool = true, win_only: bool = false };
const client_custom = [_]ClientBin{
    .{ .base = "cli", .name = "dicker" },
    .{ .base = "deploy", .name = "deploy", .in_default = false },
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const debug = b.option(bool, "debug", "dev build: keep symbols, no optimization/strip") orelse false;
    const optimize: std.builtin.OptimizeMode = if (debug) .Debug else .ReleaseFast;

    const client_step = b.step("client", "Build all client binaries (glob of client/*.cxx)");
    const server_step = b.step("server", "Build the server binaries (dicker-server, dicker-dbtool)");
    const release_step = b.step("release", "Cross-build the full matrix into client/out and server/out");

    addClientArtifacts(b, target, optimize, client_step, b.getInstallStep());
    addServerArtifacts(b, target, optimize, server_step);
    b.getInstallStep().dependOn(server_step);

    for (client_targets) |triple| {
        const rt = resolveTriple(b, triple);
        addClientArtifacts(b, rt, .ReleaseFast, release_step, null);
    }
    for (server_targets) |triple| {
        const rt = resolveTriple(b, triple);
        addServerArtifacts(b, rt, .ReleaseFast, release_step);
    }
}

fn addClientArtifacts(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    all_step: *std.Build.Step,
    default_step: ?*std.Build.Step,
) void {
    const os = target.result.os.tag;

    for (glob(b, "client", ".cxx")) |src| {
        const base = std.fs.path.stem(src);
        var spec = ClientBin{ .base = base, .name = base };
        for (client_custom) |c| {
            if (std.mem.eql(u8, c.base, base)) spec = c;
        }
        if (spec.win_only and os != .windows) continue;

        const exe = buildClientExe(b, target, optimize, spec.name, src);
        const ai = installStep(b, exe, target, "../client/out");
        all_step.dependOn(ai);
        if (default_step) |ds| {
            if (spec.in_default) ds.dependOn(ai);
        }
    }
}

fn buildClientExe(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    name: []const u8,
    src: []const u8,
) *std.Build.Step.Compile {
    const mod = b.createModule(.{ .target = target, .optimize = optimize });
    mod.link_libcpp = true; // pulls in libc too
    mod.strip = optimize != .Debug;

    mod.addCSourceFiles(.{
        .files = &.{src},
        .flags = &.{ "-std=c++20", "-O2", "-Wall", "-ffunction-sections", "-fdata-sections" },
        .language = .cpp,
    });

    const llhttp_gen = generateLlhttp(b);
    mod.addIncludePath(b.path("cxx_modules/llhttp/build"));
    mod.addIncludePath(b.path("cxx_modules/llhttp/src/native"));
    mod.addCSourceFiles(.{
        .files = &.{
            "cxx_modules/llhttp/build/c/llhttp.c",
            "cxx_modules/llhttp/src/native/api.c",
            "cxx_modules/llhttp/src/native/http.c",
        },
        .flags = &.{ "-O2", "-ffunction-sections", "-fdata-sections" },
        .language = .c,
    });

    mod.addIncludePath(b.path("client"));
    mod.addIncludePath(b.path("shared"));
    mod.addIncludePath(b.path("cxx_modules"));
    mod.addIncludePath(b.path("cxx_modules/lz4"));
    mod.addIncludePath(b.path("cxx_modules/zstd/lib"));

    linkVendor(b, mod, target, optimize);
    const wolf = wolfssl(b, target);
    mod.addIncludePath(.{ .cwd_relative = wolf.include_dir });

    const exe = b.addExecutable(.{ .name = name, .root_module = mod });
    if (llhttp_gen) |s| exe.step.dependOn(s);
    if (wolf.step) |s| exe.step.dependOn(s);
    mod.addLibraryPath(.{ .cwd_relative = wolf.lib_dir });
    mod.linkSystemLibrary("wolfssl", .{});

    switch (target.result.os.tag) {
        .windows => {
            for ([_][]const u8{ "ws2_32", "ole32", "oleaut32", "crypt32", "gdi32", "user32", "advapi32" }) |l|
                mod.linkSystemLibrary(l, .{});
            if (target.result.cpu.arch == .aarch64) {
                const def = b.graph.zig_lib_directory.join(b.allocator, &.{ "libc", "mingw", "lib64", "vssapi.def" }) catch @panic("OOM");
                const dt = b.addSystemCommand(&.{ b.graph.zig_exe, "dlltool", "-m", "arm64", "-d", def, "-l" });
                const vlib = dt.addOutputFileArg("libvssapi.a");
                mod.addLibraryPath(vlib.dirname());
            }
            mod.linkSystemLibrary("vssapi", .{});
        },
        .linux => for ([_][]const u8{ "dl", "rt" }) |l| mod.linkSystemLibrary(l, .{}),
        else => {},
    }

    return exe;
}

fn addServerArtifacts(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    step: *std.Build.Step,
) void {
    const os = target.result.os.tag;
    const wolf = wolfssl(b, target);

    {
        const mod = b.createModule(.{ .target = target, .optimize = optimize });
        mod.link_libcpp = true;
        mod.strip = optimize != .Debug;
        const server_flags = &.{ "-std=c++17", "-O2", "-Wall", "-ffunction-sections", "-fdata-sections" };
        mod.addCSourceFiles(.{
            .files = &.{
                "server/main.cxx",              "server/server.cxx",       "server/connection.cxx",
                "server/tls_layer.cxx",         "server/byte_channel.cxx", "server/file_sink.cxx",
                "server/handshake.cxx",         "server/decompressor.cxx", "server/lz4_decompressor.cxx",
                "server/zstd_decompressor.cxx", "server/db.cxx",           "server/http.cxx",
                "server/archive.cxx",           "server/router.cxx",       "server/http_context.cxx",
            },
            .flags = server_flags,
            .language = .cpp,
        });
        // Controllers and middleware are drop-in: any .cxx under these dirs is
        // compiled automatically (still register it in router.cxx).
        mod.addCSourceFiles(.{ .files = glob(b, "server/controllers", ".cxx"), .flags = server_flags, .language = .cpp });
        mod.addCSourceFiles(.{ .files = glob(b, "server/middleware", ".cxx"), .flags = server_flags, .language = .cpp });
        mod.addIncludePath(b.path("server"));
        mod.addIncludePath(b.path("cxx_modules"));
        mod.addIncludePath(b.path("shared"));
        mod.addIncludePath(b.path("cxx_modules/libuv/include"));
        mod.addIncludePath(b.path("cxx_modules/lz4"));
        mod.addIncludePath(b.path("cxx_modules/zstd/lib"));
        mod.addIncludePath(.{ .cwd_relative = wolf.include_dir });

        linkVendor(b, mod, target, optimize);
        linkLibuv(b, mod, target, optimize);

        const exe = b.addExecutable(.{ .name = "dicker-server", .root_module = mod });
        if (wolf.step) |s| exe.step.dependOn(s);
        mod.addLibraryPath(.{ .cwd_relative = wolf.lib_dir });
        mod.linkSystemLibrary("wolfssl", .{});
        mod.linkSystemLibrary("pthread", .{});
        if (os == .linux) for ([_][]const u8{ "dl", "rt" }) |l| mod.linkSystemLibrary(l, .{});

        step.dependOn(installStep(b, exe, target, "../server/out"));
    }

    {
        const mod = b.createModule(.{ .target = target, .optimize = optimize });
        mod.link_libcpp = true;
        mod.strip = optimize != .Debug;
        mod.addCSourceFiles(.{
            .files = &.{ "server/dbtool.cxx", "server/db.cxx" },
            .flags = &.{ "-std=c++17", "-O2", "-Wall", "-ffunction-sections", "-fdata-sections" },
            .language = .cpp,
        });
        mod.addIncludePath(b.path("cxx_modules"));
        mod.addIncludePath(b.path("shared"));
        mod.addIncludePath(.{ .cwd_relative = wolf.include_dir });

        const exe = b.addExecutable(.{ .name = "dicker-dbtool", .root_module = mod });
        if (wolf.step) |s| exe.step.dependOn(s);
        mod.addLibraryPath(.{ .cwd_relative = wolf.lib_dir });
        mod.linkSystemLibrary("wolfssl", .{});
        mod.linkSystemLibrary("pthread", .{});
        if (os == .linux) for ([_][]const u8{ "dl", "rt" }) |l| mod.linkSystemLibrary(l, .{});

        step.dependOn(installStep(b, exe, target, "../server/out"));
    }
}

fn linkVendor(b: *std.Build, mod: *std.Build.Module, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) void {
    const lib_mod = b.createModule(.{ .target = target, .optimize = optimize });
    lib_mod.link_libc = true;

    lib_mod.addIncludePath(b.path("cxx_modules/lz4"));
    lib_mod.addCSourceFiles(.{
        .files = &.{ "cxx_modules/lz4/lz4.c", "cxx_modules/lz4/lz4hc.c", "cxx_modules/lz4/lz4frame.c", "cxx_modules/lz4/xxhash.c" },
        .flags = &.{ "-O2", "-ffunction-sections", "-fdata-sections" },
        .language = .c,
    });

    lib_mod.addIncludePath(b.path("cxx_modules/zstd/lib"));
    lib_mod.addIncludePath(b.path("cxx_modules/zstd/lib/common"));
    for ([_][]const u8{ "cxx_modules/zstd/lib/common", "cxx_modules/zstd/lib/compress", "cxx_modules/zstd/lib/decompress" }) |dir| {
        lib_mod.addCSourceFiles(.{
            .files = glob(b, dir, ".c"),
            .flags = &.{ "-O2", "-DZSTD_DISABLE_ASM", "-ffunction-sections", "-fdata-sections" },
            .language = .c,
        });
    }

    const lib = b.addLibrary(.{ .name = "vendor", .root_module = lib_mod, .linkage = .static });
    mod.linkLibrary(lib);
}

fn linkLibuv(b: *std.Build, mod: *std.Build.Module, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) void {
    const os = target.result.os.tag;
    const uv = b.createModule(.{ .target = target, .optimize = optimize });
    uv.link_libc = true;
    uv.addIncludePath(b.path("cxx_modules/libuv/include"));
    uv.addIncludePath(b.path("cxx_modules/libuv/src"));

    var files = std.array_list.Managed([]const u8).init(b.allocator);
    const base = [_][]const u8{ "fs-poll", "idna", "inet", "random", "strscpy", "strtok", "thread-common", "threadpool", "timer", "uv-common", "uv-data-getter-setters", "version" };
    const unix = [_][]const u8{ "async", "core", "dl", "fs", "getaddrinfo", "getnameinfo", "loop-watcher", "loop", "pipe", "poll", "process", "random-devurandom", "signal", "stream", "tcp", "thread", "tty", "udp" };
    for (base) |f| files.append(b.fmt("cxx_modules/libuv/src/{s}.c", .{f})) catch @panic("OOM");
    for (unix) |f| files.append(b.fmt("cxx_modules/libuv/src/unix/{s}.c", .{f})) catch @panic("OOM");

    var defs = std.array_list.Managed([]const u8).init(b.allocator);
    defs.appendSlice(&.{ "-O2", "-ffunction-sections", "-fdata-sections", "-D_FILE_OFFSET_BITS=64", "-D_LARGEFILE_SOURCE" }) catch @panic("OOM");
    if (os == .linux) {
        defs.appendSlice(&.{ "-D_GNU_SOURCE", "-D_POSIX_C_SOURCE=200112" }) catch @panic("OOM");
        for ([_][]const u8{ "proctitle", "linux", "procfs-exepath", "random-getrandom", "random-sysctl-linux" }) |f|
            files.append(b.fmt("cxx_modules/libuv/src/unix/{s}.c", .{f})) catch @panic("OOM");
    } else {
        defs.appendSlice(&.{ "-D_DARWIN_UNLIMITED_SELECT=1", "-D_DARWIN_USE_64_BIT_INODE=1" }) catch @panic("OOM");
        for ([_][]const u8{ "proctitle", "bsd-ifaddrs", "kqueue", "random-getentropy", "darwin-proctitle", "darwin", "fsevents" }) |f|
            files.append(b.fmt("cxx_modules/libuv/src/unix/{s}.c", .{f})) catch @panic("OOM");
    }

    uv.addCSourceFiles(.{ .files = files.items, .flags = defs.items, .language = .c });
    const lib = b.addLibrary(.{ .name = "uv", .root_module = uv, .linkage = .static });
    mod.linkLibrary(lib);
}

const Wolf = struct { include_dir: []const u8, lib_dir: []const u8, step: ?*std.Build.Step };

var wolf_cache: ?std.StringHashMap(Wolf) = null;

fn wolfssl(b: *std.Build, target: std.Build.ResolvedTarget) Wolf {
    const t = target.result;
    const suffix = tripleSuffix(b, target);
    if (wolf_cache == null) wolf_cache = std.StringHashMap(Wolf).init(b.allocator);
    if (wolf_cache.?.get(suffix)) |w| return w;
    const dest = b.pathFromRoot(b.fmt("cxx_modules/wolfssl/{s}", .{suffix}));
    const lib_dir = b.fmt("{s}/lib", .{dest});
    const include_dir = b.fmt("{s}/include", .{dest});

    const asmconf: []const []const u8 = switch (t.cpu.arch) {
        .x86_64 => &.{ "--enable-intelasm", "--enable-aesni" },
        .aarch64 => if (t.os.tag == .windows) &.{"--disable-asm"} else &.{"--enable-armasm=inline"},
        else => @panic("wolfssl: unsupported arch"),
    };
    const target_flag = if (target.query.isNative()) "" else b.fmt("-target {s}", .{suffix});
    const host_triple: []const u8 = if (target.query.isNative()) "" else switch (t.os.tag) {
        .macos => if (t.cpu.arch == .x86_64) "x86_64-apple-darwin" else "aarch64-apple-darwin",
        .linux => if (t.cpu.arch == .x86_64) "x86_64-linux-musl" else "aarch64-linux-musl",
        .windows => if (t.cpu.arch == .x86_64) "x86_64-w64-mingw32" else "aarch64-w64-mingw32",
        else => @panic("wolfssl: unsupported os"),
    };

    var conf = std.array_list.Managed([]const u8).init(b.allocator);
    if (host_triple.len != 0) conf.append(b.fmt("--host={s}", .{host_triple})) catch @panic("OOM");
    conf.appendSlice(&.{
        b.fmt("--prefix={s}", .{dest}), "--enable-static",      "--disable-shared",
        "--enable-tls13",               "--enable-sni",         "--enable-alpn",
        "--enable-aesgcm",              "--enable-chacha",      "--enable-poly1305",
        "--enable-ecc",                 "--enable-curve25519",  "--enable-supportedcurves",
        "--disable-examples",           "--disable-crypttests", "--disable-sys-ca-certs",
    }) catch @panic("OOM");
    conf.appendSlice(asmconf) catch @panic("OOM");
    const stamp = b.fmt("1|{s}|{s}|{s}", .{ WOLFSSL_VER, suffix, std.mem.join(b.allocator, " ", conf.items) catch @panic("OOM") });

    if (stampOk(b, suffix, stamp)) {
        const w = Wolf{ .include_dir = include_dir, .lib_dir = lib_dir, .step = null };
        wolf_cache.?.put(suffix, w) catch @panic("OOM");
        return w;
    }

    const wolf_step = b.step(b.fmt("wolfssl-{s}", .{suffix}), "Build vendored wolfSSL");

    const srcdir = b.pathFromRoot("cxx_modules/.cache/wolfssl-src");
    const srcname = b.fmt("wolfssl-{s}-stable", .{WOLFSSL_VER});
    const srcpath = b.fmt("{s}/{s}", .{ srcdir, srcname });
    const builddir = b.fmt("{s}/build-{s}", .{ srcpath, suffix });
    const tarpath = b.fmt("{s}/wolfssl-{s}.tar.gz", .{ srcdir, WOLFSSL_VER });

    const shimdir = b.pathFromRoot(b.fmt("cxx_modules/.cache/wolfssl-shim-{s}", .{suffix}));
    const shim = b.addSystemCommand(&.{ "sh", "-c", shimScript(b, shimdir, target_flag, host_triple) });

    const mkdir_src = b.addSystemCommand(&.{ "mkdir", "-p", srcdir });
    var last: *std.Build.Step = &mkdir_src.step;

    if (!exists(b, srcpath)) {
        const url = b.fmt("https://github.com/wolfSSL/wolfssl/archive/refs/tags/v{s}-stable.tar.gz", .{WOLFSSL_VER});
        const dl = b.addSystemCommand(&.{ "curl", "-fsSL", "-o", tarpath, url });
        dl.step.dependOn(last);
        const untar = b.addSystemCommand(&.{ "tar", "xzf", tarpath, "-C", srcdir });
        untar.step.dependOn(&dl.step);
        last = &untar.step;
    }

    if (!exists(b, b.fmt("{s}/configure", .{srcpath}))) {
        const autogen = b.addSystemCommand(&.{"./autogen.sh"});
        autogen.setCwd(.{ .cwd_relative = srcpath });
        autogen.step.dependOn(last);
        last = &autogen.step;
    }

    const strip = b.addSystemCommand(&.{
        "sh",                                                                                                                                 "-c",
        b.fmt("find '{s}/wolfcrypt/src' -name '*_asm.S' -print0 | xargs -0 perl -i -ne 'print unless /^\\s*\\.(type|size)\\b/'", .{srcpath}),
    });
    strip.step.dependOn(last);

    const rmbuild = b.addSystemCommand(&.{ "rm", "-rf", builddir });
    rmbuild.step.dependOn(&strip.step);
    const mkbuild = b.addSystemCommand(&.{ "mkdir", "-p", builddir });
    mkbuild.step.dependOn(&rmbuild.step);

    var conf_argv = std.array_list.Managed([]const u8).init(b.allocator);
    conf_argv.append(b.fmt("{s}/configure", .{srcpath})) catch @panic("OOM");
    conf_argv.appendSlice(conf.items) catch @panic("OOM");
    conf_argv.appendSlice(&.{
        b.fmt("CC={s}/cc", .{shimdir}),
        b.fmt("AR={s}/ar", .{shimdir}),
        b.fmt("RANLIB={s}/ranlib", .{shimdir}),
        "C_EXTRA_FLAGS=-ffunction-sections -fdata-sections",
    }) catch @panic("OOM");
    const configure = b.addSystemCommand(conf_argv.items);
    configure.setCwd(.{ .cwd_relative = builddir });
    configure.addPathDir(shimdir);
    configure.step.dependOn(&mkbuild.step);
    configure.step.dependOn(&shim.step);

    const njobs = std.Thread.getCpuCount() catch 4;
    const make = b.addSystemCommand(&.{ "make", b.fmt("-j{d}", .{njobs}) });
    make.setCwd(.{ .cwd_relative = builddir });
    make.addPathDir(shimdir);
    make.step.dependOn(&configure.step);

    const rmdest = b.addSystemCommand(&.{ "rm", "-rf", dest });
    rmdest.step.dependOn(&make.step);
    const install_wolf = b.addSystemCommand(&.{ "make", "install" });
    install_wolf.setCwd(.{ .cwd_relative = builddir });
    install_wolf.addPathDir(shimdir);
    install_wolf.step.dependOn(&rmdest.step);

    var tail: *std.Build.Step = &install_wolf.step;
    if (t.os.tag == .windows) {
        const rename = b.addSystemCommand(&.{
            "sh",                                                                                                      "-c",
            b.fmt("[ -f '{0s}/libwolfssl.lib' ] && mv '{0s}/libwolfssl.lib' '{0s}/libwolfssl.a' || true", .{lib_dir}),
        });
        rename.step.dependOn(&install_wolf.step);
        tail = &rename.step;
    }

    const stamp_wf = b.addWriteFiles();
    const stamp_lp = stamp_wf.add(".build-flags", stamp);
    const write_stamp = b.addSystemCommand(&.{"cp"});
    write_stamp.addFileArg(stamp_lp);
    write_stamp.addArg(b.fmt("{s}/.build-flags", .{dest}));
    write_stamp.step.dependOn(tail);

    wolf_step.dependOn(&write_stamp.step);
    const w = Wolf{ .include_dir = include_dir, .lib_dir = lib_dir, .step = wolf_step };
    wolf_cache.?.put(suffix, w) catch @panic("OOM");
    return w;
}

fn shimScript(b: *std.Build, shimdir: []const u8, target_flag: []const u8, host_triple: []const u8) []const u8 {
    var s: []const u8 = b.fmt("set -e\nmkdir -p '{s}'\n", .{shimdir});
    const line = struct {
        fn f(bb: *std.Build, cur: []const u8, dir: []const u8, name: []const u8, cmd: []const u8) []const u8 {
            return bb.fmt("{s}printf '#!/bin/sh\\nexec {s} \"$@\"\\n' > '{s}/{s}'\nchmod +x '{s}/{s}'\n", .{ cur, cmd, dir, name, dir, name });
        }
    }.f;
    const cc = b.fmt("zig cc {s}", .{target_flag});
    s = line(b, s, shimdir, "cc", cc);
    s = line(b, s, shimdir, "ar", "zig ar");
    s = line(b, s, shimdir, "ranlib", "zig ranlib");
    s = line(b, s, shimdir, "lib", "zig lib");
    if (host_triple.len != 0) {
        s = line(b, s, shimdir, b.fmt("{s}-gcc", .{host_triple}), cc);
        s = line(b, s, shimdir, b.fmt("{s}-cc", .{host_triple}), cc);
        s = line(b, s, shimdir, b.fmt("{s}-ar", .{host_triple}), "zig ar");
        s = line(b, s, shimdir, b.fmt("{s}-ranlib", .{host_triple}), "zig ranlib");
    }
    return s;
}

fn exists(b: *std.Build, path: []const u8) bool {
    std.Io.Dir.accessAbsolute(b.graph.io, path, .{}) catch return false;
    return true;
}

fn installStep(b: *std.Build, exe: *std.Build.Step.Compile, target: std.Build.ResolvedTarget, out_dir: []const u8) *std.Build.Step {
    const ext: []const u8 = if (target.result.os.tag == .windows) ".exe" else "";
    const sub = if (target.query.isNative())
        b.fmt("{s}{s}", .{ exe.name, ext })
    else
        b.fmt("{s}-{s}{s}", .{ exe.name, tripleSuffix(b, target), ext });
    const ai = b.addInstallArtifact(exe, .{
        .dest_dir = .{ .override = .{ .custom = out_dir } },
        .dest_sub_path = sub,
    });
    return &ai.step;
}

var llhttp_done = false;
var llhttp_step: ?*std.Build.Step = null;
fn generateLlhttp(b: *std.Build) ?*std.Build.Step {
    if (llhttp_done) return llhttp_step;
    llhttp_done = true;

    if (exists(b, b.pathFromRoot("cxx_modules/llhttp/build/c/llhttp.c"))) return null;
    if (!exists(b, b.pathFromRoot("cxx_modules/llhttp"))) std.debug.panic(
        "llhttp is missing: clone it first with\n  git clone https://github.com/nodejs/llhttp cxx_modules/llhttp\n(build.zig then runs `npm ci && npm run build` for you)",
        .{},
    );

    const dir = b.pathFromRoot("cxx_modules/llhttp");
    const gen = b.step("llhttp", "Generate llhttp C sources (npm ci && npm run build)");
    const ci = b.addSystemCommand(&.{ "npm", "ci" });
    ci.setCwd(.{ .cwd_relative = dir });
    const bld = b.addSystemCommand(&.{ "npm", "run", "build" });
    bld.setCwd(.{ .cwd_relative = dir });
    bld.step.dependOn(&ci.step);
    gen.dependOn(&bld.step);

    llhttp_step = gen;
    return gen;
}

fn resolveTriple(b: *std.Build, triple: []const u8) std.Build.ResolvedTarget {
    const q = std.Target.Query.parse(.{ .arch_os_abi = triple }) catch @panic("bad triple");
    return b.resolveTargetQuery(q);
}

fn tripleSuffix(b: *std.Build, target: std.Build.ResolvedTarget) []const u8 {
    if (target.query.isNative()) return "host";
    return target.query.zigTriple(b.allocator) catch @panic("OOM");
}

fn glob(b: *std.Build, dir_rel: []const u8, ext: []const u8) []const []const u8 {
    const io = b.graph.io;
    var out = std.array_list.Managed([]const u8).init(b.allocator);
    var dir = b.build_root.handle.openDir(io, dir_rel, .{ .iterate = true }) catch |e| std.debug.panic("open {s}: {}", .{ dir_rel, e });
    defer dir.close(io);
    var it = dir.iterate();
    while (it.next(io) catch @panic("iterate")) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, ext)) continue;
        out.append(b.fmt("{s}/{s}", .{ dir_rel, entry.name })) catch @panic("OOM");
    }
    return out.items;
}

fn stampOk(b: *std.Build, suffix: []const u8, stamp: []const u8) bool {
    const io = b.graph.io;
    const a_rel = b.fmt("cxx_modules/wolfssl/{s}/lib/libwolfssl.a", .{suffix});
    const flags_rel = b.fmt("cxx_modules/wolfssl/{s}/.build-flags", .{suffix});
    b.build_root.handle.access(io, a_rel, .{}) catch return false;
    const got = b.build_root.handle.readFileAlloc(io, flags_rel, b.allocator, .limited(1 << 16)) catch return false;
    return std.mem.eql(u8, std.mem.trimEnd(u8, got, "\n"), stamp);
}
