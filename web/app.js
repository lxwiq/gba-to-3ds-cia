// GBA to 3DS CIA: page logic.
//
// The converter (the Rust core compiled to WebAssembly) runs in worker.js.
// Files are kept as File objects and read with File.arrayBuffer() each time
// the worker needs them; the fresh buffer is transferred, so a detached
// buffer is never reused.

const MAX_ROM_SIZE = 32 * 1024 * 1024;

const $ = (id) => document.getElementById(id);

const els = {
  boot: $('boot'),
  appError: $('app-error'),

  romPicker: $('rom-picker'),
  romStatus: $('rom-status'),
  romError: $('rom-error'),
  romInfo: $('rom-info'),

  patchPicker: $('patch-picker'),
  patchError: $('patch-error'),
  patchInfo: $('patch-info'),
  patchCheck: $('patch-check'),

  colour: $('colour'),

  title: $('title'),
  publisher: $('publisher'),
  iconPicker: $('icon-picker'),
  iconBox: $('icon-box'),
  iconPreview: $('icon-preview'),
  iconHint: $('icon-hint'),
  bannerPicker: $('banner-picker'),
  bannerBox: $('banner-box'),
  bannerPreview: $('banner-preview'),
  bannerHint: $('banner-hint'),
  soundPicker: $('sound-picker'),
  soundPreview: $('sound-preview'),

  saveType: $('save-type'),
  rtc: $('rtc'),
  titleId: $('title-id'),

  buildCard: $('build-card'),
  build: $('build'),
  buildStatus: $('build-status'),
  buildError: $('build-error'),
  buildResult: $('build-result'),

  savPicker: $('sav-picker'),
  savKind: $('sav-kind'),
  savKindHint: $('sav-kind-hint'),
  convert: $('convert'),
  savStatus: $('sav-status'),
  savError: $('sav-error'),
  savResult: $('sav-result'),
};

const state = {
  saveTypes: [], // [{id, label, size}] from the core
  rom: null, // {file, info, crc} once inspected
  romFile: null, // the chosen ROM file, even if it could not be read
  patch: null, // {file, info}
  patched: null, // {info, crc} of the patched ROM
  patchPending: false,
  patchProblem: null, // message when the patch cannot be applied
  icon: null,
  banner: null,
  sound: null,
  sav: null,
  titleEdited: false,
  savKindTouched: false,
  building: false,
  converting: false,
  loading: Promise.resolve(), // settles when every pending file load is done
  seq: { rom: 0, patchFile: 0, patch: 0 },
  urls: { icon: null, banner: null, sound: null, cia: null, sav: null },
};

/* ------------------------------------------------------------------ */
/* Worker                                                              */
/* ------------------------------------------------------------------ */

let worker = null;
let nextId = 1;
const pending = new Map();

function startWorker() {
  try {
    worker = new Worker(new URL('./worker.js', import.meta.url), { type: 'module' });
  } catch (err) {
    worker = null;
    showAppError(startProblem(err));
    return;
  }
  worker.addEventListener('message', onWorkerMessage);
  worker.addEventListener('error', (event) => {
    event.preventDefault();
    const message = startProblem(event.message ? new Error(event.message) : null);
    failPending(message);
    showAppError(message);
  });
  worker.addEventListener('messageerror', () => failPending('A message from the converter could not be read.'));
}

function startProblem(err) {
  const detail = err && err.message ? ` (${err.message})` : '';
  if (location.protocol === 'file:') {
    return `The converter cannot start from a file opened directly${detail}. Serve the web folder with a static web server, for example: python3 -m http.server -d web`;
  }
  return `The converter could not start${detail}. Reload the page. If it keeps failing, use a current version of Chrome, Edge, Firefox or Safari.`;
}

function onWorkerMessage(event) {
  const { id, ok, result, error, crashed } = event.data || {};
  const job = pending.get(id);
  if (!job) return;
  pending.delete(id);
  if (ok) job.resolve(result);
  else job.reject(new Error(error || 'Unknown error.'));
  if (crashed) restartWorker();
}

