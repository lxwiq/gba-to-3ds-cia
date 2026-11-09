// GBA to 3DS CIA: conversion worker.
//
// Runs the WebAssembly core off the main thread so the page never freezes.
// The worker keeps no files between requests: the page sends the bytes it
// needs each time, as transferred ArrayBuffers.
//
// Request:  {id, op, ...arguments}
// Reply:    {id, ok: true, result}  or  {id, ok: false, error, crashed}

import init, * as core from './pkg/gba2cia_wasm.js';

const ready = init().catch((err) => {
  throw new Error(`Could not load the converter (gba2cia_wasm_bg.wasm): ${messageOf(err)}`);
});
// Marked as handled here; every request awaits it and reports the failure.
ready.catch(() => {});

function messageOf(err) {
  if (err instanceof Error) return err.message || err.name;
  return String(err);
}

const bytes = (buffer) => (buffer ? new Uint8Array(buffer) : undefined);

// wasm-bindgen returns fresh copies, but make sure the view covers its whole
// buffer before transferring it.
function ownBuffer(view) {
  return view.byteOffset === 0 && view.byteLength === view.buffer.byteLength
    ? view.buffer
    : view.slice().buffer;
}

function inspect(rom) {
  return { info: JSON.parse(core.inspectRom(rom)), crc: core.crc32(rom) };
}

const ops = {
  saveTypes() {
    return { result: JSON.parse(core.saveTypes()) };
  },

  inspectRom({ rom }) {
    return { result: inspect(bytes(rom)) };
  },

  inspectPatch({ patch }) {
    return { result: JSON.parse(core.inspectPatch(bytes(patch))) };
  },

  // Apply the patch only to describe the result; the build applies it again.
  patchRom({ rom, patch }) {
    const out = core.applyPatch(bytes(rom), bytes(patch));
    try {
      return { result: inspect(out) };
    } catch (err) {
      throw new Error(`The patched file is not a valid GBA ROM. ${messageOf(err)}`);
    }
  },

  build({ rom, patch, options, icon, banner, sound, sav }) {
    let data = bytes(rom);
    if (patch) data = core.applyPatch(data, bytes(patch));
    const built = core.buildCia(
      data,
      JSON.stringify(options),
      bytes(icon),
      bytes(banner),
      bytes(sound),
      bytes(sav),
    );
    data = null;
    try {
      const report = JSON.parse(built.report());
      const cia = ownBuffer(built.cia());
      return { result: { cia, report }, transfer: [cia] };
    } finally {
      built.free();
    }
  },

  convertSave({ sav, kind }) {
    const converted = core.convertSave(bytes(sav), kind || 'auto');
    try {
      const report = JSON.parse(converted.report());
      const data = ownBuffer(converted.data());
      return { result: { data, report }, transfer: [data] };
    } finally {
      converted.free();
    }
  },
};

self.addEventListener('message', async (event) => {
  const { id, op, ...args } = event.data || {};
  try {
    await ready;
    if (!Object.hasOwn(ops, op)) throw new Error(`Unknown operation: ${op}`);
    const { result, transfer = [] } = ops[op](args);
    self.postMessage({ id, ok: true, result }, transfer);
  } catch (err) {
    // A trap (panic, out of memory) can leave the instance unusable: the
    // page restarts the worker when `crashed` is set.
    const crashed = err instanceof WebAssembly.RuntimeError;
    const error = crashed
      ? `The converter stopped unexpectedly (${messageOf(err)}). This usually means the browser ran out of memory. Close other tabs and try again.`
      : messageOf(err);
    self.postMessage({ id, ok: false, crashed, error });
  }
});
