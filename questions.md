# Folder Uploader — Architecture & Task Spec

A C++ tool that pushes directory trees from a **Windows client** (behind NAT, no
inbound access) to a **Linux VPS** (public IP). Derived from first principles;
it converges on a subset of what rsync/rclone already do, so **rclone is the
reference implementation** (Go, MIT). This document is the brief for
reimplementing the relevant transport in C++.

> Status of the honest question first: for a **one-off legitimate migration**,
> off-the-shelf rclone likely solves the whole task with zero custom code
> (`rclone sync <src> sftp:vps:/path --transfers=N --bwlimit=X`). Writing this in
> C++ is only justified by (a) needing a **VSS snapshot** of live data, (b) a
> licensing constraint, or (c) wanting to own the transport. Proceed assuming one
> of those holds; otherwise use rclone directly.

---

## 1. Problem statement

- **Direction:** client (Windows, sender) initiates an **outbound** connection to
  the server (Linux, receiver). The home server is behind NAT and cannot accept
  inbound connections; only egress works. The VPS has a public IP.
- **Scale:** millions of files, mixed sizes (many small + occasional multi-GB).
- **Constraints, in priority order:**
  1. **Do not disrupt the live infrastructure** on the home server. It has dense,
     latency-sensitive I/O (DBs, services). Safety of the host's own I/O is
     primary; throughput is taken from the *remainder*.
  2. **Consistency**: if live services write during the copy, a naive tree walk
     yields a time-smeared, possibly-corrupt copy. Need a point-in-time view.
  3. **Resumability**: a throttled transfer of millions of files over a loaded
     link will run long and almost certainly get interrupted. Restart must skip
     completed work, not start over.
  4. **Throughput**: maximize within the above. This is where compression and
     parallelism live.
- **Deployment:** ship the client as a self-contained `.exe`. No Cygwin/WSL
  dependency. Signed if run by the (non-technical) customer; behaviour should be
  ordinary/backup-like, not stealthy (goal is *not triggering* EDR by being
  boring, **not** evading it).

---

## 2. Key derivations (why the design is shaped this way)

These are the non-obvious conclusions we reached; they constrain the protocol.

### 2.1 Streaming compression state is inseparable from ordering
A zstd stream with a shared dictionary carries evolving state `S0→S1→S2…`. The
decoder does **not** hold an independent copy of the state — it *reconstructs* it
by consuming the compressed bytes **in order**. Compressed chunk `C` encoded
against `S2` is undecodable by a decoder sitting at `S0`. Therefore:

> **Shared dictionary ⟺ mandatory ordering.** The compression gain *is* the
> back-references; the back-references *are* the order dependency. You cannot
> reorder pieces of one dictionary stream across sockets.

Consequence: you cannot fan a single dictionary stream across N sockets and
reassemble by seqId — the decoder can't enter the stream mid-way, and buffering
to reorder before decode both re-serializes decode and defeats the parallelism.

### 2.2 `e_flush` vs `e_end` are the two levers
- `ZSTD_e_flush` at a file boundary: everything up to here is decodable, but the
  dictionary/state **lives on** → next file compresses against all prior files
  (hot dictionary) → **ordering required**.
- `ZSTD_e_end`: closes a frame, **resets** state → each frame is self-contained
  and decodable independently → **reorderable across sockets**, at the cost of a
  cold dictionary at each boundary. This is the "epoch" boundary.

### 2.3 The dictionary only pays off on small files
Small similar files (logs/JSON/HTML) benefit hugely from a shared dictionary —
each 2 KB file is mostly dictionary overhead alone. A multi-GB file is its own
dictionary (internal redundancy dominates); a "warm from neighbours" dictionary
is statistical noise on that scale. So:

```
small files  → dictionary critical → needs ORDER → cannot chunk/reorder
large files  → dictionary useless   → order irrelevant → CAN chunk/reorder
```

These point in opposite directions, which is *why the design is bimodal* rather
than a single uniform mechanism.

### 2.4 "Large files without dictionary" ≠ "without compression"
Two sources of compressibility: (1) intra-file redundancy, (2) inter-file
(dictionary) redundancy. Dropping the shared dictionary on large files loses
only source (2), which was ~nil for them anyway. Source (1) remains: each large
chunk is compressed as its **own independent zstd frame** (`e_end`). Only
already-compressed formats (jpg/mp4/zip) go raw or zstd-level-1 — because both
sources are ~0 and compressing wastes CPU we're trying to conserve.

