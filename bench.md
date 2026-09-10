# Benchmarks

## localhost roundtrip (No network I/O)

| tool | time | throughput | notes |
|---|---:|---:|---|
| rsync -a | 8.37 s | 213 MB/s | single-threaded (macOS's ancient 2.6.9) |
| rclone copy --transfers 4 | 5.13 s | 347 MB/s | parallel, no compression |
| dicker (zstd, workers=4) | 4.59 s | 388 MB/s | parallel + compress + TCP loopback |
