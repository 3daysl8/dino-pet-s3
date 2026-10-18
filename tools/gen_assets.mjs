// Generate LVGL image assets (dino sprites + pixel icons) from the approved mockup.
// The mockup's own generator code is the source of truth, so the board shows exactly
// what was signed off. Output: 4-bit indexed images (≈ 0.9 KB per 40x40 sprite),
// drawn on the device with lv_img zoom and antialiasing off.
//
//   node tools/gen_assets.mjs
//
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const html = readFileSync(join(root, "docs/mockup/dino-mockup-v3.html"), "utf8").replace(/\r\n/g, "\n");
const start = html.indexOf("  // ------------------------------------------------------------ palettes");
const end = html.indexOf("  // ------------------------------------------------------------ screen\n  const canvas");
if (start < 0 || end < 0) throw new Error("generator markers not found in mockup");
const src = html.slice(start, end);
const gen = new Function(`${src}\nreturn { makeDino, SKINS, FIXED, ICONS, ICON_PAL, G };`)();

const SKIN = "red";                       // default skin
const GHOST = { O: "#d9dbe3", o: "#a8abb6", C: "#eef0f4", S: "#c3c5cf", s: "#9a9ca7", B: "#d9dbe3" };

// ---------------------------------------------------------------- encoding
function hex(c) { const n = parseInt(c.slice(1), 16); return [(n >> 16) & 255, (n >> 8) & 255, n & 255]; }

// rows: array of arrays of palette keys (null = transparent); pal: key -> "#rrggbb"
function indexed4(name, rows, pal) {
  const h = rows.length, w = rows[0].length;
  const keys = [...new Set(rows.flat().filter(Boolean))];
  if (keys.length > 15) throw new Error(`${name}: ${keys.length} colours > 15`);
  const idx = new Map(keys.map((k, i) => [k, i + 1]));          // 0 = transparent
  const bytes = [];
  // palette: 16 x lv_color32_t (B, G, R, A)
  for (let i = 0; i < 16; i++) {
    if (i === 0 || i > keys.length) { bytes.push(0, 0, 0, 0); continue; }
    const [r, g, b] = hex(pal[keys[i - 1]]);
    bytes.push(b, g, r, 255);
  }
  const stride = Math.ceil(w / 2);
  for (let y = 0; y < h; y++) {
    for (let bx = 0; bx < stride; bx++) {
      const a = rows[y][bx * 2], b = rows[y][bx * 2 + 1];
      const hi = a ? idx.get(a) : 0, lo = b ? idx.get(b) : 0;
      bytes.push((hi << 4) | lo);
    }
  }
  return { name, w, h, bytes };
}

function emit(img) {
  const lines = [];
  for (let i = 0; i < img.bytes.length; i += 24)
    lines.push("    " + img.bytes.slice(i, i + 24).map(b => "0x" + b.toString(16).padStart(2, "0")).join(","));
  return `static const uint8_t ${img.name}_map[] = {\n${lines.join(",\n")}\n};\n` +
    `const lv_img_dsc_t ${img.name} = {\n` +
    `    .header.cf = LV_IMG_CF_INDEXED_4BIT, .header.always_zero = 0, .header.reserved = 0,\n` +
    `    .header.w = ${img.w}, .header.h = ${img.h},\n` +
    `    .data_size = sizeof(${img.name}_map), .data = ${img.name}_map,\n};\n`;
}

// ---------------------------------------------------------------- sprites
const G = gen.G;
const toRows = g => Array.from({ length: G }, (_, y) => g.slice(y * G, y * G + G));
const STAGES = ["baby", "child", "teen"];
const MOODS = ["happy", "ok", "hungry", "sick", "asleep"];
const FORMS = ["star", "buddy", "grump"];
const sprites = [];
const pal = { ...gen.FIXED, ...gen.SKINS[SKIN] };