### 2.5 Parallelism axis: pipeline, not fan-out
Async "fire many files at once, Node-style" gives nothing here: one socket (per
epoch) means nothing to overlap at the connection level, and the shared
dictionary is inherently sequential. The real win is a **3-stage pipeline** that
overlaps *heterogeneous bottlenecks*:

```
[A: disk read]  (I/O-bound) → bounded queue →
[B: compress]   (CPU-bound, SINGLE thread per dictionary stream) → bounded queue →
[C: socket send](I/O-bound, throttled)
```

Bounded queues give backpressure (the Node `pipe()` behaviour the user actually
meant): a fast producer is throttled by the slowest stage, nothing balloons in
memory, nothing spins idle. This is orthogonal to, and preferred over, multiple
sockets.

### 2.6 Multiple sockets: only for BDP, capped, and they cost the dictionary
A single TCP flow is capped at ≈ `recv_window / RTT` (bandwidth-delay product).
On a fat uplink with non-datacenter RTT, one flow underfills the pipe; N flows
each get their own window and **add up** until they hit the physical pipe. This
is the *only* honest reason for N sockets — not "N files in N pipes".

- Gain is **additive with saturation**, practical optimum **4–8**, not "more is
  better". Beyond the plateau, extra sockets only add contention against the
  live host's I/O (violates constraint #1).
- N sockets = N independent dictionary streams = **N cold dictionaries**. On
  small files this can send *more bytes* and lose net, even while filling the
  pipe better. Weigh against the dictionary loss.
- **Decision is a measurement, not a theory:** run `iperf3 -c` (1 flow) vs
  `iperf3 -P 8` on the real machine pair, plus `ping` for RTT. If `-P 8` ≫ 1
  flow → BDP-bound → sockets help; else → pipeline is enough.