function failPending(message) {
  for (const job of pending.values()) job.reject(new Error(message));
  pending.clear();
}

function restartWorker() {
  if (worker) worker.terminate();
  failPending('The converter was restarted. Try again.');
  startWorker();
}

function call(op, args = {}, transfer = []) {
  return new Promise((resolve, reject) => {
    if (!worker) {
      reject(new Error('The converter is not running. Reload the page.'));
      return;
    }
    const id = nextId++;
    pending.set(id, { resolve, reject });
    try {
      worker.postMessage({ id, op, ...args }, transfer);
    } catch (err) {
      pending.delete(id);
      reject(err);
    }
  });
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (value == null || value === false) continue;
    if (key === 'class') el.className = value;
    else el.setAttribute(key, value === true ? '' : String(value));
  }
  for (const child of children.flat(Infinity)) {
    if (child == null || child === false || child === '') continue;
    el.append(child instanceof Node ? child : String(child));
  }
  return el;
}

const code = (text) => h('code', {}, text);
const sub = (...children) => h('span', { class: 'sub' }, ...children);

function infoList(rows) {
  return h(
    'dl',
    { class: 'info' },
    rows.filter(Boolean).map(([term, ...desc]) => [h('dt', {}, term), h('dd', {}, ...desc)]),
  );
}

function notesList(notes) {
  return h('ul', { class: 'notes' }, notes.map((n) => h('li', {}, n)));
}

function message(kind, title, text) {
  return h('div', { class: `msg ${kind}` }, title ? h('strong', {}, title) : null, text ? h('p', {}, text) : null);
}

function show(region, ...nodes) {
  region.replaceChildren(...nodes.filter(Boolean));
}

function errorText(err) {
  const text = (err && err.message) || String(err || 'Unknown error.');
  return text.charAt(0).toUpperCase() + text.slice(1);
}

function showAppError(text) {
  els.boot.hidden = true;
  show(els.appError, message('error', 'The converter is not available', text));
}

function trimNumber(value) {
  return (value >= 10 ? value.toFixed(1) : value.toFixed(2)).replace(/\.?0+$/, '');
}

function mib(n) {
  return `${trimNumber(n / (1024 * 1024))} MiB`;
}

function formatSize(n) {
  if (n < 1024) return `${n} bytes`;
  if (n < 1024 * 1024) return `${trimNumber(n / 1024)} KiB`;
  return mib(n);
}

const baseName = (name) => name.replace(/\.[^./\\]*$/, '');

// "Pokemon - FireRed Version (USA, Europe) [!].gba" -> "Pokemon - FireRed Version"
function titleFromFileName(name) {
  return baseName(name)
    .replace(/\([^)]*\)|\[[^\]]*\]/g, ' ')
    .replace(/_/g, ' ')
    .replace(/\s+/g, ' ')
    .replace(/^[\s\-.,]+|[\s\-.,]+$/g, '')
    .slice(0, 64)
    .trim();
}

