#!/usr/bin/env node
// Unsleep icon converter.
//
// Turns the human-selected GPT artwork (a flat six-column contact sheet) into
// the shipped icon resources. This tool does NOT design anything: the artwork is
// fixed, and every pixel operation here is a deterministic mechanical step:
//
//   1. crop one whole cell of the sheet onto one canvas shared by every variant;
//   2. key the near-white background out to alpha (coverage + colour unmix);
//   3. resample to 16/20/24/32/48/64/256 (exact area average) - every variant at
//      every size is resampled from that ONE canvas, so the alpha of two
//      variants of the same state is identical unless the artwork itself
//      differs;
//   4. map luminance onto palette-aware inks (light/dark taskbar);
//   5. pack the seven sizes into a PNG-in-ICO container.
//
// The sheet already carries every state as a drawing of its own. What the six
// cells actually contain, read off the source pixels (see DESIGN.md):
//
//   ON       cup + steam + face, no mark          -> running
//   OFF      cup only, no mark                    -> stopped
//   SCREEN   cup + steam + face + solid screen    -> keep-awake in force
//   TIMER    cup + steam + face + clock           -> finite duration
//   BOTH     cup + steam + face + left solid screen + right clock -> both
//   PAUSED   cup + steam + face + left DASHED screen, and nothing else
//                                                  -> screen deferred
//
// PAUSED carries ONE mark: a dashed monitor. It has no clock, no pause sector
// and no hatched interior - earlier revisions of this file wrongly assumed a
// clock and moved one. Do not reintroduce PAUSED_CLOCK / moveClock.
//
// Five of the seven runtime states are therefore one whole cell, unchanged:
// OFF, ON, SCREEN, TIMER, BOTH and PAUSED (plain deferred). Only ONE state is a
// composition the sheet does not draw:
//
//   PAUSED + TIMER == deferred with a duration still running
//     = the BOTH cell (cup + solid screen + clock), with the LEFT solid screen
//       replaced by the PAUSED cell's dashed screen at the same size and place.
//       The right clock is BOTH's own, untouched.
//
// That replacement copies existing pixels of the source sheet. It never draws a
// line: the dashed screen is lifed out of PAUSED as a block and stamped over the
// cleared solid screen in BOTH at the identical canvas rectangle.
//
// Pure Node.js standard library (node:fs, node:path, node:crypto, node:zlib).
// No dependency, no network, no clock, no randomness, no locale: the output is a
// pure function of the source PNG bytes plus the constants below, so the
// resources can be re-derived byte for byte by anyone.
//
// Usage (project-local, never global):
//   node resources/icons/generate-icons-gpt.mjs --source        # install the source
//   node resources/icons/generate-icons-gpt.mjs                 # write the resources
//   node resources/icons/generate-icons-gpt.mjs --check         # compare only
//   node resources/icons/generate-icons-gpt.mjs --sheet <dir>    # reviewer sheets
//   node resources/icons/generate-icons-gpt.mjs --preview <dir> # per-size PNGs
//   node resources/icons/generate-icons-gpt.mjs --report        # coverage table
//   node resources/icons/generate-icons-gpt.mjs --dump <txt>    # composition
//
// The source artwork was drawn by GPT for this project; docs/RESOURCE-SOURCES.md
// records the file and its SHA-256. It is licensed under the repository MIT
// licence together with everything generated from it.

import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const RESOURCES = path.join(ROOT, 'resources');

// --- source geometry -------------------------------------------------------

// Where the design file lives when it is being installed from, and the copy
// kept under resources/ so the conversion is reproducible from the repository.
const SOURCE_ORIGIN = path.join(ROOT, '.pi', 'design', 'unsleep-gpt-cup-refinement-002.png');
const SOURCE_COPY = 'resources/icons/source/gpt-cup-refinement-002.png';
const SOURCE_SHA256 = '8A421526679EDA08DE44FBD12676ABB3DE2034958DB7B75B04E03C9F021E4F58';

// Cell boxes measured on the 1536x1024 sheet with a connected-component pass,
// not guessed. The y range is common to every cell: row 233 is the top of the
// steam and 436 the bottom of the lowest mark, so every cell is cropped with the
// same vertical extent. Horizontally each box is that cell's own drawing: no
// label (the label row starts below y=440), no separator, no background.
//
// The drawings are 177..186 px wide because they are hand-drawn, so they are NOT
// normalised to each other. Each crop keeps its own left edge in the carrier, so
// the cell columns land at the same relative offset and the cup body does not
// jump between states. The canvas is larger than every crop, so nothing is ever
// clipped and all variants keep the same scale.
const CELL_CANVAS_Y = 233;
const CELL_CANVAS_H = 204;
const CELLS = {
  ON: { x: 64, x1: 242 }, //        cup + steam + face; no configuration mark
  OFF: { x: 313, x1: 490 }, //      cup only: no steam, no face, no mark
  SCREEN: { x: 555, x1: 733 }, //   cup + steam + face + solid screen (right)
  TIMER: { x: 799, x1: 978 }, //    cup + steam + face + clock (right)
  BOTH: { x: 1044, x1: 1229 }, //   cup + steam + face + solid screen + clock
  PAUSED: { x: 1300, x1: 1478 }, // cup + steam + face + DASHED screen only
};

// The shared carrier, in source pixels. 226 is the widest cell (BOTH) plus a few
// pixels of slack, so the drawing keeps the whole square and never reaches the
// icon edge at any size.
const CARRIER_WIDTH = 226;
const CARRIER_HEIGHT = CELL_CANVAS_H + (226 - 204);

// The two monitor drawings, measured on the sheet (see the probe output in
// .pi/reports/t11-cup-final-011/evidence/). Both are the same drawing - frame,
// neck, stand - at the same size; the only difference is that one is stroked
// solid and the other dashed. These rectangles are therefore swappable 1:1.
//
// The task names the source rectangles, and the measurements confirm them:
//   BOTH left screen   [1043,376]..[1115,437]  (73 x 62)
//   PAUSED dashed      [1407,377]..[1479,437]  (73 x 61)
// The two differ by one row only, so the stamp is placed at the sheet-relative
// offset and no scaling is needed.
const SCREENS = {
  // Solid screen inside the BOTH cell (left monitor).
  BOTH_SOLID: { x0: 1043, y0: 376, x1: 1115, y1: 437 },
  // Dashed screen inside the PAUSED cell (the cell's only mark).
  PAUSED_DASHED: { x0: 1407, y0: 377, x1: 1479, y1: 437 },
};

// Background key. The sheet's background is white with a faint paper tone
// (individual pixels sit at 249..255). Anything whose minimum channel is within
// `floor` of 255 is treated as background, which removes the paper tone without
// touching real edges: the thinnest real stroke on this sheet is 6 px wide, so a
// covered edge pixel is never this faint. Above the floor a pixel is partly
// covered, and the white is divided back out so half-covered edges keep the
// artist's ink colour instead of arriving washed out.
const KEY_FLOOR = 14;

// --- resource table --------------------------------------------------------

