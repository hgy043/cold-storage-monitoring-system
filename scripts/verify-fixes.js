#!/usr/bin/env node
/**
 * 修复回归测试（针对项目检查报告中的 JS 端问题）
 * 前置：web/server.js 与 node-gateway/gateway.js 已启动。
 * 1) readBody 超 1MB → 应返回 413 且不悬挂
 * 2) tcp_probe 行为：未带 node_id 的帧会自动补上（先注册再发帧，能收到回包）
 * 通过 exit 0，失败 exit 1。
 */
'use strict';
const http = require('http');
const net = require('net');

const HOST = '127.0.0.1';
const WEB = 8080;
const GW = 18830;

function httpPost(path, body) {
  return new Promise((resolve, reject) => {
    const data = Buffer.isBuffer(body) ? body : Buffer.from(body || '');
    const req = http.request({ host: HOST, port: WEB, path, method: 'POST',
      headers: { 'Content-Type': 'application/json', 'Content-Length': data.length }, timeout: 8000 },
      (res) => { let b = ''; res.on('data', c => b += c); res.on('end', () => resolve({ status: res.statusCode, body: b })); });
    req.on('error', reject);
    req.on('timeout', () => { req.destroy(new Error('timeout')); });
    req.write(data);
    req.end();
  });
}

const NL = String.fromCharCode(10); // 帧分隔符 \n（避免字面量转义问题）

function tcpProbe(nodeId, frame) {
  return new Promise((resolve, reject) => {
    const sock = net.createConnection({ host: HOST, port: GW });
    let buf = '';
    const lines = [];
    const timer = setTimeout(() => { sock.destroy(); reject(new Error('probe timeout')); }, 8000);
    sock.on('connect', () => {
      sock.write(JSON.stringify({ type: 'register', node_id: nodeId, name: 'test', interval: 5, model: 'probe' }) + NL);
      let f = JSON.parse(frame);
      if (!f.node_id) f.node_id = nodeId;
      sock.write(JSON.stringify(f) + NL);
    });
    sock.on('data', (c) => {
      buf += c;
      let i;
      while ((i = buf.indexOf(NL)) >= 0) {
        const line = buf.slice(0, i).trim(); buf = buf.slice(i + 1);
        if (line) lines.push(JSON.parse(line));
      }
      const hasAck = lines.some(l => l.type === 'register_ack');
      /* 上行 data 帧网关不回包，探针只需拿到 register_ack 即闭环 */
      if (hasAck) { clearTimeout(timer); sock.destroy(); resolve(lines); }
    });
    sock.on('error', (e) => { clearTimeout(timer); reject(e); });
  });
}

(async () => {
  let pass = 0, fail = 0;
  const check = (name, ok, detail) => {
    if (ok) { pass++; console.log('PASS', name + (detail ? ' — ' + detail : '')); }
    else { fail++; console.log('FAIL', name + (detail ? ' — ' + detail : '')); }
  };

  // 1) 413 不悬挂
  try {
    const big = Buffer.alloc(1100 * 1024, 'x');
    const t0 = Date.now();
    const r = await httpPost('/api/devices/cold01_front/commands', big);
    const ms = Date.now() - t0;
    check('超 1MB 请求返回 413 且不悬挂', r.status === 413 && ms < 5000, 'status=' + r.status + ' elapsed=' + ms + 'ms');
  } catch (e) {
    check('超 1MB 请求返回 413 且不悬挂', false, e.message);
  }

  // 2) tcp_probe 自动注册：能收到 register_ack（query 是下行帧，网关不回；此测试只验证注册闭环）
  try {
    const frames = await tcpProbe('cold99_probe', JSON.stringify({ type: 'data', ts: '2026-01-01T00:00:00+08:00', temperature: -18.5, humidity: 60, led: 0, beeper: 0, alarm: 0 }));
    const ra = frames.find(l => l.type === 'register_ack');
    check('探针自动注册后收到 register_ack（data 帧上行不回包属预期）',
      !!ra && ra.node_id === 'cold99_probe' && ra.result === 'success',
      '收到 ' + frames.length + ' 帧: ' + frames.map(l => l.type).join(','));
  } catch (e) {
    check('探针自动注册后收到 register_ack（data 帧上行不回包属预期）', false, e.message);
  }

  console.log('');
  console.log('===== 修复回归: ' + pass + ' 通过, ' + fail + ' 失败 =====');
  process.exit(fail > 0 ? 1 : 0);
})().catch(e => { console.error('异常:', e); process.exit(1); });