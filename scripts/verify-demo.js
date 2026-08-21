#!/usr/bin/env node
/**
 * 端到端联调验证脚本（无第三方依赖）
 * 前置：web/server.js、node-gateway/gateway.js、tools/simulate_devices.js 均已启动。
 * 用法：node scripts/verify-demo.js [--web-port 8080] [--timeout 30000]
 * 通过则 exit 0，失败 exit 1，供 CI/一键验证使用。
 */
'use strict';
const http = require('http');

const args = process.argv.slice(2);
const getArg = (name, def) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : def;
};
const WEB_PORT = parseInt(getArg('--web-port', '8080'), 10);
const GLOBAL_TIMEOUT = parseInt(getArg('--timeout', '30000'), 10);

function httpReq(method, path, body) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const req = http.request({
      host: '127.0.0.1', port: WEB_PORT, path, method,
      headers: data ? { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data) } : {},
      timeout: 5000,
    }, (res) => {
      let buf = '';
      res.on('data', (c) => (buf += c));
      res.on('end', () => {
        let json = null;
        try { json = JSON.parse(buf); } catch { /* ignore */ }
        resolve({ status: res.statusCode, json, raw: buf });
      });
    });
    req.on('error', reject);
    req.on('timeout', () => { req.destroy(new Error('timeout')); });
    if (data) req.write(data);
    req.end();
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (tag, msg) => console.log('[' + new Date().toISOString().slice(11, 23) + '] ' + tag.padEnd(6) + ' ' + msg);

async function waitFor(fn, what, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let lastErr = null;
  while (Date.now() < deadline) {
    try { const v = await fn(); if (v) return v; } catch (e) { lastErr = e; }
    await sleep(1000);
  }
  throw new Error('等待超时: ' + what + (lastErr ? ' (最后错误: ' + lastErr.message + ')' : ''));
}

(async () => {
  let pass = 0, fail = 0;
  const check = (name, ok, detail) => {
    if (ok) { pass++; log('PASS', name + (detail ? ' — ' + detail : '')); }
    else { fail++; log('FAIL', name + (detail ? ' — ' + detail : '')); }
  };

  log('INFO', '开始端到端验证 (web port ' + WEB_PORT + ')');

  // 1. 服务可用
  const status = await waitFor(() => httpReq('GET', '/api/status').then(r => r.status === 200 ? r : null),
    'web 服务就绪', GLOBAL_TIMEOUT);
  check('web 服务 /api/status 返回 200', !!status);

  // 2. 等待设备数据上报
  const devs = await waitFor(async () => {
    const r = await httpReq('GET', '/api/devices');
    if (r.status !== 200 || !r.json || !r.json.length) return null;
    const withData = r.json.filter(d => d.latest && typeof d.latest.temperature === 'number');
    return withData.length > 0 ? withData : null;
  }, '收到设备数据上报', GLOBAL_TIMEOUT);
  check('设备数据已上报 (' + devs.length + ' 个节点有数据)', devs.length > 0,
    devs.slice(0, 3).map(d => (d.device_id || d.deviceId) + '=' + d.latest.temperature + '℃').join(', '));

  // 3. 单个设备详情
  const target = 'cold01_front';
  const detail = await httpReq('GET', '/api/devices/' + target);
  const hasDetail = detail.status === 200 && detail.json && typeof detail.json.latest.temperature === 'number';
  check('设备详情 /api/devices/' + target, hasDetail, hasDetail ? '温度=' + detail.json.latest.temperature : 'HTTP ' + detail.status);

  // 4. 统计聚合接口
  const stats = await httpReq('GET', '/api/stats');
  const statsOk = stats.status === 200 && stats.json && Array.isArray(stats.json.coldStorages) && stats.json.coldStorages.length > 0;
  check('/api/stats 返回冷库聚合', statsOk, statsOk ? stats.json.coldStorages.length + ' 个冷库' : stats.raw.slice(0, 120));

  // 5. 历史曲线接口
  const hist = await httpReq('GET', '/api/devices/' + target + '/history?points=10');
  const buckets = hist.json && Array.isArray(hist.json.buckets) ? hist.json.buckets : null;
  const histOk = hist.status === 200 && buckets && buckets.length > 0 && buckets[0].temp && 'avg' in buckets[0].temp;
  check('/api/devices/' + target + '/history 返回聚合序列', histOk, histOk ? buckets.length + ' 个桶' : hist.raw.slice(0, 120));

  // 6. 命令下发闭环（set_led → 网关 → 设备 → ack → 云端响应）
  let cmdId = null;
  try {
    const cmd = await waitFor(async () => {
      const r = await httpReq('POST', '/api/devices/' + target + '/commands', { command: 'set_led', paras: { on: 1 } });
      return r.status === 200 && r.json && r.json.requestId ? r : null;
    }, '命令接口可用', 10000);
    cmdId = cmd.json.requestId;
    check('下发命令 set_led 返回 requestId=' + cmdId, true);
  } catch (e) {
    check('下发命令 set_led 返回 requestId', false, e.message);
  }

  if (cmdId) {
    try {
      const acked = await waitFor(async () => {
        const r = await httpReq('GET', '/api/events?limit=300');
        if (r.status !== 200) return null;
        /* ack 以事件形式落库：内容含 requestId 且带“成功” */
        return (r.json || []).find(ev =>
          (ev.type === 'command' || ev.type === 'ack') &&
          String(ev.content || '').includes(cmdId) &&
          String(ev.content || '').includes('成功')) || null;
      }, '命令 ack 闭环', 15000);
      check('命令闭环完成（设备 ack 已回到云端并记录）', !!acked, acked ? JSON.stringify(acked).slice(0, 120) : '');
    } catch (e) {
      check('命令闭环完成（设备 ack 已回到云端并记录）', false, e.message);
    }
  }

  // 7. 事件日志
  const ev = await httpReq('GET', '/api/events?limit=50');
  check('/api/events 有事件记录', ev.status === 200 && Array.isArray(ev.json) && ev.json.length > 0, (ev.json || []).length + ' 条');

  console.log('\n===== 验证结果: ' + pass + ' 通过, ' + fail + ' 失败 =====');
  process.exit(fail > 0 ? 1 : 0);
})().catch((e) => {
  console.error('验证脚本异常:', e);
  process.exit(1);
});