const ICO_SIZES = [16, 20, 24, 32, 48, 64, 256];

// `background` is the taskbar the file is drawn for, `palette` the ink it
// carries; a light taskbar therefore pairs with the dark palette. Kept in step
// with resources/unsleep.rc and src/win32.cpp.
const INKS = {
  dark: { stroke: [0x1c, 0x1c, 0x1c], steam: [0x1b, 0x6b, 0x2f] },
  light: { stroke: [0xe8, 0xe8, 0xe8], steam: [0x60, 0xc9, 0x72] },
};

// Reference taskbar surfaces, used only for the contrast report.
const SURFACES = { light: [0xf3, 0xf3, 0xf3], dark: [0x20, 0x20, 0x20] };

// Every tray variant draws the state it reports. Six of the seven runtime states
// are a single source cell, used whole and unmodified apart from colour; the
// light/dark pair of a state therefore shares its cell and its alpha exactly.
// Only "deferred + duration" is a composition (see `compose`).
const VARIANTS = [
  { file: 'light-inactive.ico', id: 130, background: 'light', palette: 'dark', cell: 'OFF' },
  { file: 'light-active.ico', id: 140, background: 'light', palette: 'dark', cell: 'ON' },
  { file: 'light-screen.ico', id: 150, background: 'light', palette: 'dark', cell: 'SCREEN' },
  { file: 'light-screen-paused.ico', id: 151, background: 'light', palette: 'dark', cell: 'PAUSED' },
  { file: 'light-duration.ico', id: 152, background: 'light', palette: 'dark', cell: 'TIMER' },
  { file: 'light-screen-duration.ico', id: 153, background: 'light', palette: 'dark', cell: 'BOTH' },
  { file: 'light-screen-paused-duration.ico', id: 154, background: 'light', palette: 'dark', cell: 'BOTH', dashedScreen: true },
  { file: 'dark-inactive.ico', id: 144, background: 'dark', palette: 'light', cell: 'OFF' },
  { file: 'dark-active.ico', id: 145, background: 'dark', palette: 'light', cell: 'ON' },
  { file: 'dark-screen.ico', id: 155, background: 'dark', palette: 'light', cell: 'SCREEN' },
  { file: 'dark-screen-paused.ico', id: 156, background: 'dark', palette: 'light', cell: 'PAUSED' },
  { file: 'dark-duration.ico', id: 157, background: 'dark', palette: 'light', cell: 'TIMER' },
  { file: 'dark-screen-duration.ico', id: 158, background: 'dark', palette: 'light', cell: 'BOTH' },
  { file: 'dark-screen-paused-duration.ico', id: 159, background: 'dark', palette: 'light', cell: 'BOTH', dashedScreen: true },
  // Application icon: keeps its long-standing id and is the plain ON cup - the
  // product mark, not a snapshot of two switches that happen to be on. It is the
  // same drawing as light-active.ico and therefore byte-identical: intended.
  { file: 'app.ico', id: 128, background: 'light', palette: 'dark', cell: 'ON' },
];