// Egg + three crack levels (drawn here; the mockup only has the plain egg).
const CRACKS = [
  [],
  [[17, 13], [18, 14], [19, 13], [20, 14], [21, 13], [22, 14]],
  [[13, 18], [14, 19], [15, 18], [16, 19], [17, 18], [18, 19], [19, 18], [20, 19], [21, 18], [22, 19], [23, 18], [24, 19], [25, 18],
   [17, 13], [18, 14], [19, 13], [20, 14], [21, 13], [22, 14], [19, 15], [19, 16], [19, 17]],
  [[12, 21], [13, 22], [14, 21], [15, 22], [16, 21], [17, 22], [18, 21], [19, 22], [20, 21], [21, 22], [22, 21], [23, 22], [24, 21], [25, 22], [26, 21], [27, 22],
   [13, 18], [14, 19], [15, 18], [16, 19], [17, 18], [18, 19], [19, 18], [20, 19], [21, 18], [22, 19], [23, 18], [24, 19], [25, 18],
   [17, 13], [18, 14], [19, 13], [20, 14], [21, 13], [22, 14], [19, 15], [19, 16], [19, 17], [16, 20], [23, 20]],
];
CRACKS.forEach((pts, level) => {
  const g = gen.makeDino("egg", "ok", "star", false);
  for (const [x, y] of pts) if (g[y * G + x]) g[y * G + x] = "K";
  sprites.push(indexed4(level ? `spr_egg_crack${level}` : "spr_egg", toRows(g), pal));
});
for (const st of STAGES) {
  for (const m of MOODS) sprites.push(indexed4(`spr_${st}_${m}`, toRows(gen.makeDino(st, m, "star", false)), pal));
  sprites.push(indexed4(`spr_${st}_blink`, toRows(gen.makeDino(st, "ok", "star", true)), pal));
}
for (const f of FORMS) {
  for (const m of MOODS) sprites.push(indexed4(`spr_adult_${f}_${m}`, toRows(gen.makeDino("adult", m, f, false)), pal));
  sprites.push(indexed4(`spr_adult_${f}_blink`, toRows(gen.makeDino("adult", "ok", f, true)), pal));
}
sprites.push(indexed4("spr_ghost", toRows(gen.makeDino("ghost", "dead", "star", false)), { ...gen.FIXED, ...GHOST }));

// ---------------------------------------------------------------- icons
const icons = Object.entries(gen.ICONS).map(([name, rows]) => {
  const w = Math.max(...rows.map(r => r.length));
  const grid = rows.map(r => [...r.padEnd(w, ".")].map(k => (k === "." ? null : k)));
  return indexed4(`icon_${name}`, grid, gen.ICON_PAL);
});

// ---------------------------------------------------------------- write
const all = [...sprites, ...icons];
const header = `// GENERATED by tools/gen_assets.mjs from docs/mockup/dino-mockup-v3.html. Do not edit.
#pragma once
#include "lvgl.h"

typedef enum { DINO_HAPPY = 0, DINO_OK, DINO_HUNGRY, DINO_SICK, DINO_ASLEEP, DINO_BLINK } dino_face_t;

// stage: 0 egg, 1 baby, 2 child, 3 teen, 4 adult, 5 ghost (matches pet_stage_t)
// form (adult only): 1 star, 2 buddy, 3 grump (matches pet_form_t)
const lv_img_dsc_t *dino_sprite(int stage, int form, dino_face_t face);
const lv_img_dsc_t *egg_sprite(int crack);   // crack 0..3

${all.map(i => `extern const lv_img_dsc_t ${i.name};`).join("\n")}
`;
const faceKeys = [...MOODS, "blink"];
const table = (prefix) => `{ ${faceKeys.map(m => `&${prefix}_${m}`).join(", ")} }`;
const body = `// GENERATED by tools/gen_assets.mjs from docs/mockup/dino-mockup-v3.html. Do not edit.
#include "dino_assets.h"

${all.map(emit).join("\n")}
static const lv_img_dsc_t *const s_young[3][6] = {
    ${STAGES.map(s => table(`spr_${s}`)).join(",\n    ")}
};
static const lv_img_dsc_t *const s_adult[3][6] = {
    ${FORMS.map(f => table(`spr_adult_${f}`)).join(",\n    ")}
};

const lv_img_dsc_t *egg_sprite(int crack)
{
    static const lv_img_dsc_t *const e[4] = {&spr_egg, &spr_egg_crack1, &spr_egg_crack2, &spr_egg_crack3};
    return e[crack < 0 ? 0 : crack > 3 ? 3 : crack];
}

const lv_img_dsc_t *dino_sprite(int stage, int form, dino_face_t face)
{
    if (face > DINO_BLINK) face = DINO_OK;
    switch (stage) {
    case 0: return &spr_egg;
    case 1: case 2: case 3: return s_young[stage - 1][face];
    case 4: return s_adult[(form >= 1 && form <= 3) ? form - 1 : 0][face];
    default: return &spr_ghost;
    }
}
`;
const out = join(root, "firmware/main/app/assets");
writeFileSync(join(out, "dino_assets.h"), header);
writeFileSync(join(out, "dino_assets.c"), body);
const total = all.reduce((n, i) => n + i.bytes.length, 0);
console.log(`${sprites.length} sprites + ${icons.length} icons, ${(total / 1024).toFixed(1)} KB`);
