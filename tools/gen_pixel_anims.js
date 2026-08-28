#!/usr/bin/env node
/*
 * gen_pixel_anims.js — converte os presets de pixel art do ClaudePix
 * (https://claudepix.vercel.app) em firmware/claude_stick/pixel_anims.h.
 *
 * Os presets sao paginas HTML autocontidas: cada uma define um PRESET com
 * { name, frames: [{ hold, frame }] }, onde cada frame e uma grade de 20x20
 * com um indice de paleta por celula. Os que usam o motor compartilhado
 * (animations/creature-engine.js) tem 3 valores — vazio, corpo, olho; os
 * autocontidos trazem a propria paleta, de ate 16 cores.
 *
 * Este script roda cada pagina num contexto de VM com um DOM minimo, le o
 * PRESET ja com todos os quadros calculados, deduplica as grades e emite C.
 *
 * Por que dar esse trabalho, em vez de baixar o MP4 que o site oferece: o
 * ESP32 nao decodifica video, e nao tem RAM para quadros bitmap. Grade de
 * 20x20 a 4 bits sao 200 bytes por quadro, em flash. Na placa, uma unica
 * lv_canvas de 20x20 em RGB565 (800 bytes de RAM) e ampliada por
 * lv_image_set_scale — o mesmo caminho que os sprites do Clawd ja usam.
 *
 * Uso:
 *     node tools/gen_pixel_anims.js <pasta com os .html> [creature-engine.js]
 *
 * A pasta deve conter os presets baixados de claudepix.vercel.app/animations/.
 */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

// Cores dos presets de 3 valores. Os autocontidos trazem a propria paleta.
// O corpo usa o coral do firmware (C_ACCENT) para o bicho combinar com o
// resto da interface; o olho usa o fundo da tela (C_BG).
const BODY = 0xE8865F;
const EYE = 0x14141C;

const DIR = process.argv[2];
const ENGINE = process.argv[3] || path.join(DIR || '.', 'creature-engine.js');
const OUT = path.join(__dirname, '..', 'firmware', 'claude_stick', 'pixel_anims.h');

if (!DIR || !fs.existsSync(DIR)) {
  console.error('uso: node tools/gen_pixel_anims.js <pasta com os .html> [creature-engine.js]');
  process.exit(1);
}

// DOM minimo: os presets autocontidos montam a grade com createElement e
// appendChild antes de definir os quadros. Nada disso e usado aqui, mas
// precisa existir para o script rodar ate o fim.
function fakeDom() {
  const node = {
    style: {}, children: [],
    appendChild(c) { this.children.push(c); return c; },
    setAttribute() {}, addEventListener() {},
  };
  return {
    getElementById: () => Object.assign(Object.create(node), { style: {}, children: [] }),
    createElement: () => Object.assign(Object.create(node), { style: {}, children: [] }),
    querySelector: () => null,
    addEventListener() {},
  };
}

function loadPreset(file, engineSrc) {
  const ctx = {
    console: { log() {}, warn() {}, error() {} },
    document: fakeDom(),
    requestAnimationFrame: () => 0,
    setTimeout: () => 0, setInterval: () => 0,
    clearTimeout() {}, clearInterval() {},
    performance: { now: () => 0 },
    Date: Date, Math: Math, JSON: JSON,
  };
  ctx.window = ctx;
  vm.createContext(ctx);

  const html = fs.readFileSync(file, 'utf8');
  const usesEngine = /creature-engine\.js/.test(html);
  if (usesEngine) vm.runInContext(engineSrc, ctx);

  const body = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)]
    .map((m) => m[1])
    .join('\n')
    // a chamada de montagem depende de layout real; o PRESET ja esta pronto
    .replace(/window\.PixelEngine\.mount\([\s\S]*?\);/g, '');

  // const/let de topo ficam no escopo do SCRIPT, nao no do contexto: os
  // presets so exportam window.PRESET, entao a paleta e a grade base existem
  // durante a execucao e somem depois. Este trailer roda no mesmo escopo e
  // alcanca as duas.
  const trailer = [
    ';(function(){',
    '  try { window.__PAL = (typeof PAL !== "undefined") ? PAL : null; } catch(e) {}',
    '  try { window.__BASE = (typeof CREATURE !== "undefined") ? CREATURE',
    '      : (typeof BASE !== "undefined") ? BASE',
    '      : (typeof SCENE !== "undefined") ? SCENE : null; } catch(e) {}',
    '})();',
  ].join('\n');

  vm.runInContext(body + '\n' + trailer, ctx);

  // Duas convencoes: os presets do motor exportam window.PRESET.frames; os
  // autocontidos exportam window.FRAMES direto. Mesmo formato de passo nos
  // dois casos: { hold, frame }.
  const preset = ctx.PRESET
    || (Array.isArray(ctx.FRAMES) ? { name: path.basename(file, '.html'), frames: ctx.FRAMES } : null);
  if (!preset || !Array.isArray(preset.frames)) throw new Error('sem PRESET nem FRAMES');

  // paleta: a do proprio preset, ou a de 3 valores do motor
  let pal;
  const rawPal = Array.isArray(ctx.__PAL) ? ctx.__PAL : (Array.isArray(ctx.PAL) ? ctx.PAL : null);
  if (rawPal) {
    pal = rawPal.map((c) => (c === "transparent" ? null : hex(c)));
  } else {
    pal = [null, BODY, EYE];
  }

  // frame: null significa "a grade base"
  const base = ctx.__BASE || (ctx.PixelEngine && ctx.PixelEngine.CREATURE) || null;
  const frames = preset.frames.map((f) => f.frame || base);
  if (frames.some((f) => !f)) throw new Error('quadro nulo sem grade base');

  return { name: path.basename(file, '.html'), pal, frames, holds: preset.frames.map((f) => f.hold) };
}