function safeFileName(text, fallback) {
  const cleaned = String(text || '')
    .normalize('NFC')
    // eslint-disable-next-line no-control-regex
    .replace(/[\u0000-\u001f\u007f"*/:<>?\\|]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
    .replace(/^\.+/, '')
    .replace(/[. ]+$/, '')
    .slice(0, 120)
    .trim();
  if (!cleaned || /^(con|prn|aux|nul|com\d|lpt\d)$/i.test(cleaned)) return fallback;
  return cleaned;
}

function offerDownload(buffer, name, slot) {
  if (state.urls[slot]) URL.revokeObjectURL(state.urls[slot]);
  const url = URL.createObjectURL(new Blob([buffer], { type: 'application/octet-stream' }));
  state.urls[slot] = url;
  const a = h('a', { href: url, download: name, hidden: true });
  document.body.append(a);
  a.click();
  a.remove();
  return url;
}

function saveLabel(id) {
  const type = state.saveTypes.find((t) => t.id === id);
  return type ? type.label : id;
}

// Wait for every file load in progress before building or converting.
function track(promise) {
  state.loading = Promise.allSettled([state.loading, promise]);
  return promise;
}

/* ------------------------------------------------------------------ */
/* File pickers                                                        */
/* ------------------------------------------------------------------ */

const hasFiles = (event) => Array.from(event.dataTransfer?.types || []).includes('Files');

function picker(root, { empty, emptySub, fullSub, onFile }) {
  const input = root.querySelector('input[type="file"]');
  const name = root.querySelector('[data-name]');
  const subLine = root.querySelector('[data-sub]');
  const clear = root.querySelector('[data-clear]');

  const display = (file) => {
    name.textContent = file ? `${file.name}, ${formatSize(file.size)}` : empty;
    if (subLine) subLine.textContent = file ? fullSub : emptySub;
    if (clear) clear.hidden = !file;
    root.classList.toggle('has-file', Boolean(file));
  };
  const pick = (file) => {
    display(file);
    onFile(file);
  };

  input.addEventListener('change', () => {
    if (input.files && input.files.length) pick(input.files[0]);
  });
  if (clear) {
    clear.addEventListener('click', () => {
      input.value = '';
      pick(null);
      input.focus();
    });
  }
  const over = (event) => {
    if (!hasFiles(event)) return;
    event.preventDefault();
    event.dataTransfer.dropEffect = 'copy';
    root.classList.add('is-over');
  };
  root.addEventListener('dragenter', over);
  root.addEventListener('dragover', over);
  root.addEventListener('dragleave', (event) => {
    if (!root.contains(event.relatedTarget)) root.classList.remove('is-over');
  });
  root.addEventListener('drop', (event) => {
    if (!hasFiles(event)) return;
    event.preventDefault();
    root.classList.remove('is-over');
    const file = event.dataTransfer.files[0];
    if (file) {
      input.value = '';
      pick(file);
    }
  });
  display(null);
}

// Dropping a file outside a picker must not make the browser open it.
// Pickers handle their own events first and call preventDefault().
window.addEventListener('dragover', (event) => {
  if (!hasFiles(event) || event.defaultPrevented) return;
  event.preventDefault();
  event.dataTransfer.dropEffect = 'none';
});
window.addEventListener('drop', (event) => {
  if (hasFiles(event)) event.preventDefault();
});

/* ------------------------------------------------------------------ */
/* ROM and patch                                                       */
/* ------------------------------------------------------------------ */

function current() {
  if (state.patched) return { info: state.patched.info, crc: state.patched.crc, patched: true };
  if (state.rom) return { info: state.rom.info, crc: state.rom.crc, patched: false };
  return null;
}

async function loadRom(file) {
  const seq = ++state.seq.rom;
  state.seq.patch++; // discard a patch preview still in progress
  state.romFile = file;
  state.rom = null;
  state.patched = null;
  state.patchPending = false;
  state.savKindTouched = false;
  show(els.romError, null);
  render();

  if (file.size > MAX_ROM_SIZE) {
    els.romStatus.textContent = '';
    show(els.romError, message('error', 'This file is too large', `${file.name} is ${mib(file.size)}. GBA ROMs are at most 32 MiB.`));
    await updatePatch();
    return;
  }

  els.romStatus.textContent = `Reading ${file.name}...`;
  try {
    const rom = await file.arrayBuffer();
    const result = await call('inspectRom', { rom }, [rom]);
    if (seq !== state.seq.rom) return;
    state.rom = { file, info: result.info, crc: result.crc };
    const { title, gameCode } = result.info;
    els.romStatus.textContent = `Loaded ${file.name}: ${title || 'no internal title'}${gameCode ? ` (${gameCode})` : ''}.`;
  } catch (err) {
    if (seq !== state.seq.rom) return;
    els.romStatus.textContent = '';
    show(els.romError, message('error', 'This ROM cannot be used', errorText(err)));
  }
  await updatePatch();
}

async function loadPatch(file) {
  const seq = ++state.seq.patchFile;
  state.seq.patch++; // discard a patch preview still in progress
  state.patch = null;
  state.patched = null;
  state.patchPending = false;
  show(els.patchError, null);
  render();
  if (file) {
    try {
      const patch = await file.arrayBuffer();
      const info = await call('inspectPatch', { patch }, [patch]);
      if (seq !== state.seq.patchFile) return;
      state.patch = { file, info };
    } catch (err) {
      if (seq !== state.seq.patchFile) return;
      show(els.patchError, message('error', 'This patch cannot be read', errorText(err)));
    }
  }
  await updatePatch();
}

// Apply the patch to the loaded ROM to show what will be built.
async function updatePatch() {
  const seq = ++state.seq.patch;
  state.patched = null;
  state.patchProblem = null;
  state.patchPending = false;
  const { rom, patch } = state;
  const expected = patch && patch.info.sourceCrc32;
  if (rom && patch && (!expected || expected === rom.crc)) {
    state.patchPending = true;
    render();
    try {
      const [romBuffer, patchBuffer] = await Promise.all([rom.file.arrayBuffer(), patch.file.arrayBuffer()]);
      const result = await call('patchRom', { rom: romBuffer, patch: patchBuffer }, [romBuffer, patchBuffer]);
      if (seq !== state.seq.patch) return;
      state.patched = result;
    } catch (err) {
      if (seq !== state.seq.patch) return;
      state.patchProblem = errorText(err);
    }
    state.patchPending = false;
  }
  render();
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

function describeSave(info) {
  const signature = info.saveSignature ? code(info.saveSignature) : null;
  switch (info.saveConfidence) {
    case 'database':
      return [sub('From the list of known games', signature ? ['; save library ', signature] : null)];
    case 'signature':
      return [sub('Save library ', signature, ' found in the ROM')];
    case 'guess':
      return [
        sub('Guessed from the save library ', signature),
        h('span', { class: 'sub warn' }, 'This is a guess. If the game does not save, choose another type under Advanced.'),
      ];
    default:
      return [sub('No save library found in the ROM')];
  }
}

function renderRomInfo() {
  const cur = current();
  if (!cur) {
    els.romInfo.hidden = true;
    els.romInfo.replaceChildren();
    return;
  }
  const { info, crc, patched } = cur;
  els.romInfo.replaceChildren(
    h('h3', {}, patched ? 'Detected (patched ROM)' : 'Detected'),
    infoList([
      ['Internal title', info.title || 'None'],
      ['Game code', info.gameCode || 'None', sub(`Maker ${info.makerCode || 'none'}, revision ${info.version}`)],
      ['Size', mib(info.size)],
      ['CRC32', code(crc)],
      ['Save type', info.saveLabel, ...describeSave(info)],
      [
        'Real-time clock',
        info.rtc ? 'Yes' : 'No',
        sub(info.rtcReason || (info.rtc ? '' : 'No clock library found in the ROM')),
      ],
      info.notes && info.notes.length ? ['Notes', notesList(info.notes)] : null,
    ]),
  );
  els.romInfo.hidden = false;
}

function patchCheck() {
  const { rom, patch, patched, patchProblem, patchPending } = state;
  const p = patch.info;
  if (!rom) return message('note', null, 'Load the base ROM above to check that it matches the patch.');
  if (p.sourceCrc32 && rom.crc !== p.sourceCrc32) {
    if (p.targetCrc32 && rom.crc === p.targetCrc32) {
      return message('warn', 'Already patched', 'The loaded ROM already has this patch applied. Remove the patch, or load the clean base ROM.');
    }
    return message(
      'error',
      'Wrong base ROM',
      `The loaded ROM has CRC32 ${rom.crc}, but this patch needs CRC32 ${p.sourceCrc32}. Use a clean, unmodified dump of the original game.`,
    );
  }
  if (patchPending) return message('note', null, 'Applying the patch...');
  if (patchProblem) return message('error', 'The patch cannot be applied', patchProblem);
  if (!patched) return null;
  const done = `Patched ROM: ${patched.info.title || 'no internal title'}, ${mib(patched.info.size)}, CRC32 ${patched.crc}. The patch is applied when you build.`;
  if (!p.sourceCrc32) {
    return message(
      'warn',
      'Base ROM not checked',
      `${p.format} patches do not record the base ROM, so the match cannot be checked. Make sure the loaded ROM is the one the patch was made for. ${done}`,
    );
  }
  return message('ok', 'The ROM matches', `The loaded ROM is the right base (CRC32 ${rom.crc}). ${done}`);
}

function renderPatch() {
  const { patch, rom } = state;
  if (!patch) {
    els.patchInfo.hidden = true;
    els.patchInfo.replaceChildren();
    show(els.patchCheck, null);
    return;
  }
  const p = patch.info;
  els.patchInfo.replaceChildren(
    infoList([
      ['Format', p.format],
      [
        'Base ROM',
        p.sourceCrc32 ? ['CRC32 ', code(p.sourceCrc32)] : 'Not recorded in the patch',
        p.sourceSize ? `, ${mib(p.sourceSize)}` : null,
      ],
      rom ? ['Loaded ROM', 'CRC32 ', code(rom.crc)] : null,
      p.targetSize ? ['Patched size', mib(p.targetSize)] : null,
    ]),
  );
  els.patchInfo.hidden = false;
  show(els.patchCheck, patchCheck());
}

function autoTitle() {
  if (state.patch) {
    const fromPatch = titleFromFileName(state.patch.file.name);
    if (fromPatch) return fromPatch;
  }
  if (state.romFile) {
    const fromRom = titleFromFileName(state.romFile.name);
    if (fromRom) return fromRom;
  }
  const cur = current();
  return cur ? cur.info.title : '';
}

function syncTitle() {
  if (!state.titleEdited) els.title.value = autoTitle();
  const cur = current();
  els.title.placeholder = (cur && cur.info.title) || 'GBA game';
}

function renderAutoLabels() {
  const cur = current();
  els.saveType.options[0].textContent = cur ? `Auto: ${cur.info.saveLabel}` : 'Auto (detect from the ROM)';
  els.rtc.options[0].textContent = cur ? `Auto: ${cur.info.rtc ? 'on' : 'off'}` : 'Auto (detect from the ROM)';
}

// The save type the build will use, when a ROM is loaded.
function gameSaveType() {
  if (els.saveType.value !== 'auto') return els.saveType.value;
  const cur = current();
  return cur ? cur.info.save : null;
}

function syncSaveKind() {
  const game = gameSaveType();
  if (!state.savKindTouched) {
    const usable = game && game !== 'none' && Array.from(els.savKind.options).some((o) => o.value === game);
    els.savKind.value = usable ? game : 'auto';
  }
  els.savKindHint.textContent =
    game && game !== 'none'
      ? `Auto picks the type from the file size. The loaded game uses ${saveLabel(game)}; the save must match it.`
      : 'Auto picks the type from the file size. It must match the game.';
}

function render() {
  renderRomInfo();
  renderPatch();
  syncTitle();
  renderAutoLabels();
  syncSaveKind();
}

/* ------------------------------------------------------------------ */
/* HOME Menu images and sound                                          */
/* ------------------------------------------------------------------ */

const IMAGE_HINTS = {
  icon: {
    none: 'A plain default is generated.',
    chosen: (w, ht) => `${w} x ${ht} pixels. Cropped to a centred square and shown at 48 x 48.`,
    small: (w, ht) => Math.min(w, ht) <= 48,
  },
  banner: {
    none: 'A plain default is generated.',
    chosen: (w, ht) => `${w} x ${ht} pixels. Fitted inside 256 x 128, shown on the top screen when the title is selected.`,
    small: (w, ht) => w <= 256 && ht <= 128,
  },
};

function setImage(kind, file) {
  const box = els[`${kind}Box`];
  const img = els[`${kind}Preview`];
  const hint = els[`${kind}Hint`];
  const text = IMAGE_HINTS[kind];
  state[kind] = file;
  if (state.urls[kind]) URL.revokeObjectURL(state.urls[kind]);
  state.urls[kind] = null;
  img.onload = null;
  img.onerror = null;
  img.removeAttribute('src');
  box.hidden = true;
  if (!file) {
    hint.textContent = text.none;
    return;
  }
  const url = URL.createObjectURL(file);
  state.urls[kind] = url;
  hint.textContent = 'Loading the preview...';
  img.onload = () => {
    const { naturalWidth: w, naturalHeight: ht } = img;
    img.classList.toggle('pixelated', text.small(w, ht));
    box.hidden = false;
    hint.textContent = text.chosen(w, ht);
  };
  img.onerror = () => {
    hint.textContent = 'This browser cannot show this image. The build may fail; a PNG file is the safest choice.';
  };
  img.src = url;
}

function setSound(file) {
  state.sound = file;
  if (state.urls.sound) URL.revokeObjectURL(state.urls.sound);
  state.urls.sound = null;
  els.soundPreview.removeAttribute('src');
  els.soundPreview.hidden = !file;
  if (file) {
    state.urls.sound = URL.createObjectURL(file);
    els.soundPreview.src = state.urls.sound;
  }
  els.soundPreview.load();
}

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

function setBuilding(on) {
  state.building = on;
  els.build.disabled = on;
  els.build.textContent = on ? 'Building...' : 'Build .cia';
  els.buildCard.setAttribute('aria-busy', on ? 'true' : 'false');
}

function buildOptions() {
  const mode = document.querySelector('input[name="mode"]:checked');
  return {
    mode: mode ? mode.value : 'vc',
    title: els.title.value.trim(),
    publisher: els.publisher.value.trim(),
    titleId: els.titleId.value.replace(/\s+/g, ''),
    save: els.saveType.value,
    rtc: els.rtc.value,
    scale: 'full',
    colour: els.colour.value,
  };
}

async function build() {
  if (state.building) return;
  show(els.buildError, null);
  els.buildStatus.textContent = '';

  setBuilding(true);
  els.buildStatus.textContent = 'Building... Large ROMs take a few seconds.';
  try {
    await state.loading;
    if (!state.rom) {
      els.buildStatus.textContent = '';
      const text = state.romFile
        ? 'The chosen ROM cannot be used. Choose another file in section 1.'
        : 'Choose a ROM in section 1 first.';
      show(els.buildError, message('error', 'Nothing to build', text));
      return;
    }

    const files = [state.rom.file, state.patch && state.patch.file, state.icon, state.banner, state.sound];
    const [rom, patch, icon, banner, sound] = await Promise.all(files.map((f) => (f ? f.arrayBuffer() : null)));
    const options = buildOptions();
    const result = await call(
      'build',
      { rom, patch, icon, banner, sound, options },
      [rom, patch, icon, banner, sound].filter(Boolean),
    );

    const name = `${safeFileName(result.report.title, 'game')}.cia`;
    const url = offerDownload(result.cia, name, 'cia');
    renderBuildResult(result.report, options, url, name);
    els.buildStatus.textContent = `Done. ${name} is downloading.`;
  } catch (err) {
    els.buildStatus.textContent = '';
    show(els.buildError, message('error', 'The build failed', errorText(err)));
  } finally {
    setBuilding(false);
  }
}

function renderBuildResult(report, options, url, name) {
  els.buildResult.replaceChildren(
    h('h3', {}, 'Built'),
    infoList([
      ['Title', report.title],
      ['Title ID', code(report.titleId)],
      ['Product code', code(report.productCode)],
      ['Output', report.mode],
      options.mode === 'vc' ? ['Colours', options.colour === 'original' ? 'Original GBA screen' : 'Vivid'] : null,
      ['Save type', report.saveLabel],
      ['Real-time clock', report.rtc ? 'On' : 'Off'],
      ['Size', mib(report.size)],
      report.notes && report.notes.length ? ['Notes', notesList(report.notes)] : null,
    ]),
    h('p', { class: 'download' }, 'If the download did not start: ', h('a', { href: url, download: name }, `download ${name}`), '.'),
    h(
      'p',
      { class: 'note' },
      'To keep your progress, quit the game with HOME. Forcing the console off, or an empty battery, loses the progress since the last quit.',
    ),
  );
  els.buildResult.hidden = false;
}

/* ------------------------------------------------------------------ */
/* Save conversion                                                     */
/* ------------------------------------------------------------------ */

function convertedSaveName(file) {
  const title = els.title.value.trim() || (state.rom ? autoTitle() : '');
  if (title) return `${safeFileName(title, 'game')}.gbavc.sav`;
  const base = safeFileName(baseName(file.name), 'save');
  return /\.sav$/i.test(file.name) ? `${base}.gbavc.sav` : `${base}.sav`;
}

async function convert() {
  if (state.converting) return;
  show(els.savError, null);
  els.savStatus.textContent = '';
  if (!state.sav) {
    show(els.savError, message('error', 'No save chosen', 'Choose a .sav or .srm file first.'));
    return;
  }

  state.converting = true;
  els.convert.disabled = true;
  els.convert.textContent = 'Converting...';
  els.savStatus.textContent = 'Converting...';
  try {
    await state.loading;
    const file = state.sav;
    const sav = await file.arrayBuffer();
    const result = await call('convertSave', { sav, kind: els.savKind.value }, [sav]);
    const name = convertedSaveName(file);
    const url = offerDownload(result.data, name, 'sav');
    renderSaveResult(result.report, url, name);
    els.savStatus.textContent = `Done. ${name} is downloading.`;
  } catch (err) {
    els.savStatus.textContent = '';
    show(els.savError, message('error', 'The save cannot be converted', errorText(err)));
  } finally {
    state.converting = false;
    els.convert.disabled = false;
    els.convert.textContent = 'Convert';
  }
}

function renderSaveResult(report, url, name) {
  const game = state.rom ? gameSaveType() : null;
  const mismatch = game && game !== 'none' && game !== report.save;
  show(
    els.savResult,
    h('h3', {}, 'Converted'),
    infoList([
      ['Save type', report.saveLabel],
      ['Size', `${formatSize(report.size)} (${report.size.toLocaleString('en-US')} bytes)`],
      report.notes && report.notes.length ? ['Notes', notesList(report.notes)] : null,
    ]),
    mismatch
      ? message('warn', 'Save type differs from the game', `The loaded game uses ${saveLabel(game)}. The save type must match the game, or the save will not load.`)
      : null,
    h('p', { class: 'download' }, 'If the download did not start: ', h('a', { href: url, download: name }, `download ${name}`), '.'),
  );
  els.savResult.hidden = false;
}

/* ------------------------------------------------------------------ */
/* Start                                                               */
/* ------------------------------------------------------------------ */

function fillSaveTypes(types) {
  state.saveTypes = types;
  for (const t of types) {
    els.saveType.append(h('option', { value: t.id }, t.label));
    if (t.size > 0) els.savKind.append(h('option', { value: t.id }, t.label));
  }
  render();
}

function wire() {
  picker(els.romPicker, {
    empty: 'Choose a ROM file',
    emptySub: 'or drop it here: .gba, .agb or .bin',
    fullSub: 'Choose another file, or drop it here',
    onFile: (file) => {
      if (file) track(loadRom(file));
    },
  });
  picker(els.patchPicker, { empty: 'No patch chosen', onFile: (file) => track(loadPatch(file)) });
  picker(els.iconPicker, { empty: 'No image chosen', onFile: (file) => setImage('icon', file) });
  picker(els.bannerPicker, { empty: 'No image chosen', onFile: (file) => setImage('banner', file) });
  picker(els.soundPicker, { empty: 'No sound chosen', onFile: setSound });
  picker(els.savPicker, {
    empty: 'No save chosen',
    onFile: (file) => {
      state.sav = file;
      show(els.savError, null);
    },
  });

  els.title.addEventListener('input', () => {
    state.titleEdited = els.title.value.trim() !== '';
  });
  els.title.addEventListener('blur', () => {
    if (!state.titleEdited) syncTitle();
  });
  els.saveType.addEventListener('change', syncSaveKind);
  els.savKind.addEventListener('change', () => {
    state.savKindTouched = true;
  });
  els.build.addEventListener('click', build);
  els.convert.addEventListener('click', convert);
}

els.boot.hidden = true;
wire();
startWorker();
render();
if (worker) {
  call('saveTypes')
    .then(fillSaveTypes)
    .catch((err) => showAppError(errorText(err)));
}
