'use strict';

const net = require('net');
const zlib = require('zlib');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const { once } = require('events');

const MAGIC = Buffer.from('DKR1', 'ascii');
const PROTOCOL_VERSION = 1;

const Cmd = { Upload: 0x33 };
const Algo = { None: 0, Lz4: 1, Zstd: 2 };
const UnitType = { File: 0, Chunk: 1, End: 0xff };
const BlockType = { EndOfUnit: 0, Data: 1 };
const Flag = { Integrity: 1 << 0 };

const SESSION_ID_SIZE = 16;
const HMAC_SIZE = 32;
const SIGNED_PREFIX_SIZE = MAGIC.length + 2 + 1 + 1 + SESSION_ID_SIZE;
const HANDSHAKE_SIZE = SIGNED_PREFIX_SIZE + HMAC_SIZE;
const HANDSHAKE_REPLY_SIZE = MAGIC.length + 1;
const MAX_PATH_LENGTH = 4096;
const MAX_BLOCK_SIZE = 64 * 1024 * 1024;

const STATUS_NAME = {
  0: 'ok',
  1: 'bad-magic',
  2: 'bad-version',
  3: 'auth-failed',
  4: 'bad-algo',
  5: 'server-error',
};

const READ_CHUNK = 256 * 1024;
const WIRE_BLOCK = 4 * 1024 * 1024;

class Conn {
  constructor(url) {
    const match = String(url).match(
      /^(dickers?):\/\/(?:([^@/]*)@)?([^:/?#]+)(?::(\d+))?/
    );
    if (!match) {
      throw new Error('invalid dicker url: ' + url);
    }
    this.scheme = match[1];
    this.tls = this.scheme === 'dickers';
    this.key = match[2] ? decodeURIComponent(match[2]) : '';
    this.host = match[3];
    this.port = match[4] ? Number(match[4]) : 0;
    if (!this.port) {
      throw new Error('dicker url is missing a port: ' + url);
    }
  }
}

class Config {
  constructor(conn) {
    this.conn = conn;
    this.verbose = false;
    this.level = 3;
    this.integrity = false;
    this.sessionId = crypto.randomBytes(SESSION_ID_SIZE);
  }
}

function normalizeSessionId(value) {
  if (Buffer.isBuffer(value)) {
    if (value.length !== SESSION_ID_SIZE) {
      throw new Error('session id must be ' + SESSION_ID_SIZE + ' bytes');
    }
    return value;
  }
  const buffer = Buffer.from(String(value), 'hex');
  if (buffer.length !== SESSION_ID_SIZE) {
    throw new Error('session id hex must decode to ' + SESSION_ID_SIZE + ' bytes');
  }
  return buffer;
}

function buildHandshake(key, sessionId, flags) {
  const buffer = Buffer.alloc(HANDSHAKE_SIZE);
  MAGIC.copy(buffer, 0);
  buffer.writeUInt16LE(PROTOCOL_VERSION, 4);
  buffer[6] = flags;
  buffer[7] = Algo.Zstd;
  sessionId.copy(buffer, 8);
  const hmac = crypto
    .createHmac('sha256', Buffer.from(key, 'utf8'))
    .update(buffer.subarray(0, SIGNED_PREFIX_SIZE))
    .digest();
  hmac.copy(buffer, SIGNED_PREFIX_SIZE);
  return buffer;
}

function writeAll(socket, buffer) {
  return new Promise((resolve, reject) => {
    socket.write(buffer, (err) => (err ? reject(err) : resolve()));
  });
}

function readExactly(socket, count) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let have = 0;
    const cleanup = () => {
      socket.removeListener('data', onData);
      socket.removeListener('error', onError);
      socket.removeListener('close', onClose);
    };
    const onData = (data) => {
      chunks.push(data);
      have += data.length;
      if (have >= count) {
        cleanup();
        resolve(Buffer.concat(chunks).subarray(0, count));
      }
    };
    const onError = (err) => {
      cleanup();
      reject(err);
    };
    const onClose = () => {
      cleanup();
      reject(new Error('connection closed before handshake reply'));
    };
    socket.on('data', onData);
    socket.on('error', onError);
    socket.on('close', onClose);
  });
}

async function* walkFiles(root) {
  const resolved = path.resolve(root);
  const info = await fs.promises.lstat(resolved);
  if (info.isFile()) {
    yield { fullPath: resolved, base: path.dirname(resolved) };
    return;
  }
  if (!info.isDirectory()) {
    return;
  }
  yield* walkDirectory(resolved, path.dirname(resolved));
}

async function* walkDirectory(dir, base) {
  const entries = await fs.promises.readdir(dir, { withFileTypes: true });
  entries.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  for (const entry of entries) {
    const fullPath = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      yield* walkDirectory(fullPath, base);
    } else if (entry.isFile()) {
      yield { fullPath, base };
    }
  }
}

function toRelativePath(fullPath, base) {
  return path.relative(base, fullPath).split(path.sep).join('/');
}

class Scheduler {
  constructor(config) {
    this.config = config;
    this.roots = [];
    this.files = 0;
    this.rawBytes = 0;
    this.compressedBytes = 0;
  }