function hex(css) {
  const m = /^#?([0-9a-f]{6})$/i.exec(String(css).trim());
  if (!m) return 0x000000;
  return parseInt(m[1], 16);
}

function rgb565(v) {
  const r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
  return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}

// 20x20 celulas de 4 bits = 200 bytes, dois pixels por byte (alto = par)
function pack(grid) {
  const out = [];
  for (let r = 0; r < 20; r++) {
    for (let c = 0; c < 20; c += 2) {
      const a = (grid[r][c] || 0) & 0x0f;
      const b = (grid[r][c + 1] || 0) & 0x0f;
      out.push((a << 4) | b);
    }
  }
  return out;
}

const files = fs.readdirSync(DIR).filter((f) => f.endsWith('.html')).sort();
const engineSrc = fs.existsSync(ENGINE) ? fs.readFileSync(ENGINE, 'utf8') : '';

const anims = [];
for (const f of files) {
  try {
    const p = loadPreset(path.join(DIR, f), engineSrc);
    if (p.pal.length > 16) throw new Error('paleta de ' + p.pal.length + ' cores (max 16)');

    // deduplica: os presets repetem muito a mesma grade entre passos
    const seen = new Map();
    const grids = [];
    const steps = p.frames.map((g, i) => {
      const key = JSON.stringify(g);
      if (!seen.has(key)) { seen.set(key, grids.length); grids.push(pack(g)); }
      return { hold: Math.min(p.holds[i], 65535), grid: seen.get(key) };
    });
    anims.push({ ...p, grids, steps });
    console.log('  %s  %d passos, %d grades, paleta de %d',
                p.name.padEnd(20), steps.length, grids.length, p.pal.length);
  } catch (e) {
    console.log('  %s  IGNORADO: %s', f.padEnd(20), e.message);
  }
}

if (!anims.length) { console.error('nenhum preset extraido'); process.exit(1); }

const id = (n) => 'PA_' + n.toUpperCase().replace(/[^A-Z0-9]+/g, '_');
const L = [];
L.push('// Gerado por tools/gen_pixel_anims.js — NAO editar a mao.');
L.push('//');
L.push('// Animacoes de pixel art de 20x20 vindas do ClaudePix');
L.push('// (https://claudepix.vercel.app), convertidas para grades empacotadas em');
L.push('// flash. Cada celula sao 4 bits de indice de paleta, dois pixels por byte:');
L.push('// 200 bytes por grade. Os passos repetem grades, entao cada preset guarda');
L.push('// as grades unicas uma vez e a sequencia so aponta para elas.');
L.push('#pragma once');
L.push('#include <stdint.h>');
L.push('#include <pgmspace.h>');
L.push('');
L.push('#define PA_W 20');
L.push('#define PA_H 20');
L.push('#define PA_GRID_BYTES ((PA_W * PA_H) / 2)');
L.push('');
L.push('struct pa_step_t { uint16_t hold_ms; uint8_t grid; };');
L.push('struct pa_anim_t {');
L.push('  const char *name;');
L.push('  const uint16_t *pal;      // RGB565; indice 0 = transparente');
L.push('  uint8_t palN;');
L.push('  const uint8_t *grids;     // gridN * PA_GRID_BYTES');
L.push('  uint8_t gridN;');
L.push('  const pa_step_t *steps;');
L.push('  uint16_t stepN;');
L.push('};');
L.push('');

let totalBytes = 0;
anims.forEach((a, ai) => {
  const n = a.name.replace(/[^A-Za-z0-9]+/g, '_');
  L.push('// ' + a.name + ': ' + a.steps.length + ' passos, ' + a.grids.length + ' grades');
  L.push('static const uint16_t pa_pal_' + n + '[] PROGMEM = {');
  L.push('  ' + a.pal.map((c) => (c === null ? '0x0000' : '0x' + rgb565(c).toString(16).padStart(4, '0'))).join(', '));
  L.push('};');
  L.push('static const uint8_t pa_grids_' + n + '[] PROGMEM = {');
  a.grids.forEach((g, gi) => {
    for (let i = 0; i < g.length; i += 20) {
      L.push('  ' + g.slice(i, i + 20).map((b) => '0x' + b.toString(16).padStart(2, '0')).join(',') + ',');
    }
    if (gi < a.grids.length - 1) L.push('');
  });
  L.push('};');
  L.push('static const pa_step_t pa_steps_' + n + '[] PROGMEM = {');
  L.push('  ' + a.steps.map((s) => '{' + s.hold + ',' + s.grid + '}').join(', '));
  L.push('};');
  L.push('');
  totalBytes += a.grids.length * 200 + a.steps.length * 4 + a.pal.length * 2;
});

L.push('static const pa_anim_t PA_ANIMS[] = {');
anims.forEach((a) => {
  const n = a.name.replace(/[^A-Za-z0-9]+/g, '_');
  L.push('  { "' + a.name + '", pa_pal_' + n + ', ' + a.pal.length + ', pa_grids_' + n +
         ', ' + a.grids.length + ', pa_steps_' + n + ', ' + a.steps.length + ' },');
});
L.push('};');
L.push('#define PA_COUNT ' + anims.length);
L.push('');
anims.forEach((a, i) => L.push('#define ' + id(a.name).padEnd(24) + ' ' + i));
L.push('');

fs.writeFileSync(OUT, L.join('\n') + '\n');
console.log('\n%s: %d animacoes, ~%d bytes de flash', path.relative(process.cwd(), OUT),
            anims.length, totalBytes);
