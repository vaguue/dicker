# Benchmarks

## localhost roundtrip (No network I/O)

| tool | time | throughput | notes |
|---|---:|---:|---|
| rsync -a | 8.37 s | 213 MB/s | single-threaded (macOS's ancient 2.6.9) |
| rclone copy --transfers 4 | 5.13 s | 347 MB/s | parallel, no compression |
| dicker (zstd, workers=4) | 4.59 s | 388 MB/s | parallel + compress + TCP loopback |

Dataset: real Telegram `tdata` (1.7 GB, 10,351 files, mostly already-compressed
media). Warm cache, replicated to a fresh dst, median of 5. Even where compression
can't help (loopback) and dicker also `fsync`s every file, the read-ahead + 4-way
pipeline make it fastest — so the disk-IO + compression layers aren't a bottleneck.

## over the network (VPS, Amsterdam, ~122 Mbit uplink, high latency + bufferbloat)

Interleaved bursts: fixed ~103 MB / 1,256-file slice of tdata, 4 rounds of
dicker→rsync→rclone back-to-back (so every tool sees the same network minute),
medians. Uplink measured at 121.9 Mbit (`networkQuality`), "Responsiveness: Low".

| tool | time | throughput | notes |
|---|---:|---:|---|
| dicker (zstd, 4 conns) | 5.90 s | 17.5 MB/s | won all 4 rounds |
| rsync -a (1 conn, SSH) | 7.18 s | 14.4 MB/s | line-rate (raw bytes) |
| rclone copy (SFTP, --transfers 4) | 179.8 s | 0.6 MB/s | ~180 s every round |
