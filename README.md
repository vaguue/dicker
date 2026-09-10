# Dicker

A lightweight tool and SDK to transfer your files as fast as possible. This repo was designed for a case when you need to go faster than rclone / rsync - dicker does it by:
- Cross-file compression streams
- Lock-free async channels for I/O
- Supporting windows VSS

## Usage 
The repo consists of server and client, running a server is as simple as
```bash
DICKER_SECRET=dicker-secret ./dicker-server --root /tmp
```
following the client:
```bash
./dicker dicker://dicker-secret@1.2.3.4:5959 file-or-directory-1 file-or-directory-2
```

C++ SDK usage:
```cpp
#include "dicker.h"

int main() {
  dicker::Conn conn{"dicker://dicker-secret@1.2.3.4:5959"};

  dicker::Config cfg{conn};
  dicker::Scheduler dicker{cfg};

  dicker.addRoot("file-or-directory-1");
  dicker.addRoot("file-or-directory-2");

  dicker.start();

  return 0;
}
``` 

## Notes

- The server is built using [libuv](https://github.com/libuv/libuv), supported compression algorithms include zstd and lz4.
- No documentation yet, but the code is pretty simple 
- The server is mostly vibecoded, the client (the hard part) and the protocol are manual 
- Everything is built from source using `zig c++`, no 3rd party deps
- If you want to know how it works check out [arch.md](https://github.com/vaguue/dicker/blob/main/arch.md)
- Outperforms rclone on raw disk: [bench.md](https://github.com/vaguue/dicker/blob/main/bench.md)
