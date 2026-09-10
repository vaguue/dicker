# Task

And .exe program exports (uploads) a bunch of directories to a specified server, each directory can contain millions of files, or can contain a multiple-gigabytes files. Should be as quick as possible but doesn't hang the whole system CPU and IO-wise, we should be able to cap how intensive the process is.

# Definitions

*Worker* - a runtime entity that owns a TCP socket and keeps a global compression state, so the compression surives between the files, queues a storage-reader for contents. Each worker accepts an array of worker-task entities
*Worker-task* - a task to send on wire a file or a chunk of file (for big files)
*Scheduler* - Owns a bunch of workers, gives the tasks to each worker, but must be really tight, do the most of the work while all of the workers are busy, keep a live state for each worker. Scheduller must keep in mind the backpressure problem as well
*Storage-reader* - A single entity the can be queued for reading something from fs (a whole file or a chunk of file), must be extremely efficient and support VSS. A question is if the storage should actually be a single thing. I think the best approach is to give a shared ptr, decide on concurrency later.
*Server* - accepts TCP connections, for each connection keeps a compression state to decompress the files or file chunks it receives. The writing process doesn't have to be super quick but receiving must be. My library of choice here is I think libuv. An important thing is when we receive a chunk-type payload (there should be a difference in the frame header) we should pwrite it into a huge file once decompressed

# Protocol 

- First of all we need a framed stream with per-file flush boundaries protocol that wraps the files in a stream, the protocol must support a compression and the compression state is kept between the files, we don't start from nothing for each file, that's the point
- The protocol should support optional integrity checks (append a file checksum)
- The protocol supports authentication, so different sessions create different subdirectories, but resuming the old session should keep writing to the old directory. Also important for the case of multiple workers, they must have a single destination on the server, so each worker must be authenticated
- During the handshake we check the session ID and it's signature (HMAC I guess) and also we get some info like the compression algorithm (streamed version); for now the only supported compression is zstd
- If we support integrity checks, we can reject the files we already have, but that's for the future, but the authentication is a foundation for that

# Code style 

- "  " offset, not tabs no "    "
- always {}, e.g.
```
if (...) {

}
else {

}
```
- No glueing multiple line sinto a single one, makes it harder to read for me
- No comments, self documenting code only, no hacky variable names
- When relevant, C-style function and constructions can be wrapped in RAII patterns (for libuv especially). Zero runtime overhead though.
- Struct only, no class, everything's public

# dicker wire protocol implementation (v1)

Plain TCP. All integers little-endian. One TCP connection = one worker = one
continuous compression stream. Multiple connections may share a `session_id`
(they write into the same server directory); large-file chunks may be split
across connections and arrive out of order (reassembled by `pwrite` at offset).

## Handshake (client -> server, once per connection, 56 bytes)

| field       | size | notes                                             |
|-------------|------|---------------------------------------------------|
| magic       | 4    | "DKR1"                                             |
| version     | 2    | u16 LE, currently 1                               |
| flags       | 1    | bit0 = per-unit integrity checksum present         |
| algo        | 1    | 1 = lz4 (frame, linked blocks), 2 = zstd          |
| session_id  | 16   | client-generated, stable across reconnects        |
| hmac        | 32   | HMAC-SHA256(key, first 24 bytes) — magic..session |

## Handshake reply (server -> client, 5 bytes)

| field  | size | notes                                                    |
|--------|------|----------------------------------------------------------|
| magic  | 4    | "DKR1"                                                    |
| status | 1    | 0 ok, 1 bad magic, 2 bad version, 3 auth, 4 algo, 5 error |

On non-zero status the server closes the connection.

## Units (client -> server, repeated after an OK handshake)

A unit is a small header, then a **block stream**, then an optional checksum
trailer. There is no total length anywhere: the client reads -> compresses ->
emits a block, repeats, then flushes and sends an end-of-unit marker. The
compression state (dictionary/window) carries across units on the connection;
the client flushes the codec at each end-of-unit so the unit is fully decodable.

Unit header:

| field     | size     | notes                                    |
|-----------|----------|------------------------------------------|
| unit_type | 1        | 0 FILE, 1 FILE_CHUNK, 0xFF END-OF-SESSION |
| path_len  | 2        | u16 LE, 1..4096                           |
| path      | path_len | UTF-8 relative path (no `..`, no drive)   |
| offset    | 8        | u64 LE — FILE_CHUNK only, pwrite target   |

Block stream (repeats until an end-of-unit block):

| field      | size     | notes                                        |
|------------|----------|----------------------------------------------|
| block_type | 1        | 1 = DATA, 0 = END-OF-UNIT                     |
| block_len  | 4        | u32 LE — DATA only, compressed bytes to read  |
| payload    | block_len| DATA only, compressed bytes (fed to decoder)  |

Checksum trailer (only if handshake flags bit0 set), immediately after the
END-OF-UNIT block:

| field    | size | notes                                            |
|----------|------|--------------------------------------------------|
| checksum | 8    | u64 LE, XXH64 of the whole decompressed unit     |

The checksum covers the entire file (FILE) or the entire chunk (FILE_CHUNK),
computed incrementally on both ends as blocks flow. END-OF-SESSION
(`unit_type == 0xFF`) carries no other fields; the stream also ends on TCP FIN.

## Sizing

- A file smaller than the big-file threshold (256 MB, like rclone's
  `--multi-thread-cutoff`) is ONE FILE unit, streamed in ~256 KB blocks —
  bounded memory, no size cap on the file.
- A file at/above the threshold is split into FILE_CHUNK units by byte range,
  distributed across connections, each streamed the same way.
- Server bounds a single block to `kMaxBlockSize` (64 MB).

## Server behavior

- FILE: decode blocks, write sequentially to `<root>/<session_hex>/<path>`.
- FILE_CHUNK: decode blocks, `pwrite` at `offset` (shared refcounted fd;
  out-of-order chunks across connections are fine).
- Paths confined under the session dir; `..`/absolute/drive rejected.
- v1: handshake-ack-only (no resume/dedup); fsync on close.

## Compression

- **lz4**: LZ4 Frame, `blockMode = LZ4F_blockLinked`, one frame per connection,
  `LZ4F_flush` per unit. Cross-file window 64 KB (format cap).
- **zstd**: one `ZSTD_CStream` per connection, `ZSTD_e_flush` per unit, never
  `e_end` mid-connection. Server allows `windowLogMax = 31`.
