#!/usr/bin/env node
/* 生成 800x480 24bit 深色科技风背景 BMP（供 LVGL lv_img 加载，盘符 S: 见 lv_conf.h） */
'use strict';
const fs = require('fs');
const path = require('path');
const W = 800, H = 480;            //800*480
const buf = Buffer.alloc(54 + W * H * 3);
buf.write('BM', 0);
buf.writeUInt32LE(54 + W * H * 3, 2);
buf.writeUInt32LE(54, 10);
buf.writeUInt32LE(40, 14);
buf.writeInt32LE(W, 18);
buf.writeInt32LE(H, 22);
buf.writeUInt16LE(1, 26);
buf.writeUInt16LE(24, 28);
buf.writeUInt32LE(0, 30);
buf.writeUInt32LE(W * H * 3, 34);
buf.writeInt32LE(2835, 38);
buf.writeInt32LE(2835, 42);

const lerp = (a, b, t) => a + (b - a) * t;
const stops = [
  [0.00, [0x10, 0x26, 0x4a]],
  [0.35, [0x0b, 0x1a, 0x35]],
  [0.75, [0x08, 0x12, 0x28]],
  [1.00, [0x05, 0x0b, 0x18]],
];
function baseColor(y) {
  const t = y / (H - 1);
  let i = 0;
  while (i < stops.length - 2 && t > stops[i + 1][0]) i++;
  const [t0, c0] = stops[i];
  const [t1, c1] = stops[i + 1];
  const k = Math.min(1, Math.max(0, (t - t0) / (t1 - t0)));
  return [lerp(c0[0], c1[0], k), lerp(c0[1], c1[1], k), lerp(c0[2], c1[2], k)];
}
function radial(cx, cy, r, strength, x, y) {
  const dx = x - cx, dy = y - cy;
  const d = Math.sqrt(dx * dx + dy * dy);
  if (d > r) return 0;
  return Math.pow(1 - d / r, 2.2) * strength;
}
for (let y = 0; y < H; y++) {
  for (let x = 0; x < W; x++) {
    let r, g, b;
    const c = baseColor(y);
    r = c[0]; g = c[1]; b = c[2];
    const gl = radial(140, 50, 760, 26, x, y) + radial(930, 520, 700, 20, x, y);
    r += gl * 0.10; g += gl * 0.45; b += gl * 0.70;
    if (y === 88) { r = 0x2a; g = 0xd8; b = 0xff; }
    if (y === 89) { r = lerp(r, 0x35, 0.5); g = lerp(g, 0xd8, 0.5); b = lerp(b, 0xff, 0.5); }
    if (x < 3) { r = 0x35; g = 0xd8; b = 0xff; }
    const R = Math.min(255, Math.max(0, r)) | 0;
    const G = Math.min(255, Math.max(0, g)) | 0;
    const B = Math.min(255, Math.max(0, b)) | 0;
    const off = 54 + (y * W + x) * 3;
    buf[off] = B; buf[off + 1] = G; buf[off + 2] = R;
  }
}
const out = path.join(__dirname, 'bg.bmp');
fs.writeFileSync(out, buf);
console.log('bg.bmp written:', buf.length, 'bytes');
