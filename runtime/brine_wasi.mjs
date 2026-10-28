// Runs a WebAssembly build of a brine program under Node.js:
//
//     node brine_wasi.mjs program.wasm [args...]
//
// `brine run --target wasm` and `brine test --target wasm` use it.
import { readFile } from "node:fs/promises";
import { WASI } from "node:wasi";
import process from "node:process";

const [file, ...args] = process.argv.slice(2);
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
try {
  process.exitCode = wasi.start(instance);
} catch (e) {
  // a trap: brine panics exit with 101, so this is a crash (or a stack overflow)
  process.stderr.write(`\nWebAssembly trap: ${e && e.message ? e.message : e}\n`);
  process.exitCode = 134;
}
