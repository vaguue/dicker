# Dicker

A lightweight tool and SDK to transfer your files as fast as possible. This repo was designed for a case when you need to go faster than rclone / rsync - dicker does it by:
- Cross-file compression streams
- Lock-free async channels for I/O
- Supporting windows VSS

## Usage 
The repo consists of server and client, running a server is as simple as
```bash
DICKER_KEY=dicker-secret ./dicker-server --root /tmp
```
following the client:
```bash
# use dicker for plaintext, dickers for dicker over TLS
DICKER_KEY=dicker-secret ./dicker dickers://dicker-secret@1.2.3.4:5959 --root file-or-directory-1 --root file-or-directory-2
```

The server listens on one port for everything: dicker uploads, dicker over TLS, HTTP and HTTPS.

## Build

```bash
zig build            # client, server and both libraries for this machine
zig build release    # the full cross-compiled matrix
zig build -Ddebug    # debug build
```

Everything lands in `client/out/` and `server/out/`: the binaries, `libdicker.a` / `libdicker-server.a` and an `include/` folder. `client/out/example` and `server/out/example` are built from [client/example.cxx](client/example.cxx) and [server/example.cxx](server/example.cxx), which use the libraries the same way an SDK user would. Dependencies (wolfSSL, zstd, lz4, libuv, [muypoco](https://github.com/vaguue/muypoco)) are pinned in `build.zig.zon` and compiled from source with `zig c++`. Needs Zig 0.16.0 or newer.

## C++ SDK

```bash
zig fetch --save git+https://github.com/vaguue/dicker
```

```zig
const dicker = b.dependency("dicker", .{ .target = target, .optimize = optimize });
exe.root_module.linkLibrary(dicker.artifact("dicker"));
```

```cpp
#include "dicker/dicker.h"

int main() {
  dicker::Conn conn{"dickers://dicker-secret@1.2.3.4:5959"};

  dicker::Config cfg{conn};
  dicker::Scheduler dicker{cfg};

  dicker.addRoot("file-or-directory-1");
  dicker.addRoot("file-or-directory-2");

  dicker.start();

  return 0;
}
``` 

### Upload events

```cpp
dicker::events.on<"upload:started">([](const dicker::UploadStarted& u) noexcept {
  // u.path, u.type (file or chunk of a big file), u.offset, u.length, u.fileSize
});

dicker::events.on<"upload:finished">([](const dicker::UploadFinished& u) noexcept {
  // same as above, plus u.compressedBytes and u.ok
});
```

One pair per file, or per chunk for big files. Handlers run on the upload worker threads, several at once, so keep them thread-safe and quick. "finished" means the unit was read, compressed and queued on the connection. Events are [muypoco](https://github.com/vaguue/muypoco) events, so `dicker::events::on` (by presence of code) works too.

## Server library

```zig
const dicker = b.dependency("dicker", .{ .target = target, .optimize = optimize, .server = true });
exe.root_module.linkLibrary(dicker.artifact("dicker-server"));
```

```cpp
#include "dicker/http_context.h"
#include "dicker/server.h"
#include "dicker/server_events.h"

int main() {
  dicker::ServerConfig config;
  config.key = "dicker-secret";
  config.root = "/backups";
  config.channel_high_water = 64 * 1024 * 1024;
  config.channel_low_water = 16 * 1024 * 1024;

  dicker::Server server(config);

  server.route("GET", "/health", [](dicker::HttpContext& ctx) {
    ctx.body = "ok\n";
  });

  dicker::server::events.on<"upload:finished">([](const dicker::server::UploadFinished& u) noexcept {
    // u.session, u.peer, u.path, u.bytes, u.compressedBytes, u.ok, u.reason
  });

  if (!server.listen("0.0.0.0", 5959)) {
    return 1;
  }
  server.run();
}
```

- Routes: `server.route(method, path, handler)` for exact paths, `server.route(method, dicker::Match::Prefix, "/files/", handler)` for prefixes (the tail is `ctx.param`). An optional last argument is a middleware chain, e.g. `{dicker::mw::require_auth}`. Your routes are checked before the built-in `/`, `/uploads` and `/uploads/...`. To turn the built-ins off, set `config.default_routes = false`.
- `server.run()` serves until `server.stop()`, which works from any thread, a route handler included.
- Server events, on `dicker::server::events`: `"client:connected"`, `"upload:started"`, `"upload:finished"` and `"client:disconnected"`. They run on the server's threads.

## Notes

- The server is built using [libuv](https://github.com/libuv/libuv), supported compression algorithms include zstd and lz4.
- No documentation yet, but the code is pretty simple 
- The server is mostly vibecoded, the client (the hard part) and the protocol are manual 
- If you want to know how it works check out [arch.md](https://github.com/vaguue/dicker/blob/main/arch.md)
- Outperforms rclone on raw disk: [bench.md](https://github.com/vaguue/dicker/blob/main/bench.md)
