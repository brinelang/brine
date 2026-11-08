// Runs a WebAssembly build of a brine program under Node.js:
//
//     node brine_wasi.mjs program.wasm [args...]
//
// `brine run --target wasm` and `brine test --target wasm` use it.
import { readFile } from "node:fs/promises";
import { readdirSync, writeSync } from "node:fs";
import { join } from "node:path";
import { WASI } from "node:wasi";
import { Worker, isMainThread, workerData } from "node:worker_threads";
import process from "node:process";

// WebAssembly frames live on the thread's native stack, about 1 MB on the
// main thread: recursion that runs natively would trap after ~10k calls. A
// worker thread can have a big stack.
const STACK_MB = 256;

if (isMainThread) {
  const worker = new Worker(new URL(import.meta.url), {
    workerData: process.argv.slice(2),
    resourceLimits: { stackSizeMb: STACK_MB },
  });
  worker.on("exit", (code) => {
    process.exitCode = code;
  });
  worker.on("error", (e) => {
    writeSync(2, `\nbrine: the WebAssembly runner failed: ${e && e.stack ? e.stack : e}\n`);
    process.exitCode = 134;
  });
} else {
  const [file, ...args] = workerData;
  // paths relative to the current folder work as they do natively. Only that
  // folder: with "/" also open, wasi-libc resolves relative paths under "/".
  const preopens = { ".": "." };
  const wasi = new WASI({
    version: "preview1",
    args: [file, ...args],
    env: process.env,
    preopens,
    returnOnExit: true,
  });
  const imports = wasi.getImportObject();
  let instance = null;
  readdirFallback(imports.wasi_snapshot_preview1, preopens, () => instance.exports.memory);
  const module = await WebAssembly.compile(await readFile(file));
  instance = await WebAssembly.instantiate(module, imports);
  let code = 0;
  try {
    code = wasi.start(instance);
  } catch (e) {
    // a trap: what the program printed before it is still in its buffer
    try {
      instance.exports.brine_flush?.();
    } catch {}
    if (e instanceof RangeError) {
      // the engine's call stack ran out: report it as native code does
      writeSync(2, "\npanic: stack overflow (recursion too deep)\n");
      code = 101;
    } else {
      // brine panics exit with 101, so this is a crash
      writeSync(2, `\nWebAssembly trap: ${e && e.message ? e.message : e}\n`);
      code = 134;
    }
  }
  process.exit(code);
}

// Node's WASI has no fd_readdir on Windows (ENOSYS), so `os.list_dir` would
// fail there. Where it says so, list the folder with node:fs instead: the
// folder each fd names is followed through path_open, from the preopens on.
function readdirFallback(w, preopens, memory) {
  const ENOSYS = 52;
  const dirs = new Map();
  let fd = 3;
  for (const host of Object.values(preopens)) dirs.set(fd++, host);
  const pathOpen = w.path_open;
  w.path_open = (dirfd, dirflags, ptr, len, oflags, base, inheriting, fdflags, out) => {
    const r = pathOpen(dirfd, dirflags, ptr, len, oflags, base, inheriting, fdflags, out);
    if (r === 0 && dirs.has(dirfd)) {
      const m = memory();
      const name = Buffer.from(m.buffer, ptr, len).toString();
      dirs.set(new DataView(m.buffer).getUint32(out, true), join(dirs.get(dirfd), name));
    }
    return r;
  };
  const fdClose = w.fd_close;
  w.fd_close = (f) => {
    const r = fdClose(f);
    if (r === 0 && f > 2 + Object.keys(preopens).length) dirs.delete(f);
    return r;
  };
  const fdRenumber = w.fd_renumber;
  w.fd_renumber = (from, to) => {
    const r = fdRenumber(from, to);
    if (r === 0) {
      if (dirs.has(from)) dirs.set(to, dirs.get(from));
      else dirs.delete(to);
      dirs.delete(from);
    }
    return r;
  };
  const fdReaddir = w.fd_readdir;
  w.fd_readdir = (f, buf, bufLen, cookie, usedPtr) => {
    const r = fdReaddir(f, buf, bufLen, cookie, usedPtr);
    if (r !== ENOSYS || !dirs.has(f)) return r;
    let ents;
    try {
      ents = readdirSync(dirs.get(f), { withFileTypes: true });
    } catch (e) {
      return { ENOENT: 44, ENOTDIR: 54, EACCES: 2, EPERM: 63 }[e.code] ?? 29;
    }
    const all = [[".", 3], ["..", 3]];
    for (const e of ents) {
      const t = e.isDirectory() ? 3 : e.isFile() ? 4 : e.isSymbolicLink() ? 7 : 0;
      all.push([e.name, t]);
    }
    // dirents (next cookie u64, inode u64, name length u32, type u8, 3 bytes
    // of padding) each followed by the name; the last one may be cut short
    const m = new Uint8Array(memory().buffer);
    let used = 0;
    for (let i = Number(cookie); i < all.length && used < bufLen; i++) {
      const name = Buffer.from(all[i][0]);
      const ent = new Uint8Array(24 + name.length);
      const dv = new DataView(ent.buffer);
      dv.setBigUint64(0, BigInt(i + 1), true);
      dv.setBigUint64(8, 0n, true);
      dv.setUint32(16, name.length, true);
      dv.setUint8(20, all[i][1]);
      ent.set(name, 24);
      const n = Math.min(ent.length, bufLen - used);
      m.set(ent.subarray(0, n), buf + used);
      used += n;
    }
    new DataView(memory().buffer).setUint32(usedPtr, used, true);
    return 0;
  };
}