// --- PNG decoding ----------------------------------------------------------

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; ++n) {
    let c = n;
    for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(buffer) {
  let c = 0xffffffff;
  for (let i = 0; i < buffer.length; ++i) c = CRC_TABLE[(c ^ buffer[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length, 0);
  const typeAndData = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(typeAndData), 0);
  return Buffer.concat([length, typeAndData, crc]);
}

const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

function paeth(a, b, c) {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  return pb <= pc ? b : c;
}

// Decodes the PNG subset the source uses (8-bit truecolour, with or without
// alpha, all five filters). Returns RGBA.
function decodePng(buffer) {
  if (!buffer.subarray(0, 8).equals(PNG_SIGNATURE)) throw new Error('not a PNG');
  let offset = 8;
  let width = 0;
  let height = 0;
  let depth = 0;
  let colorType = 0;
  let interlace = 0;
  const idat = [];
  while (offset + 8 <= buffer.length) {
    const length = buffer.readUInt32BE(offset);
    const type = buffer.toString('ascii', offset + 4, offset + 8);
    const data = buffer.subarray(offset + 8, offset + 8 + length);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      depth = data[8];
      colorType = data[9];
      interlace = data[12];
    } else if (type === 'IDAT') {
      idat.push(data);
    } else if (type === 'IEND') {
      break;
    }
    offset += 12 + length;
  }
  if (depth !== 8) throw new Error(`unsupported bit depth ${depth}`);
  if (interlace !== 0) throw new Error('interlaced PNG not supported');
  const channels = colorType === 2 ? 3 : colorType === 6 ? 4 : 0;
  if (channels === 0) throw new Error(`unsupported colour type ${colorType}`);
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = width * channels;
  const pixels = Buffer.alloc(stride * height);
  for (let y = 0; y < height; ++y) {
    const filter = raw[y * (stride + 1)];
    const source = y * (stride + 1) + 1;
    const target = y * stride;
    for (let x = 0; x < stride; ++x) {
      const left = x >= channels ? pixels[target + x - channels] : 0;
      const up = y > 0 ? pixels[target - stride + x] : 0;
      const upLeft = y > 0 && x >= channels ? pixels[target - stride + x - channels] : 0;
      const value = raw[source + x];
      let out;
      switch (filter) {
        case 0: out = value; break;
        case 1: out = value + left; break;
        case 2: out = value + up; break;
        case 3: out = value + ((left + up) >> 1); break;
        case 4: out = value + paeth(left, up, upLeft); break;
        default: throw new Error(`bad PNG filter ${filter}`);
      }
      pixels[target + x] = out & 0xff;
    }
  }
  const rgba = Buffer.alloc(width * height * 4);
  for (let i = 0; i < width * height; ++i) {
    rgba[i * 4] = pixels[i * channels];
    rgba[i * 4 + 1] = pixels[i * channels + 1];
    rgba[i * 4 + 2] = pixels[i * channels + 2];
    rgba[i * 4 + 3] = channels === 4 ? pixels[i * channels + 3] : 255;
  }
  return { width, height, rgba };
}

// Encodes RGBA as 8-bit truecolour+alpha PNG. Filter 0 on every scanline keeps
// the bytes a pure function of the pixels (no adaptive heuristic).
function encodePng(width, height, rgba) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;
  ihdr[9] = 6;
  const raw = Buffer.alloc(height * (width * 4 + 1));
  for (let y = 0; y < height; ++y) {
    raw[y * (width * 4 + 1)] = 0;
    Buffer.from(rgba.buffer, rgba.byteOffset + y * width * 4, width * 4).copy(raw, y * (width * 4 + 1) + 1);
  }
  return Buffer.concat([
    PNG_SIGNATURE,
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

function encodeIco(entries) {
  const header = Buffer.alloc(6);
  header.writeUInt16LE(0, 0);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(entries.length, 4);
  const directory = Buffer.alloc(16 * entries.length);
  let offset = 6 + 16 * entries.length;
  for (const [index, entry] of entries.entries()) {
    const at = index * 16;
    directory[at] = entry.size >= 256 ? 0 : entry.size;
    directory[at + 1] = entry.size >= 256 ? 0 : entry.size;
    directory.writeUInt16LE(1, at + 4);
    directory.writeUInt16LE(32, at + 6);
    directory.writeUInt32LE(entry.png.length, at + 8);
    directory.writeUInt32LE(offset, at + 12);
    offset += entry.png.length;
  }
  return Buffer.concat([header, directory, ...entries.map((entry) => entry.png)]);
}

// --- keying ---------------------------------------------------------------

function sha256(buffer) {
  return createHash('sha256').update(buffer).digest('hex').toUpperCase();
}

function loadSheet() {
  const copy = path.join(ROOT, SOURCE_COPY);
  if (!existsSync(copy)) {
    throw new Error(`missing ${SOURCE_COPY}; run with --source first`);
  }
  const bytes = readFileSync(copy);
  const hash = sha256(bytes);
  if (hash !== SOURCE_SHA256) {
    throw new Error(`source hash ${hash} does not match the recorded ${SOURCE_SHA256}`);
  }
  return decodePng(bytes);
}

// Distance from white, via the minimum channel: the ink is neutral dark and the
// steam is green, so the minimum channel measures coverage for both and ignores
// chroma noise in the background.
function covered(r, g, b) {
  return 255 - Math.min(r, g, b);
}

// Alpha in 0..1 plus the unmixed ink colour for one source pixel.
function keyPixel(r, g, b) {
  const distance = covered(r, g, b);
  if (distance <= KEY_FLOOR) return null;
  const coverage = (distance - KEY_FLOOR) / (255 - KEY_FLOOR);
  const color = [r, g, b].map((value) => {
    const unmixed = (value - (1 - coverage) * 255) / coverage;
    return Math.max(0, Math.min(255, unmixed));
  });
  return { coverage, color };
}

// The sheet's ink is one dark colour plus green steam, so ink identity is
// decided by chroma: green-dominant, clearly saturated pixels are steam.
function isSteam(color) {
  const [r, g, b] = color;
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  return g >= r + 18 && g >= b + 8 && max - min > 22;
}

// --- layers ---------------------------------------------------------------

// A layer is a float RGBA image: colour in 0..255, alpha in 0..1, unpremultiplied.
function extractLayer(box) {
  const width = box.x1 - box.x0 + 1;
  const height = box.y1 - box.y0 + 1;
  const rgba = new Float64Array(width * height * 4);
  for (let y = 0; y < height; ++y) {
    for (let x = 0; x < width; ++x) {
      const at = ((box.y0 + y) * sheet.width + box.x0 + x) * 4;
      const keyed = keyPixel(sheet.rgba[at], sheet.rgba[at + 1], sheet.rgba[at + 2]);
      if (!keyed) continue;
      const to = (y * width + x) * 4;
      rgba[to] = keyed.color[0];
      rgba[to + 1] = keyed.color[1];
      rgba[to + 2] = keyed.color[2];
      rgba[to + 3] = keyed.coverage;
    }
  }
  return { width, height, rgba };
}

// Where a cell's crop sits in carrier coordinates. Every cell is cropped with
// the same y range and keeps its own left edge at the SAME column of the
// carrier, so the six cells stay in register with each other and the cup body
// does not shift between states. Centring each crop instead would move the cup
// because the cells differ in width by up to 9 px.
const CARRIER_TOP = Math.round((CARRIER_HEIGHT - CELL_CANVAS_H) / 2);

// One shared left margin. Every cell starts there, so cell-relative and
// carrier-relative x differ by the same constant for all six cells.
const CARRIER_LEFT = 8;
function carrierLeft() {
  return CARRIER_LEFT;
}

// Converts a sheet coordinate pair into carrier coordinates using the cell it
// was measured in. The single place where cell->carrier geometry is decided.
function toCarrier(cellName, x, y) {
  return { x: carrierLeft(cellName) + (x - CELLS[cellName].x),
           y: CARRIER_TOP + (y - CELL_CANVAS_Y) };
}

// Draws a cell's drawing into the carrier, unmodified.
function cellCarrier(name) {
  const cell = CELLS[name];
  const canvas = { width: CARRIER_WIDTH, height: CARRIER_HEIGHT,
                   rgba: new Float64Array(CARRIER_WIDTH * CARRIER_HEIGHT * 4) };
  const layer = extractLayer({ x0: cell.x, y0: CELL_CANVAS_Y, x1: cell.x1, y1: CELL_CANVAS_Y + CELL_CANVAS_H - 1 });
  paste(canvas, layer, carrierLeft(name), CARRIER_TOP);
  return canvas;
}

// Copies a rectangle of the sheet onto the canvas at an explicit carrier
// position. The pixels are copied, never drawn; the rectangle is taken exactly
// as it appears in the source, ink and transparency included.
function stampBox(canvas, box, at) {
  const shape = extractLayer(box);
  paste(canvas, shape, at.x, at.y);
  return { x: at.x, y: at.y, width: shape.width, height: shape.height };
}

// Source-over paste at an integer position.
function paste(canvas, shape, x0, y0) {
  for (let y = 0; y < shape.height; ++y) {
    for (let x = 0; x < shape.width; ++x) {
      const from = (y * shape.width + x) * 4;
      const alpha = shape.rgba[from + 3];
      if (alpha <= 0) continue;
      const tx = x0 + x;
      const ty = y0 + y;
      if (tx < 0 || ty < 0 || tx >= canvas.width || ty >= canvas.height) continue;
      const to = (ty * canvas.width + tx) * 4;
      const out = 1 - (1 - canvas.rgba[to + 3]) * (1 - alpha);
      if (out <= 0) continue;
      for (let c = 0; c < 3; ++c) {
        canvas.rgba[to + c] = (shape.rgba[from + c] * alpha +
                               canvas.rgba[to + c] * canvas.rgba[to + 3] * (1 - alpha)) / out;
      }
      canvas.rgba[to + 3] = out;
    }
  }
  return canvas;
}

// Erases the ink inside `box` (carrier coordinates): the mark is already in the
// drawing there and is either moved or replaced, so the old pixels go.
function eraseMark(canvas, box) {
  for (let y = box.y0; y <= box.y1; ++y) {
    for (let x = box.x0; x <= box.x1; ++x) {
      const at = (y * canvas.width + x) * 4;
      canvas.rgba[at] = 0;
      canvas.rgba[at + 1] = 0;
      canvas.rgba[at + 2] = 0;
      canvas.rgba[at + 3] = 0;
    }
  }
}

// The source-pixel layer of one variant, plus the composition record used by
// --report and --dump.
//
// Six of the seven runtime states need no composition at all: the cell already
// is the state.
//
// The single composed state is "screen deferred with a duration still running"
// (ids 154/159). The sheet does not draw it, so it is built from the two cells
// that each contribute one existing element:
//
//   1. start from the whole BOTH cell: cup + steam + face + left solid screen +
//      right clock;
//   2. clear the BOTH left screen rectangle (carrier rectangle of SCREENS.BOTH_SOLID);
//   3. stamp PAUSED's dashed screen (SCREENS.PAUSED_DASHED, the same drawing at
//      the same size) at that cleared rectangle.
//
// The cup and the right clock are never touched, and no line is drawn: the
// dashed screen is the artwork's own pixels. Both rectangles are 73 px wide, and
// differ by one row in height, so the stamp is placed at the cleared rectangle's
// origin and no scaling step is needed.
function composeVariantLayer(variant) {
  const canvas = cellCarrier(variant.cell);
  const placement = { cell: variant.cell, moves: [] };
  if (variant.dashedScreen) {
    const solid = SCREENS.BOTH_SOLID;
    const dashed = SCREENS.PAUSED_DASHED;
    const dest = toCarrier('BOTH', solid.x0, solid.y0);
    // Clear the solid screen exactly where it is drawn (inclusive bounds).
    eraseMark(canvas, { x0: dest.x, y0: dest.y,
                        x1: dest.x + (solid.x1 - solid.x0),
                        y1: dest.y + (solid.y1 - solid.y0) });
    const at = stampBox(canvas, dashed, dest);
    placement.moves.push({
      mark: 'DASHED_SCREEN',
      from: { cell: 'PAUSED', box: dashed },
      replaced: { cell: 'BOTH', box: solid },
      to: at,
      srcSize: { w: dashed.x1 - dashed.x0 + 1, h: dashed.y1 - dashed.y0 + 1 },
      scale: 1,
      delta: { dx: dashed.x0 - solid.x0, dy: dashed.y0 - solid.y0 },
    });
  }
  return { canvas, placement };
}

// Exact area-average resample to size x size, premultiplied during the average
// so a partially covered edge pixel does not darken.
function resample(layer, size) {
  const out = new Float64Array(size * size * 4);
  const sx = layer.width / size;
  const sy = layer.height / size;
  for (let y = 0; y < size; ++y) {
    const y0 = y * sy;
    const y1 = y0 + sy;
    for (let x = 0; x < size; ++x) {
      const x0 = x * sx;
      const x1 = x0 + sx;
      let r = 0;
      let g = 0;
      let b = 0;
      let a = 0;
      let weight = 0;
      for (let py = Math.floor(y0); py < Math.ceil(y1); ++py) {
        const wy = Math.min(y1, py + 1) - Math.max(y0, py);
        if (wy <= 0) continue;
        for (let px = Math.floor(x0); px < Math.ceil(x1); ++px) {
          const wx = Math.min(x1, px + 1) - Math.max(x0, px);
          if (wx <= 0) continue;
          const from = (Math.min(layer.height - 1, py) * layer.width + Math.min(layer.width - 1, px)) * 4;
          const alpha = layer.rgba[from + 3];
          const w = wx * wy;
          r += layer.rgba[from] * w * alpha;
          g += layer.rgba[from + 1] * w * alpha;
          b += layer.rgba[from + 2] * w * alpha;
          a += alpha * w;
          weight += w;
        }
      }
      const to = (y * size + x) * 4;
      if (weight <= 0 || a <= 0) continue;
      out[to] = r / a;
      out[to + 1] = g / a;
      out[to + 2] = b / a;
      out[to + 3] = a / weight;
    }
  }
  return { width: size, height: size, rgba: out };
}

// --- palette --------------------------------------------------------------

// Grey-scale lightness of an inked pixel, 0 = darkest.
function inkLightness(color) {
  return Math.min(color[0], color[1], color[2]) / 255;
}

// Paints a resampled layer in the variant's ink. The source's own tonal ramp is
// kept (its darkest pixels become the palette ink, still-inked edges stay a
// little lighter), and the green steam becomes the palette accent. Colour only:
// shape and coverage are untouched, which is why the two palettes of one state
// produce identical alpha.
function applyPalette(layer, palette) {
  const inks = INKS[palette];
  const out = Buffer.alloc(layer.width * layer.height * 4);
  for (let i = 0; i < layer.width * layer.height; ++i) {
    const at = i * 4;
    const alpha = layer.rgba[at + 3];
    if (alpha <= 0) continue;
    const color = [layer.rgba[at], layer.rgba[at + 1], layer.rgba[at + 2]];
    const target = isSteam(color) ? inks.steam : inks.stroke;
    const tone = 0.72 + 0.28 * (1 - inkLightness(color));
    out[at] = Math.round(target[0] * tone);
    out[at + 1] = Math.round(target[1] * tone);
    out[at + 2] = Math.round(target[2] * tone);
    out[at + 3] = Math.round(alpha * 255);
  }
  return { width: layer.width, height: layer.height, rgba: out };
}

// --- contrast report -------------------------------------------------------

function luminance(rgb) {
  const channel = (value) => {
    const s = value / 255;
    return s <= 0.03928 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * channel(rgb[0]) + 0.7152 * channel(rgb[1]) + 0.0722 * channel(rgb[2]);
}

function contrast(a, b) {
  const la = luminance(a);
  const lb = luminance(b);
  return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
}

// --- build ----------------------------------------------------------------

const sheet = existsSync(path.join(ROOT, SOURCE_COPY)) ? loadSheet() : null;

// The 15 variants, each rasterised from its own source-pixel layer. `entries`
// holds the seven PNG payloads that go into the .ico.
const built = new Map();
const placements = new Map();
for (const variant of VARIANTS) {
  if (!sheet) break;
  const { canvas, placement } = composeVariantLayer(variant);
  const entries = ICO_SIZES.map((size) => ({ size, png: encodePng(
    size, size, applyPalette(resample(canvas, size), variant.palette).rgba) }));
  built.set(variant.file, { ico: encodeIco(entries), entries, canvas, variant });
  placements.set(variant.file, placement);
}

// Structural self-checks that must hold before anything is written.
function verify() {
  const problems = [];
  const alphaOf = (file, size) => decodePng(built.get(file).entries.find((e) => e.size === size).png).rgba;
  const alphaAt = (rgba, i) => rgba[i * 4 + 3];
  // 1. Both palettes of one state must share one alpha image at every size: the
  //    same cell, the same composition, only colour differs. This is the check
  //    that would have caught the earlier light/dark mismatch.
  for (const light of VARIANTS.filter((v) => v.background === 'light')) {
    const darkName = light.file.replace(/^light-/, 'dark-');
    if (!built.has(darkName)) continue;
    for (const size of ICO_SIZES) {
      const a = alphaOf(light.file, size);
      const b = alphaOf(darkName, size);
      let differ = 0;
      for (let i = 0; i < size * size; ++i) if (alphaAt(a, i) !== alphaAt(b, i)) ++differ;
      if (differ !== 0) problems.push(`${light.file} and ${darkName} differ in alpha at ${size}px (${differ} px)`);
    }
  }
  // 2. Each runtime state must be its own distinct drawing, and the composed
  //    state must actually differ from the plain BOTH cell (otherwise the dashed
  //    screen swap did nothing).
  for (const background of ['light', 'dark']) {
    const plainPaused = VARIANTS.find((v) => v.file === `${background}-screen.ico`);
    const deferred = VARIANTS.find((v) => v.file === `${background}-screen-paused.ico`);
    if (plainPaused.cell === deferred.cell) {
      problems.push(`${background}: accepted (solid screen) and deferred (dashed) use the same cell`);
    }
    for (const size of [16, 32, 256]) {
      const withDuration = VARIANTS.find((v) => v.file === `${background}-screen-paused-duration.ico`);
      const both = VARIANTS.find((v) => v.file === `${background}-screen-duration.ico`);
      const a = alphaOf(withDuration.file, size);
      const b = alphaOf(both.file, size);
      let changed = 0;
      for (let i = 0; i < size * size; ++i) if (Math.abs(alphaAt(a, i) - alphaAt(b, i)) > 8) ++changed;
      if (changed === 0) {
        problems.push(`${withDuration.file}@${size} is identical to ${both.file}: the dashed screen was not swapped in`);
      }
    }
  }
  // 3. The composed state must keep BOTH's right-hand clock: the region right of
  //    the swapped screen has to be unchanged between BOTH and BOTH+dashed.
  for (const background of ['light', 'dark']) {
    const withDuration = VARIANTS.find((v) => v.file === `${background}-screen-paused-duration.ico`);
    const both = VARIANTS.find((v) => v.file === `${background}-screen-duration.ico`);
    const record = placements.get(withDuration.file).moves.find((m) => m.mark === 'DASHED_SCREEN');
    const keepFrom = record.to.x + record.srcSize.w;
    const size = 256;
    const scale = size / CARRIER_WIDTH;
    const a = alphaOf(withDuration.file, size);
    const b = alphaOf(both.file, size);
    let changed = 0;
    for (let y = 0; y < size; ++y) {
      for (let x = Math.ceil(keepFrom * scale); x < size; ++x) {
        const i = y * size + x;
        if (Math.abs(alphaAt(a, i) - alphaAt(b, i)) > 8) ++changed;
      }
    }
    if (changed !== 0) {
      problems.push(`${withDuration.file}@256 changed ${changed} px right of the swapped screen (the clock must stay BOTH's)`);
    }
  }
  // 4. Marks must be visible at 16 px: a variant that carries a mark must differ
  //    from the plain running icon of the same palette.
  for (const variant of VARIANTS) {
    if (variant.cell === 'ON') continue;
    if (variant.cell === 'OFF') continue;
    const plain = VARIANTS.find((v) => v.background === variant.background && v.cell === 'ON');
    const a = alphaOf(variant.file, 16);
    const b = alphaOf(plain.file, 16);
    let changed = 0;
    for (let i = 0; i < 16 * 16; ++i) if (Math.abs(alphaAt(a, i) - alphaAt(b, i)) > 24) ++changed;
    if (changed === 0) problems.push(`${variant.file}@16 is indistinguishable from ${plain.file}`);
  }
  // 5. The .rc must declare exactly these files with these ids.
  const rcPath = path.join(RESOURCES, 'unsleep.rc');
  if (!existsSync(rcPath)) {
    problems.push('missing resources/unsleep.rc');
  } else {
    const text = readFileSync(rcPath, 'utf8');
    for (const variant of VARIANTS) {
      const match = text.match(new RegExp(`^\\s*(\\d+)\\s+ICON\\s+"${variant.file.replace('.', '\\.')}"\\s*$`, 'm'));
      if (!match) problems.push(`${variant.file} is not declared in unsleep.rc`);
      else if (Number(match[1]) !== variant.id) {
        problems.push(`${variant.file} is declared as id ${match[1]} but the table says ${variant.id}`);
      }
    }
    for (const [, , file] of text.matchAll(/^\s*(\d+)\s+ICON\s+"([^"]+)"/gm)) {
      if (!VARIANTS.some((variant) => variant.file === file)) {
        problems.push(`unsleep.rc declares ${file}, which the converter does not produce`);
      }
    }
  }
  // 6. Ink must not be clipped: the carrier is larger than every crop, so no
  //    drawing may reach the left or right icon edge (a cut would read as a bug).
  for (const variant of VARIANTS) {
    const payload = decodePng(built.get(variant.file).entries.find((item) => item.size === 256).png);
    const size = 256;
    const opaque = (x, y) => payload.rgba[(y * size + x) * 4 + 3] > 40;
    let clipped = 0;
    for (let y = 0; y < size; ++y) {
      if (opaque(0, y)) ++clipped;
      if (opaque(size - 1, y)) ++clipped;
    }
    if (clipped > 0) problems.push(`${variant.file}@256: ${clipped} opaque pixel(s) on the left/right edge`);
  }
  return problems;
}

function parseArgs(argv) {
  const options = { check: false, preview: null, sheet: null, report: false, source: false, dump: null };
  for (let i = 0; i < argv.length; ++i) {
    if (argv[i] === '--check') options.check = true;
    else if (argv[i] === '--preview') options.preview = argv[++i] ?? null;
    else if (argv[i] === '--sheet') options.sheet = argv[++i] ?? null;
    else if (argv[i] === '--report') options.report = true;
    else if (argv[i] === '--source') options.source = true;
    else if (argv[i] === '--dump') options.dump = argv[++i] ?? null;
    else if (argv[i] === '--help' || argv[i] === '-h') {
      console.log('usage: node resources/icons/generate-icons-gpt.mjs [--source] [--check] ' +
                  '[--preview <dir>] [--sheet <file.png>] [--report] [--dump <file>]');
      process.exit(0);
    } else {
      console.error(`unknown argument: ${argv[i]}`);
      process.exit(2);
    }
  }
  return options;
}

// `--dump` prints the composed source-pixel canvas of every variant as text, at
// a fixed column width, so the composition can be checked without an image
// viewer: '#' ink, 'G' accent, ' ' empty. Review aid, not part of the build.
function dumpLayer(layer, label) {
  const columns = 100;
  const rows = Math.round((layer.height / layer.width) * columns * 0.5);
  const lines = [`== ${label} ${layer.width}x${layer.height} ==`];
  for (let ry = 0; ry < rows; ++ry) {
    let line = '';
    for (let rx = 0; rx < columns; ++rx) {
      const x = Math.min(layer.width - 1, Math.floor((rx + 0.5) * (layer.width / columns)));
      const y = Math.min(layer.height - 1, Math.floor((ry + 0.5) * (layer.height / rows)));
      const at = (y * layer.width + x) * 4;
      const alpha = layer.rgba[at + 3];
      if (alpha <= 0.15) { line += ' '; continue; }
      line += isSteam([layer.rgba[at], layer.rgba[at + 1], layer.rgba[at + 2]]) ? 'G' : '#';
    }
    lines.push(line);
  }
  return lines.join('\n');
}

function insideProject(target) {
  const resolved = path.resolve(target);
  return resolved === ROOT || resolved.startsWith(ROOT + path.sep);
}

// --- contact sheet ---------------------------------------------------------
//
// Reviewer artefact only. This function lays out the BUILT icons for a human to
// look at; it never draws or alters an icon. Every icon pixel below comes from
// `built`, so what is reviewed is exactly what ships.
//
// Layout rules (fixed after 009 produced rows that overlapped each other):
//   * one ROW per state, so nothing can bleed into another state;
//   * a row is tall enough for its tallest cell plus margins - the 128 px
//     blow-up sets the height, nothing is allowed to collide with it;
//   * the 16/20/24/32 px columns show the REAL icon at its real size, one
//     column per size, each in its own cell;
//   * the blow-up column is the 16 px icon scaled up by an integer factor, not
//     the 32 px icon, so what enlarges is the size that actually has to read at
//     a glance;
//   * total width stays <= 1400 px;
//   * the light and the dark theme go to SEPARATE images, each on its own
//     taskbar colour, so light ink is never judged on a light fill;
//   * the application icon gets its own small section on a light surface, since
//     its ink is dark by design.

const SHEET_STATES = [
  { name: 'stop', file: 'inactive', caption: 'stopped' },
  { name: 'run', file: 'active', caption: 'running' },
  { name: 'screen', file: 'screen', caption: 'screen on' },
  { name: 'timer', file: 'duration', caption: 'timer' },
  { name: 'screen+duration', file: 'screen-duration', caption: 'screen + timer' },
  { name: 'paused', file: 'screen-paused', caption: 'paused' },
  { name: 'paused+duration', file: 'screen-paused-duration', caption: 'paused + timer' },
];

const SHEET_REAL_SIZES = [16, 20, 24, 32];
const SHEET_ZOOM = 8;
const SHEET_ZOOM_SOURCE = 16;

// A 5x7 bitmap font, drawn as filled rectangles. Enough for the labels; keeps
// the sheet dependency-free (no font file, no text API).
const GLYPHS = {
  ' ': ['00000', '00000', '00000', '00000', '00000', '00000', '00000'],
  'A': ['01110', '10001', '10001', '11111', '10001', '10001', '10001'],
  'B': ['11110', '10001', '10001', '11110', '10001', '10001', '11110'],
  'C': ['01110', '10001', '10000', '10000', '10000', '10001', '01110'],
  'D': ['11110', '10001', '10001', '10001', '10001', '10001', '11110'],
  'E': ['11111', '10000', '10000', '11110', '10000', '10000', '11111'],
  'F': ['11111', '10000', '10000', '11110', '10000', '10000', '10000'],
  'G': ['01110', '10001', '10000', '10111', '10001', '10001', '01111'],
  'H': ['10001', '10001', '10001', '11111', '10001', '10001', '10001'],
  'I': ['11111', '00100', '00100', '00100', '00100', '00100', '11111'],
  'J': ['00111', '00010', '00010', '00010', '00010', '10010', '01100'],
  'K': ['10001', '10010', '10100', '11000', '10100', '10010', '10001'],
  'L': ['10000', '10000', '10000', '10000', '10000', '10000', '11111'],
  'M': ['10001', '11011', '10101', '10101', '10001', '10001', '10001'],
  'N': ['10001', '11001', '10101', '10011', '10001', '10001', '10001'],
  'O': ['01110', '10001', '10001', '10001', '10001', '10001', '01110'],
  'P': ['11110', '10001', '10001', '11110', '10000', '10000', '10000'],
  'Q': ['01110', '10001', '10001', '10001', '10101', '10010', '01101'],
  'R': ['11110', '10001', '10001', '11110', '10100', '10010', '10001'],
  'S': ['01111', '10000', '10000', '01110', '00001', '00001', '11110'],
  'T': ['11111', '00100', '00100', '00100', '00100', '00100', '00100'],
  'U': ['10001', '10001', '10001', '10001', '10001', '10001', '01110'],
  'V': ['10001', '10001', '10001', '10001', '10001', '01010', '00100'],
  'W': ['10001', '10001', '10001', '10101', '10101', '11011', '10001'],
  'X': ['10001', '10001', '01010', '00100', '01010', '10001', '10001'],
  'Y': ['10001', '10001', '01010', '00100', '00100', '00100', '00100'],
  'Z': ['11111', '00001', '00010', '00100', '01000', '10000', '11111'],
  '0': ['01110', '10001', '10011', '10101', '11001', '10001', '01110'],
  '1': ['00100', '01100', '00100', '00100', '00100', '00100', '01110'],
  '2': ['01110', '10001', '00001', '00010', '00100', '01000', '11111'],
  '3': ['11111', '00010', '00100', '00010', '00001', '10001', '01110'],
  '4': ['00010', '00110', '01010', '10010', '11111', '00010', '00010'],
  '5': ['11111', '10000', '11110', '00001', '00001', '10001', '01110'],
  '6': ['00110', '01000', '10000', '11110', '10001', '10001', '01110'],
  '7': ['11111', '00001', '00010', '00100', '01000', '01000', '01000'],
  '8': ['01110', '10001', '10001', '01110', '10001', '10001', '01110'],
  '9': ['01110', '10001', '10001', '01111', '00001', '00010', '01100'],
  '+': ['00000', '00100', '00100', '11111', '00100', '00100', '00000'],
  '-': ['00000', '00000', '00000', '11111', '00000', '00000', '00000'],
  '.': ['00000', '00000', '00000', '00000', '00000', '01100', '01100'],
  '/': ['00001', '00010', '00010', '00100', '01000', '01000', '10000'],
  '(': ['00010', '00100', '01000', '01000', '01000', '00100', '00010'],
  ')': ['01000', '00100', '00010', '00010', '00010', '00100', '01000'],
  ':': ['00000', '01100', '01100', '00000', '01100', '01100', '00000'],
};

function textWidth(text, scale) {
  return text.length * 6 * scale - scale;
}

function drawText(canvas, text, x0, y0, scale, rgb) {
  let cursor = x0;
  for (const ch of text.toUpperCase()) {
    const glyph = GLYPHS[ch] ?? GLYPHS[' '];
    for (let row = 0; row < 7; ++row) {
      for (let col = 0; col < 5; ++col) {
        if (glyph[row][col] !== '1') continue;
        for (let dy = 0; dy < scale; ++dy) {
          for (let dx = 0; dx < scale; ++dx) {
            const px = cursor + col * scale + dx;
            const py = y0 + row * scale + dy;
            if (px < 0 || py < 0 || px >= canvas.width || py >= canvas.height) continue;
            const at = (py * canvas.width + px) * 4;
            canvas.rgba[at] = rgb[0];
            canvas.rgba[at + 1] = rgb[1];
            canvas.rgba[at + 2] = rgb[2];
            canvas.rgba[at + 3] = 255;
          }
        }
      }
    }
    cursor += 6 * scale;
  }
}

function fillRect(canvas, x0, y0, w, h, rgb) {
  for (let y = y0; y < y0 + h; ++y) {
    for (let x = x0; x < x0 + w; ++x) {
      if (x < 0 || y < 0 || x >= canvas.width || y >= canvas.height) continue;
      const at = (y * canvas.width + x) * 4;
      canvas.rgba[at] = rgb[0];
      canvas.rgba[at + 1] = rgb[1];
      canvas.rgba[at + 2] = rgb[2];
      canvas.rgba[at + 3] = 255;
    }
  }
}

// Blits an icon payload at (ox,oy), optionally magnified by an integer factor
// with nearest-neighbour sampling (so a blow-up shows the real pixels).
function blitIcon(canvas, icon, ox, oy, zoom) {
  for (let y = 0; y < icon.height * zoom; ++y) {
    for (let x = 0; x < icon.width * zoom; ++x) {
      const from = (Math.floor(y / zoom) * icon.width + Math.floor(x / zoom)) * 4;
      const alpha = icon.rgba[from + 3];
      if (alpha === 0) continue;
      const a = alpha / 255;
      const at = ((oy + y) * canvas.width + ox + x) * 4;
      if (ox + x < 0 || oy + y < 0 || ox + x >= canvas.width || oy + y >= canvas.height) continue;
      for (let c = 0; c < 3; ++c) {
        canvas.rgba[at + c] = Math.round(icon.rgba[from + c] * a + canvas.rgba[at + c] * (1 - a));
      }
      canvas.rgba[at + 3] = Math.max(canvas.rgba[at + 3], alpha);
    }
  }
}

function iconOf(variant, size) {
  return decodePng(built.get(variant.file).entries.find((item) => item.size === size).png);
}

// One theme's contact sheet. `background` picks which palette's files to show and
// therefore which taskbar colour to paint.
function composeThemeSheet(background) {
  const surface = SURFACES[background];
  const text = background === 'dark' ? [0xf0, 0xf0, 0xf0] : [0x18, 0x18, 0x18];
  const zoomPx = SHEET_ZOOM_SOURCE * SHEET_ZOOM;

  const MARGIN = 16;
  const GAP = 16;
  const LABEL_W = 190;          // left column: state name
  const realCell = 48;          // cell for the real-size icons (32 px + padding)
  const zoomCell = zoomPx + MARGIN;
  const HEADER_H = 46;
  const ROW_H = Math.max(zoomPx + MARGIN * 2, realCell + MARGIN * 2, 160);

  const columnsW = SHEET_REAL_SIZES.length * (realCell + GAP) + zoomCell;
  const width = MARGIN * 2 + LABEL_W + GAP + columnsW;
  const rows = SHEET_STATES.length;
  const height = HEADER_H + MARGIN * 2 + rows * ROW_H;
  if (width > 1400) throw new Error(`contact sheet ${width}px exceeds the 1400px limit`);

  const canvas = { width, height, rgba: Buffer.alloc(width * height * 4) };
  for (let i = 0; i < width * height; ++i) canvas.rgba[i * 4 + 3] = 255;
  fillRect(canvas, 0, 0, width, height, surface);

  // Header: title plus one column caption per real size.
  drawText(canvas, `UNSLEEP ${background} taskbar (${background === 'light' ? 'dark ink' : 'light ink'})`,
           MARGIN, MARGIN, 2, text);
  let cx = MARGIN + LABEL_W + GAP;
  for (const size of SHEET_REAL_SIZES) {
    const label = String(size);
    drawText(canvas, label, cx + Math.floor((realCell - textWidth(label, 1)) / 2), MARGIN + 20, 1, text);
    cx += realCell + GAP;
  }
  drawText(canvas, '16 X8', cx + Math.floor((zoomCell - textWidth('16 X8', 1)) / 2), MARGIN + 20, 1, text);

  for (let index = 0; index < rows; ++index) {
    const state = SHEET_STATES[index];
    const variant = VARIANTS.find((v) => v.file === `${background}-${state.file}.ico`);
    if (!variant) throw new Error(`no resource for ${background}/${state.name}`);
    const rowTop = HEADER_H + index * ROW_H;
    drawText(canvas, state.caption, MARGIN, rowTop + Math.floor(ROW_H / 2) - 4, 1, text);

    let left = MARGIN + LABEL_W + GAP;
    for (const size of SHEET_REAL_SIZES) {
      const icon = iconOf(variant, size);
      const ox = left + Math.floor((realCell - size) / 2);
      const oy = rowTop + Math.floor((ROW_H - size) / 2);
      blitIcon(canvas, icon, ox, oy, 1);
      left += realCell + GAP;
    }
    // Blow-up column: the real 16 px icon, magnified by a whole factor.
    const zoomed = iconOf(variant, SHEET_ZOOM_SOURCE);
    blitIcon(canvas, zoomed, left, rowTop + Math.floor((ROW_H - zoomPx) / 2), SHEET_ZOOM);
  }
  return { png: encodePng(width, height, canvas.rgba), width, height };
}

// The application icon, on its own light demo surface: it is dark ink, so it must
// not be shown on a dark fill.
function composeAppSheet() {
  const surface = SURFACES.light;
  const text = [0x18, 0x18, 0x18];
  const MARGIN = 16;
  const GAP = 24;
  const cell = 256 + MARGIN;
  const sizes = [16, 20, 24, 32, 48, 64, 256];
  const width = Math.min(1400, MARGIN * 2 + sizes.length * (cell + GAP));
  const height = MARGIN * 3 + 24 + 256;
  const canvas = { width, height, rgba: Buffer.alloc(width * height * 4) };
  fillRect(canvas, 0, 0, width, height, surface);
  const app = VARIANTS.find((variant) => variant.id === 128);
  drawText(canvas, 'APP ICON (DARK INK ON LIGHT SURFACE) - EVERY SHIPPED SIZE', MARGIN, MARGIN, 2, text);
  let left = MARGIN;
  for (const size of sizes) {
    const icon = iconOf(app, size);
    const ox = left + Math.floor((cell - size) / 2);
    const oy = MARGIN * 2 + 24 + Math.floor((256 - size) / 2);
    blitIcon(canvas, icon, ox, oy, 1);
    drawText(canvas, String(size), left + Math.floor((cell - textWidth(String(size), 1)) / 2), height - MARGIN - 7, 1, text);
    left += cell + GAP;
  }
  return { png: encodePng(width, height, canvas.rgba), width, height };
}

// Measured geometry of the two screen rectangles, resolved into carrier
// coordinates. Used by the report so the swap can be verified against the sheet.
const SCREEN_GEOMETRY = {
  bothSolid: sheet ? { ...toCarrier('BOTH', SCREENS.BOTH_SOLID.x0, SCREENS.BOTH_SOLID.y0),
                       w: SCREENS.BOTH_SOLID.x1 - SCREENS.BOTH_SOLID.x0 + 1,
                       h: SCREENS.BOTH_SOLID.y1 - SCREENS.BOTH_SOLID.y0 + 1 } : null,
  pausedDashed: sheet ? { ...toCarrier('BOTH', SCREENS.PAUSED_DASHED.x0, SCREENS.PAUSED_DASHED.y0),
                          w: SCREENS.PAUSED_DASHED.x1 - SCREENS.PAUSED_DASHED.x0 + 1,
                          h: SCREENS.PAUSED_DASHED.y1 - SCREENS.PAUSED_DASHED.y0 + 1 } : null,
};

// --- main -----------------------------------------------------------------

function main() {
  const options = parseArgs(process.argv.slice(2));

  if (options.source) {
    const target = path.join(ROOT, SOURCE_COPY);
    mkdirSync(path.dirname(target), { recursive: true });
    const bytes = readFileSync(SOURCE_ORIGIN);
    const hash = sha256(bytes);
    if (hash !== SOURCE_SHA256) {
      console.error(`source ${SOURCE_ORIGIN} has SHA-256 ${hash}, expected ${SOURCE_SHA256}`);
      process.exit(1);
    }
    writeFileSync(target, bytes);
    console.log(`source|${SOURCE_COPY}|${bytes.length}|${hash}|installed`);
    return;
  }

  const problems = verify();
  const reviewOnly = options.report || options.sheet || options.dump || options.preview;
  if (problems.length > 0) {
    for (const problem of problems) console.error(`self-check: ${problem}`);
    if (!reviewOnly) process.exit(1);
    console.error(`self-check: ${problems.length} problem(s) - review output only, nothing written to resources/`);
  }

  if (options.dump) {
    const lines = [];
    for (const variant of VARIANTS) {
      const placement = placements.get(variant.file);
      const moves = placement.moves.map((m) => `${m.mark}:${m.from}->(${m.to.x},${m.to.y})`).join(' ');
      lines.push(dumpLayer(built.get(variant.file).canvas,
                           `${variant.file} id=${variant.id} cell=${placement.cell} ${moves}`));
    }
    if (!insideProject(options.dump)) {
      console.error(`dump must stay inside the project: ${options.dump}`);
      process.exit(2);
    }
    mkdirSync(path.dirname(path.resolve(options.dump)), { recursive: true });
    writeFileSync(options.dump, lines.join('\n\n'));
    console.log(`dump|${path.resolve(options.dump)}|${lines.length} variants`);
    return;
  }

  if (options.report) {
    console.log(`source|${SOURCE_COPY}|${SOURCE_SHA256}|${sheet.width}x${sheet.height}`);
    console.log(`carrier|${CARRIER_WIDTH}x${CARRIER_HEIGHT}|cells_y=${CELL_CANVAS_Y}..${CELL_CANVAS_Y + CELL_CANVAS_H - 1}|left=${CARRIER_LEFT}`);
    console.log(`screen_geometry|both_solid=${JSON.stringify(SCREEN_GEOMETRY.bothSolid)}|paused_dashed=${JSON.stringify(SCREEN_GEOMETRY.pausedDashed)}`);
    for (const variant of VARIANTS) {
      const inks = INKS[variant.palette];
      const surface = SURFACES[variant.background];
      const moves = placements.get(variant.file).moves
        .map((m) => `${m.mark}@${m.to.x},${m.to.y}(${m.srcSize.w}x${m.srcSize.h},x${m.scale})`).join('+');
      console.log(`${variant.file}|id=${variant.id}|cell=${variant.cell}|moves=${moves || 'none'}|` +
                  `background=${variant.background}|palette=${variant.palette}|` +
                  `stroke_contrast=${contrast(inks.stroke, surface).toFixed(2)}|` +
                  `steam_contrast=${contrast(inks.steam, surface).toFixed(2)}`);
    }
    return;
  }

  const rows = [];
  let mismatches = 0;
  for (const variant of VARIANTS) {
    const { ico } = built.get(variant.file);
    const target = path.join(RESOURCES, variant.file);
    const existing = existsSync(target) ? readFileSync(target) : null;
    const matches = existing !== null && existing.equals(ico);
    if (!options.check && !matches) writeFileSync(target, ico);
    else if (options.check && !matches) ++mismatches;
    rows.push(`${variant.file}|${variant.id}|${ico.length}|${sha256(ico)}|` +
              `${existing === null ? 'missing' : sha256(existing)}|` +
              `${matches ? 'match' : options.check ? 'DIFF' : 'written'}`);
  }

  if (options.preview) {
    if (!insideProject(options.preview)) {
      console.error(`preview directory must stay inside the project: ${options.preview}`);
      process.exit(2);
    }
    mkdirSync(options.preview, { recursive: true });
    // Each preview is the exact PNG payload inside the matching .ico, at its
    // real size. `--preview` also writes a 128 px and a 256 px nearest-neighbour
    // blow-up per STATE, so a reviewer can look at one state at a time without
    // reading the contact sheet.
    for (const variant of VARIANTS) {
      for (const entry of built.get(variant.file).entries) {
        const out = path.join(options.preview, `${path.parse(variant.file).name}-${entry.size}.png`);
        writeFileSync(out, entry.png);
      }
    }
    for (const background of ['light', 'dark']) {
      for (const state of SHEET_STATES) {
        const variant = VARIANTS.find((v) => v.file === `${background}-${state.file}.ico`);
        if (!variant) continue;
        const base = `${background}-${state.file}`;
        // 128 px: the 16 px icon x8 (what has to be legible at a glance).
        const small = iconOf(variant, SHEET_ZOOM_SOURCE);
        const big128 = { width: 128, height: 128, rgba: Buffer.alloc(128 * 128 * 4) };
        blitIcon(big128, small, 0, 0, 8);
        writeFileSync(path.join(options.preview, `${base}-zoom16x8-128.png`), encodePng(128, 128, big128.rgba));
        // 256: the real 256 px payload, exactly as shipped.
        const real256 = iconOf(variant, 256);
        writeFileSync(path.join(options.preview, `${base}-real-256.png`),
                      built.get(variant.file).entries.find((e) => e.size === 256).png);
        void real256;
      }
    }
  }

  if (options.sheet) {
    if (!insideProject(options.sheet)) {
      console.error(`contact sheet must stay inside the project: ${options.sheet}`);
      process.exit(2);
    }
    // `--sheet <dir>` writes three PNGs: the light sheet, the dark sheet and the
    // app-icon sheet. Passing a .png path keeps the old single-file behaviour
    // (light theme only) for compatibility.
    const target = path.resolve(options.sheet);
    const isDir = !/\.png$/i.test(target);
    if (isDir) mkdirSync(target, { recursive: true });
    else mkdirSync(path.dirname(target), { recursive: true });
    const write = (name, sheetImage) => {
      const file = isDir ? path.join(target, name) : target;
      writeFileSync(file, sheetImage.png);
      console.log(`sheet|${file}|${sheetImage.width}x${sheetImage.height}|${sheetImage.png.length}|${sha256(sheetImage.png)}`);
    };
    if (isDir) {
      write('contact-sheet-light.png', composeThemeSheet('light'));
      write('contact-sheet-dark.png', composeThemeSheet('dark'));
      write('contact-sheet-app.png', composeAppSheet());
    } else {
      write(path.basename(target), composeThemeSheet('light'));
    }
  }

  for (const row of rows) console.log(row);
  if (options.check && mismatches > 0) {
    console.error(`check failed: ${mismatches} file(s) differ`);
    process.exit(1);
  }
}

main();
