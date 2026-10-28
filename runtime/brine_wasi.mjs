// Runs a WebAssembly build of a brine program under Node.js:
//
//     node brine_wasi.mjs program.wasm [args...]
//
// `brine run --target wasm` and `brine test --target wasm` use it.
import { readFile } from "node:fs/promises";
import { writeSync } from "node:fs";
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
  const module = await WebAssembly.compile(await readFile(file));
  const instance = await WebAssembly.instantiate(module, wasi.getImportObject());
  let code = 0;
  try {
    code = wasi.start(instance);
  } catch (e) {
    // a trap: brine panics exit with 101, so this is a crash (or a stack overflow)
    writeSync(2, `\nWebAssembly trap: ${e && e.message ? e.message : e}\n`);
    code = 134;
  }
  // the program writes straight to the file descriptors: nothing to flush
  process.exit(code);
}
