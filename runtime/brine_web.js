// brine programs in a web browser: the WASI (preview 1) calls a WebAssembly
// build of a brine program makes (`brine build --target web`).
//
//     import { run } from "./brine.js";
//     const code = await run("program.wasm", {
//       args: ["a", "b"],
//       stdout: (text) => console.log(text),
//       stderr: (text) => console.error(text),
//     });
//
// Output arrives as text whenever the program flushes it. There is no file
// system (opening a file fails) and standard input is empty. `os.sleep`
// waits by spinning, so a program that sleeps is best run in a Web Worker.

const ERRNO_BADF = 8;
const ERRNO_INVAL = 28;
const ERRNO_NOSYS = 52;
const ERRNO_SPIPE = 70;

class Exit {
  constructor(code) {
    this.code = code;
  }
}

// Runs a module (a URL, a Response, bytes or a WebAssembly.Module) to the end.
// Returns its exit code: 0, the code it exits with, 101 after a panic.
export async function run(wasm, options = {}) {
  const args = ["program", ...(options.args || [])];
  const env = Object.entries(options.env || {}).map(([k, v]) => `${k}=${v}`);
  const out = [options.stdout || ((t) => console.log(t)), options.stderr || ((t) => console.error(t))];
  const decoders = [new TextDecoder(), new TextDecoder()];
  const enc = new TextEncoder();
  let memory = null;
  const view = () => new DataView(memory.buffer);
  const bytes = () => new Uint8Array(memory.buffer);

  // writes a list of strings for args_get/environ_get
  const putList = (list, ptrs, buf) => {
    const v = view();
    for (const s of list) {
      const b = enc.encode(s + "\0");
      v.setUint32(ptrs, buf, true);
      bytes().set(b, buf);
      ptrs += 4;
      buf += b.length;
    }
    return 0;
  };
  const sizes = (list, countPtr, sizePtr) => {
    const v = view();
    v.setUint32(countPtr, list.length, true);
    v.setUint32(sizePtr, list.reduce((n, s) => n + enc.encode(s).length + 1, 0), true);
    return 0;
  };
  const now = (id) => BigInt(Math.round(id === 0 ? Date.now() * 1e6 : performance.now() * 1e6));

  const wasi = {
    args_get: (ptrs, buf) => putList(args, ptrs, buf),
    args_sizes_get: (c, s) => sizes(args, c, s),
    environ_get: (ptrs, buf) => putList(env, ptrs, buf),
    environ_sizes_get: (c, s) => sizes(env, c, s),
    clock_res_get: (id, ptr) => (view().setBigUint64(ptr, 1000n, true), 0),
    clock_time_get: (id, precision, ptr) => (view().setBigUint64(ptr, now(id), true), 0),
    fd_write: (fd, iovs, n, written) => {
      if (fd !== 1 && fd !== 2) return ERRNO_BADF;
      const v = view();
      let total = 0;
      let text = "";
      for (let i = 0; i < n; i++) {
        const p = v.getUint32(iovs + i * 8, true);
        const len = v.getUint32(iovs + i * 8 + 4, true);
        text += decoders[fd - 1].decode(bytes().subarray(p, p + len), { stream: true });
        total += len;
      }
      if (text) out[fd - 1](text);
      v.setUint32(written, total, true);
      return 0;
    },
    fd_read: (fd, iovs, n, read) => {
      if (fd !== 0) return ERRNO_BADF;
      view().setUint32(read, 0, true);
      return 0;
    },
    fd_fdstat_get: (fd, ptr) => {
      if (fd > 2) return ERRNO_BADF;
      const v = view();
      v.setUint8(ptr, 2); // a character device
      v.setUint16(ptr + 2, 0, true);
      v.setBigUint64(ptr + 8, 0xffffffffffffffffn, true);
      v.setBigUint64(ptr + 16, 0xffffffffffffffffn, true);
      return 0;
    },
    fd_fdstat_set_flags: () => 0,
    fd_close: (fd) => (fd <= 2 ? 0 : ERRNO_BADF),
    fd_seek: (fd) => (fd <= 2 ? ERRNO_SPIPE : ERRNO_BADF),
    // no preopened folders: no file system
    fd_prestat_get: () => ERRNO_BADF,
    fd_prestat_dir_name: () => ERRNO_BADF,
    proc_exit: (code) => {
      throw new Exit(code);
    },
    random_get: (ptr, len) => {
      for (let i = 0; i < len; i += 65536) crypto.getRandomValues(bytes().subarray(ptr + i, ptr + Math.min(len, i + 65536)));
      return 0;
    },
    sched_yield: () => 0,
    // sleeping (clock subscriptions only): spins until the earliest deadline
    poll_oneoff: (subs, events, n, nevents) => {
      if (n === 0) return ERRNO_INVAL;
      const v = view();
      let until = Infinity;
      for (let i = 0; i < n; i++) {
        const s = subs + i * 48;
        if (v.getUint8(s + 8) !== 0) return ERRNO_NOSYS;
        const id = v.getUint32(s + 16, true);
        const t = Number(v.getBigUint64(s + 24, true)) / 1e6;
        const abs = (v.getUint16(s + 40, true) & 1) !== 0;
        const base = id === 0 ? Date.now() : performance.now();
        until = Math.min(until, performance.now() + (abs ? t - base : t));
      }
      while (performance.now() < until) {}
      for (let i = 0; i < n; i++) {
        const e = events + i * 32;
        v.setBigUint64(e, v.getBigUint64(subs + i * 48, true), true);
        v.setUint16(e + 8, 0, true);
        v.setUint8(e + 10, 0);
      }
      v.setUint32(nevents, n, true);
      return 0;
    },
  };
  // everything else (files, sockets) isn't there
  const imports = {
    wasi_snapshot_preview1: new Proxy(wasi, { get: (t, name) => t[name] || (() => ERRNO_NOSYS) }),
  };

  let module = wasm;
  if (typeof wasm === "string" || wasm instanceof URL) module = await fetch(wasm);
  if (module instanceof Response) module = await WebAssembly.compileStreaming(module);
  else if (!(module instanceof WebAssembly.Module)) module = await WebAssembly.compile(module);
  const instance = await WebAssembly.instantiate(module, imports);
  memory = instance.exports.memory;
  let code = 0;
  try {
    instance.exports._start();
  } catch (e) {
    if (!(e instanceof Exit)) throw e;
    code = e.code;
  }
  for (let fd = 0; fd < 2; fd++) {
    const rest = decoders[fd].decode();
    if (rest) out[fd](rest);
  }
  return code;
}