  addRoot(root) {
    this.roots.push(root);
  }

  log(message) {
    if (this.config.verbose) {
      process.stderr.write('[dicker] ' + message + '\n');
    }
  }

  async start() {
    const conn = this.config.conn;
    if (conn.tls) {
      throw new Error('dickers:// (TLS) is not supported by this client');
    }
    if (typeof zlib.createZstdCompress !== 'function') {
      throw new Error(
        'this Node build has no built-in zstd (need Node >= 22.15); running ' + process.version
      );
    }
    if (this.config.integrity) {
      throw new Error('integrity checks require XXH64, not implemented in this client');
    }

    const sessionId = normalizeSessionId(this.config.sessionId);
    const flags = this.config.integrity ? Flag.Integrity : 0;

    const socket = net.connect({ host: conn.host, port: conn.port });
    socket.on('error', () => {});

    let zstd = null;
    try {
      await once(socket, 'connect');
      this.log('connected to ' + conn.host + ':' + conn.port);

      await writeAll(socket, buildHandshake(conn.key, sessionId, flags));
      const reply = await readExactly(socket, HANDSHAKE_REPLY_SIZE);
      if (reply.subarray(0, MAGIC.length).compare(MAGIC) !== 0) {
        throw new Error('handshake reply had bad magic');
      }
      const status = reply[MAGIC.length];
      if (status !== 0) {
        throw new Error('handshake rejected: ' + (STATUS_NAME[status] || status));
      }
      this.log('handshake ok session=' + sessionId.toString('hex') + ' algo=zstd');

      await writeAll(socket, Buffer.from([Cmd.Upload]));

      zstd = zlib.createZstdCompress({
        params: { [zlib.constants.ZSTD_c_compressionLevel]: this.config.level },
      });
      const outQueue = [];
      zstd.on('data', (chunk) => outQueue.push(chunk));

      const drainBlocks = async () => {
        while (outQueue.length > 0) {
          const chunk = outQueue.shift();
          let offset = 0;
          while (offset < chunk.length) {
            const size = Math.min(chunk.length - offset, WIRE_BLOCK, MAX_BLOCK_SIZE);
            const header = Buffer.allocUnsafe(5);
            header[0] = BlockType.Data;
            header.writeUInt32LE(size, 1);
            await writeAll(socket, header);
            await writeAll(socket, chunk.subarray(offset, offset + size));
            offset += size;
            this.compressedBytes += size;
          }
        }
      };

      const feed = (chunk) =>
        new Promise((resolve, reject) => {
          zstd.write(chunk, (err) => (err ? reject(err) : resolve()));
        });

      const flushUnit = () =>
        new Promise((resolve) => zstd.flush(zlib.constants.ZSTD_e_flush, resolve));

      for (const root of this.roots) {
        for await (const entry of walkFiles(root)) {
          const relativePath = toRelativePath(entry.fullPath, entry.base);
          const pathBytes = Buffer.from(relativePath, 'utf8');
          if (pathBytes.length === 0 || pathBytes.length > MAX_PATH_LENGTH) {
            this.log('skip ' + relativePath + ' (path length ' + pathBytes.length + ')');
            continue;
          }

          const header = Buffer.allocUnsafe(3 + pathBytes.length);
          header[0] = UnitType.File;
          header.writeUInt16LE(pathBytes.length, 1);
          pathBytes.copy(header, 3);
          await writeAll(socket, header);

          let fileBytes = 0;
          const readStream = fs.createReadStream(entry.fullPath, { highWaterMark: READ_CHUNK });
          for await (const chunk of readStream) {
            fileBytes += chunk.length;
            await feed(chunk);
            await drainBlocks();
          }
          await flushUnit();
          await drainBlocks();
          await writeAll(socket, Buffer.from([BlockType.EndOfUnit]));

          this.files += 1;
          this.rawBytes += fileBytes;
          this.log('sent ' + relativePath + ' bytes=' + fileBytes);
        }
      }

      await writeAll(socket, Buffer.from([UnitType.End]));
      socket.end();
      await once(socket, 'close');

      const ratio = this.compressedBytes > 0 ? this.rawBytes / this.compressedBytes : 0;
      this.log(
        'done files=' + this.files + ' bytes=' + this.rawBytes +
        ' wire=' + this.compressedBytes + ' ratio=' + ratio.toFixed(2) + 'x'
      );
      return { files: this.files, bytes: this.rawBytes, wire: this.compressedBytes };
    } finally {
      if (zstd) {
        zstd.destroy();
      }
      socket.destroy();
    }
  }
}

async function main(argv) {
  const conn = new Conn('dicker://kek@161.104.54.88:443');
  const config = new Config(conn);
  config.verbose = true;

  const scheduler = new Scheduler(config);

  scheduler.addRoot('/tmp');

  const result = await scheduler.start();
  process.stdout.write(
    '[*] Finish files=' + result.files + ' bytes=' + result.bytes + ' wire=' + result.wire + '\n'
  );
}

if (require.main === module) {
  main(process.argv).catch((err) => {
    process.stderr.write('[dicker] error: ' + err.message + '\n');
    process.exit(1);
  });
}

module.exports = { Conn, Config, Scheduler };