### 2.7 Ownership granularity — hybrid, keyed on file size
Neither "socket owns a file" nor "socket owns a chunk" alone works:
- File-ownership dies on a multi-GB file (one socket grinds it alone while the
  rest starve — parallelism inverts exactly where it's needed most).
- Chunk-ownership of a *dictionary stream* is impossible (§2.1) and its seqId
  reordering/mid-stream-resume is the "танцы с бубном" dead end.

Resolution — a **size threshold** makes each mechanism apply only where its cost
is zero:

```
if file.size >= CHUNK_THRESHOLD:          # "large"
    split into byte-range chunks by ORIGINAL offset
    each chunk = independent zstd frame (e_end), NO shared dictionary
    chunks are self-contained → distribute across N sockets, any order
    server: pwrite(decompressed, original_offset) + interval map → commit when gapless
else:                                     # "small" (the mass)
    file stays inside a GROUP (an epoch)
    group is the unit of work-stealing; one socket owns a whole group
    inside the group: one continuous dictionary + e_flush per file (ordered)
```

The unit of distribution is therefore **"a chunk of work of bounded size"**:
a group of small files, or a chunk of a large file. Large files *produce* their
own units instead of *being* one. `offset` is the natural seqId for large chunks
(position = order, free); one-socket-per-group gives ordering for free for small
files. **No cross-socket dictionary stream ever exists**, so no seqId/reorder
buffer is needed anywhere.

### 2.8 Anti-duplication = atomic pop, not pre-partition
With work-stealing, units live in one bounded concurrent queue; senders pop
concurrently. An **atomic pop** hands each unit to exactly one sender — that is
the entire anti-duplication mechanism, for free. Prefer dynamic work-stealing
over static `hash(path) % N` sharding: static balances *count* but not *volume*
(all big files could hash to one socket), and volume is what matters.

### 2.9 Two tunables collapse the whole balance
- `bytes_per_group` (lower bound on unit size) — bigger = warmer dictionary =
  better ratio, but coarser balance / longer tail; smaller = finer balance but
  colder dictionary.
- `CHUNK_THRESHOLD` (upper bound, where large-file chunking kicks in) — kills the
  long tail (no monolith can stall at the end of the queue).

Together they clamp every work unit to a comparable size, which is what makes
work-stealing balance well across a 2 KB … 5 GB file-size spread.

### 2.10 Pretrained dictionary — the escape hatch (optional)
A `zstd --train`ed dictionary is a **static** blob loaded identically on both
ends *before* the stream. It gives dictionary-quality compression to
*independently* compressed files — no ordering dependency. This is the only way
to have **both** a warm dictionary **and** cross-socket parallelism on small
files: every socket loads the same pretrained blob, files stay independent.
- Cost: needs a representative training sample (an extra pass over data — which
  we're trying to spare the disk), works best when small files are **homogeneous**
  (mostly one or two types). On a heterogeneous mix the gain flattens.
- Both ends must hold the *identical* blob (hash/version it; server verifies).
  This is the one place "different tables on the ends" is genuinely fatal —
  unlike streaming state, where it isn't even a coherent notion.

---

## 3. Consistency & host-safety (the parts no off-the-shelf tool does)

These are the reasons to own the client at all.

### 3.1 VSS snapshot (Windows) — point-in-time consistency
If live services write to the target folders during the copy, read from a
**Volume Shadow Copy** instead of the live volume:
- Create a shadow copy (VSS COM API `IVssBackupComponents`, or pragmatically
  `wmic shadowcopy call create` / `vssadmin`), obtain the
  `\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN` device path, read the tree
  from there, delete the snapshot when done.
- Copy-on-write: live services keep writing to the real volume; the snapshot is
  frozen at time `T`. This is the FS-level analogue of an MVCC/transaction
  snapshot — the user's own "don't lock the prod DB for 10 min" analogy.
- Caveats: needs shadow-storage space that grows with source churn over a long
  throttled transfer; the snapshot must outlive the whole transfer.
- (Linux equivalent, if the source were ever Linux: LVM/ZFS/btrfs snapshot.)

### 3.2 Not disrupting live I/O
- `SetPriorityClass(PROCESS_MODE_BACKGROUND_BEGIN)` — lowers CPU **and I/O**
  priority so the OS serves the live services first. Critical when reading
  millions of files (metadata storms).
- Cap the number of **in-flight I/O ops** to 1–2. On dense-I/O hosts the killer
  is queue depth: your requests jumping ahead of the DB's. Deliberately
  under-drive the disk, leaving gaps for others.
- Bandwidth cap the uplink (e.g. 20–30% of it), not "as fast as possible" — the
  link is already loaded. This is a *don't-disturb* measure, framed honestly to
  the customer as "won't hog your connection", not as hiding.

---

## 4. Wire protocol (the reimplemented part)

Custom, dependency-free framing over a plain TCP connection (TLS on top). No
`curl`/HTTP. Two unit types on the wire, tagged in a header.

### 4.1 Session
```
[magic/version : 4B]
[job_id : 16B]        # persistent, client-generated, SAME across reconnects
                      # server directory == job_id (NOT per-TCP-session)
[optional: pretrained-dict hash/version : 32B]  # 0 if none; server verifies
```
`job_id` gives uniqueness across *runs* (different id) and resume across
*reconnects* (same id). A reconnect is by definition a new TCP session, so
ownership must key on `job_id`, not the socket.

### 4.2 Unit types
**GROUP (small files):** one dictionary stream, `e_flush` per file.
```
[unit_type = GROUP : 1B][group_id : 8B]
--- repeated inside one continuous ZSTD_CStream, e_flush after each file ---
  [path_len : 2B][relative_path : utf-8]   # normalized, see §4.4
  [orig_size : 8B]
  [xxhash of original : 8B]
  [file bytes]
  <ZSTD_e_flush>                            # commit point for this file
--- end group ---
```
Server: decompress continuously; at each flush boundary a whole file is in hand
→ write into `job_id/…` → that is the resume unit.

**FILE_CHUNK (large files):** independent frames, order-free.
```
[unit_type = FILE_CHUNK : 1B]
[file_id : 8B][path_len:2B][relative_path]
[orig_offset : 8B]    # where in the reassembled file this chunk goes (pwrite)
[orig_len : 8B]       # decompressed length of this chunk
[comp_flag : 1B]      # 0=raw (already-compressed formats), 1=zstd-frame
[comp_len : 8B]       # bytes to read from socket for this chunk's payload
[payload...]          # a self-contained zstd frame (e_end) or raw bytes
```
Server: read `comp_len`, decompress to `orig_len`, `pwrite(fd, buf, orig_offset)`;
maintain a received-interval map per file; commit (fsync + rename) when gapless.

### 4.3 Resume — server-state on reconnect (preferred over ack-stream)
On (re)connect with `job_id`, the server reports what it already has:
- completed `group_id`s, and per-incomplete-file the received interval set.
Client diffs against its own tree/plan and sends only what's missing.
- Small files: a group either completed or not → resend the whole (small) group;
  never "enter a dictionary stream mid-way" (impossible) — just start a fresh
  epoch/frame.
- Large files: resend only the missing byte ranges (chunks are self-contained).
Rationale: server is the source of truth; client keeps minimal state. Alternative
(ack-stream + client append-only manifest) is faster on reconnect but needs a
bidirectional protocol and a millions-line manifest.

### 4.4 Server-side safety
- **Path traversal:** this writes paths supplied by the client to someone else's
  VPS. Strip drive letters, reject `..` and absolute paths, confine everything
  under `job_id/`. Non-negotiable.
- **Durability vs throughput:** do **not** fsync per file (kills throughput and
  the disk on millions of files). For *network-interruption* resume, fsync isn't
  needed at all — data in page cache survives as long as the server process
  lives. fsync only guards *crash* consistency. Compromise: fsync periodically
  (every N files / T seconds) and on clean session close. A file left partial by
  a crash is detected by `size on disk != declared size` and re-sent whole.

---

## 5. Client architecture (Windows)

```
SetPriorityClass(BACKGROUND_BEGIN)                 # constraint #1
  → create VSS snapshot (if source is live)        # constraint #2
    → walk the snapshot tree
    → SORT by type/directory (dictionary locality: all .log together, etc.)
    → classify by size → GROUP units (small) / FILE_CHUNK units (large)
    → producer thread: push units into a bounded concurrent queue  (§2.5 stage A)
  → N sender workers (IOCP on Windows; 1 socket each; N from §2.6 or hardcoded):
      unit = queue.pop()                            # atomic → anti-dup (§2.8)
      GROUP:      own ZSTD_CStream, e_flush per file, send framed  (stage B+C)
      FILE_CHUNK: independent frame(s), send with orig_offset
      MAX_SEND_SPEED cap (throttle)                 # constraint #1
      on success: mark unit done (manifest / rely on server-state)
      on error:   do NOT mark; unit re-sent whole on resume
  → delete VSS snapshot
SetPriorityClass(BACKGROUND_END)
```
`maxSockets`: accept a hardcoded value **or** compute aria2-style (start at 1,
add a socket while aggregate throughput rises, stop when the marginal socket adds
< ~10% → you've found the saturation plateau).

## 6. Server architecture (Linux VPS)
```
plain TCP accept (one listener, N connections for one job_id)
  read session header → resolve/create job_id dir → verify dict version
  per connection, dispatch by unit_type:
    GROUP:      continuous decompress, commit-on-flush into job_id/
    FILE_CHUNK: pwrite by orig_offset, interval map, commit-on-complete
  path-traversal guard on every path
  on (re)connect: reply with server-state (done groups + partial intervals)
  periodic fsync; clean-close fsync
```

---

## 7. rclone as reference — what to study in their repo
- **`fs/sync`** — the sync engine: how transfers are scheduled and parallelized.
- **`--transfers` / `--multi-thread-streams`** — their answer to our N-sockets and
  large-file chunking; read how they pick stream counts and split large files.
- **`--bwlimit`** — token-bucket throttling (our uplink cap).
- **SFTP backend (`backend/sftp`)** — how they talk to a Linux box over SSH; a
  sane baseline transport if we decide not to hand-roll TCP framing after all.
- **Multi-thread download/upload chunking** — offset-based reassembly, directly
  analogous to our FILE_CHUNK/`pwrite` path.
- Note the divergence: rclone sends **whole files, no delta, no cross-file
  dictionary** — it optimizes pure parallel throughput. Our small-file
  dictionary stream is the main thing rclone does *not* do; everything else we
  want, it already has. Decide per-§2 whether the dictionary is worth owning the
  transport for, or whether rclone's model is simply good enough.

---

## 8. Open decisions to resolve before/with Claude Code
1. **Is the source live during copy?** → VSS needed, or read directly.
2. **iperf3 1-flow vs -P 8, and RTT?** → whether N sockets are justified at all,
   and how many. Pipeline first regardless.
3. **Small-file profile:** homogeneous (→ streaming dict, or pretrained dict +
   parallelism) or heterogeneous grab-bag (→ streaming dict with real neighbours,
   or accept rclone's no-dict model)?
4. **Size profile:** is there real multi-GB *uncompressible* data (→ FILE_CHUNK
   with mid-level zstd), or are large files mostly already-compressed (→ raw
   chunks, dictionary question moot)?
5. **`bytes_per_group`, `CHUNK_THRESHOLD`, `maxSockets`** starting values — fall
   out of (3) and (4).
6. **Build/dist:** static-linked single `.exe` (zstd + platform libs), signed?
   Or thin launcher around rclone after all?