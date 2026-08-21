'use strict';

// ====================================================================
// 冷库数据监控系统 - Web 后端（单文件）
// 内嵌 aedes MQTT broker + REST + WebSocket + 内存/文件数据存储
// 仅依赖 aedes / ws / mqtt，HTTP 使用原生 http 模块，Node >= 18
// ====================================================================

const http = require('http');
const net = require('net');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const aedes = require('aedes')();
const mqtt = require('mqtt');
const { WebSocketServer } = require('ws');

// ------------------------------ 配置 ------------------------------
function loadConfig() {
  const cfg = {
    gatewayId: 'demo_gateway_01',  // 默认演示值；真实部署用 GATEWAY_ID 环境变量或 config.json 注入
    mqttPort: 1883,
    httpPort: 8080,
    onlineTimeoutSec: 60,        // 设备无数据判定离线时长（秒）
    historyMax: 3000,            // 每节点历史环形数组上限
    eventsMax: 2000,             // 事件环形数组上限
    persistIntervalMs: 30000,    // 持久化间隔
  };
  try {
    const cfgPath = path.join(__dirname, 'config.json');
    if (fs.existsSync(cfgPath)) {
      const c = JSON.parse(fs.readFileSync(cfgPath, 'utf8'));
      Object.assign(cfg, c);
    }
  } catch (e) {
    console.log('[config] config.json 读取失败，使用默认值:', e.message);
  }
  cfg.gatewayId = process.env.GATEWAY_ID || cfg.gatewayId;
  cfg.mqttPort = parseInt(process.env.MQTT_PORT || cfg.mqttPort, 10) || 1883;
  cfg.httpPort = parseInt(process.env.PORT || cfg.httpPort, 10) || 8080;
  return cfg;
}
const CFG = loadConfig();

const dataDir = path.join(__dirname, 'data');
const historyPath = path.join(dataDir, 'history.json');
const publicDir = path.join(__dirname, 'public');
const startTime = Date.now();
const onlineTimeoutMs = CFG.onlineTimeoutSec * 1000;

// --------------------------- 内存存储 ----------------------------
const devices = new Map();   // device_id -> record
const events = [];           // 事件环形数组（旧 -> 新）
const pending = new Map();   // requestId -> pending 命令信息
let gatewayOnline = false;
let gatewayLastSeen = 0;
let mqttClient = null;
let wss = null;

// ----------------------------- 工具 ------------------------------
function log() {
  const args = Array.prototype.slice.call(arguments);
  console.log.apply(console, ['[' + new Date().toISOString() + ']'].concat(args));
}
function round2(n) { return Math.round(n * 100) / 100; }
function num(v) { return (typeof v === 'number' && isFinite(v)) ? v : null; }
function deviceDisplayName(id) {
  const m = /^cold(\d+)_(front|back)$/.exec(id);
  if (m) return parseInt(m[1], 10) + '号冷库-' + (m[2] === 'front' ? '前门' : '后门');
  return id;
}
function coldIndex(id) {
  const m = /^cold(\d+)_/.exec(id);
  return m ? parseInt(m[1], 10) : null;
}
function avg(a) { return a.length ? a.reduce(function (s, x) { return s + x; }, 0) / a.length : 0; }
function maxOf(a) { return a.length ? Math.max.apply(null, a) : null; }
function minOf(a) { return a.length ? Math.min.apply(null, a) : null; }
function extractRequestId(topic) {
  const m = /request_id=([^/]+)$/.exec(topic);
  return m ? m[1] : null;
}

function broadcast(obj) {
  if (!wss) return;
  const msg = JSON.stringify(obj);
  wss.clients.forEach(function (client) {
    if (client.readyState === 1) {
      try { client.send(msg); } catch (e) { /* ignore */ }
    }
  });
}

// ---------------------------- 事件日志 ----------------------------
function addEvent(type, content, deviceId, level) {
  level = level || 'info';
  const e = {
    ts: Date.now(),
    time: new Date().toISOString(),
    type: type,
    level: level,
    content: content,
    device_id: deviceId || null,
  };
  events.push(e);
  if (events.length > CFG.eventsMax) events.shift();
  broadcast({ type: 'event', event: e });
  return e;
}
function getEvents(limit) {
  const n = Math.min(limit || 100, events.length);
  return events.slice(-n).reverse();
}

// ---------------------------- 状态/查询 ----------------------------
function getStatus() {
  let onlineCount = 0;
  devices.forEach(function (r) { if (r.online) onlineCount++; });
  return {
    gatewayOnline: gatewayOnline,
    deviceCount: devices.size,
    onlineCount: onlineCount,
    brokerPort: CFG.mqttPort,
    httpPort: CFG.httpPort,
    uptime: Math.floor((Date.now() - startTime) / 1000),
  };
}

function getDevicesList() {
  const arr = [];
  devices.forEach(function (r) {
    arr.push({ device_id: r.device_id, name: r.name, online: r.online, lastSeen: r.lastSeen, latest: r.latest });
  });
  arr.sort(function (a, b) { return a.device_id < b.device_id ? -1 : (a.device_id > b.device_id ? 1 : 0); });
  return arr;
}

function getDeviceDetail(id) {
  const r = devices.get(id);
  if (!r) return null;
  const temps = r.history.map(function (h) { return h.temperature; }).filter(function (v) { return typeof v === 'number' && isFinite(v); });
  const hums = r.history.map(function (h) { return h.humidity; }).filter(function (v) { return typeof v === 'number' && isFinite(v); });
  return {
    device_id: id,
    name: r.name,
    online: r.online,
    lastSeen: r.lastSeen,
    latest: r.latest,
    summary: {
      samples: r.history.length,
      temperature: temps.length ? { avg: round2(avg(temps)), max: maxOf(temps), min: minOf(temps) } : null,
      humidity: hums.length ? { avg: round2(avg(hums)), max: maxOf(hums), min: minOf(hums) } : null,
    },
  };
}

function getHistory(id, points) {
  const r = devices.get(id);
  if (!r) return null;
  const hist = r.history;
  const n = hist.length;
  const buckets = [];
  if (n === 0) return { device_id: id, points: points, buckets: buckets };
  const bucketCount = Math.max(1, Math.min(points, n));
  const size = n / bucketCount;
  for (let b = 0; b < bucketCount; b++) {
    const start = Math.floor(b * size);
    const end = Math.min(n, Math.max(start + 1, Math.floor((b + 1) * size)));
    const slice = hist.slice(start, end);
    const temps = slice.map(function (s) { return s.temperature; }).filter(function (v) { return typeof v === 'number' && isFinite(v); });
    const hums = slice.map(function (s) { return s.humidity; }).filter(function (v) { return typeof v === 'number' && isFinite(v); });
    buckets.push({
      t: slice[0].ts,
      temp: temps.length ? { avg: round2(avg(temps)), max: maxOf(temps), min: minOf(temps) } : null,
      humidity: hums.length ? { avg: round2(avg(hums)), max: maxOf(hums), min: minOf(hums) } : null,
    });
  }
  return { device_id: id, points: points, buckets: buckets };
}

function getAgg(id, windowSec) {
  const r = devices.get(id);
  if (!r) return null;
  const cutoff = Date.now() - windowSec * 1000;
  const slice = r.history.filter(function (h) { return h.ts >= cutoff; });
  const temps = slice.map(function (s) { return s.temperature; }).filter(function (v) { return typeof v === 'number' && isFinite(v); });
  const hums = slice.map(function (s) { return s.humidity; }).filter(function (v) { return typeof v === 'number' && isFinite(v); });
  return {
    device_id: id,
    window: windowSec,
    count: slice.length,
    temperature: temps.length ? { max: maxOf(temps), avg: round2(avg(temps)), min: minOf(temps) } : null,
    humidity: hums.length ? { max: maxOf(hums), avg: round2(avg(hums)), min: minOf(hums) } : null,
  };
}

// 冷库聚合（当前仅 1 号冷库；供卡片与柱状图）
function getStats() {
  const map = {};
  devices.forEach(function (r) {
    const idx = coldIndex(r.device_id);
    if (idx === null || idx < 1 || idx > 16) return;
    if (!map[idx]) map[idx] = { index: idx, name: idx + '号冷库', front: null, back: null };
    const pos = /_front$/.test(r.device_id) ? 'front' : 'back';
    map[idx][pos] = r.latest ? {
      temperature: r.latest.temperature,
      humidity: r.latest.humidity,
      led: r.latest.led,
      beeper: r.latest.beeper,
      alarm: r.latest.alarm,
      threshold: r.latest.alarm_threshold,
      online: r.online,
    } : null;
  });
  const coldStorages = [];
  for (let i = 1; i <= 1; i++) {
    const s = map[i] || { index: i, name: i + '号冷库', front: null, back: null };
    const temps = [];
    if (s.front && s.front.online && typeof s.front.temperature === 'number') temps.push(s.front.temperature);
    if (s.back && s.back.online && typeof s.back.temperature === 'number') temps.push(s.back.temperature);
    const avgT = temps.length ? round2(avg(temps)) : null;
    let threshold = -20;
    if (s.front && typeof s.front.threshold === 'number') threshold = s.front.threshold;
    else if (s.back && typeof s.back.threshold === 'number') threshold = s.back.threshold;
    const alarm = (s.front && s.front.online && typeof s.front.temperature === 'number' && s.front.temperature > threshold)
      || (s.back && s.back.online && typeof s.back.temperature === 'number' && s.back.temperature > threshold);
    coldStorages.push({ index: i, name: i + '号冷库', front: s.front, back: s.back, avg: avgT, threshold: threshold, alarm: !!alarm });
  }
  return { coldStorages: coldStorages };
}

// ---------------------------- 告警逻辑 ----------------------------
function checkAlarm(rec) {
  const l = rec.latest;
  if (!l) return;
  const th = l.alarm_threshold;
  if (typeof th === 'number' && typeof l.temperature === 'number' && l.temperature > th) {
    if (!rec.alarmActive) {
      rec.alarmActive = true;
      addEvent('alarm', rec.device_id + ' 温度 ' + l.temperature + '℃ 超过阈值 ' + th + '℃', rec.device_id, 'warn');
    }
  } else if (rec.alarmActive) {
    rec.alarmActive = false;
    addEvent('alarm', rec.device_id + ' 温度恢复正常', rec.device_id, 'info');
  }
}

// ------------------------- MQTT 消息处理 --------------------------
// 存储单个设备属性（一号冷库前后门共用）
function storeProps(devId, props, evt) {
  const now = Date.now();
  let rec = devices.get(devId);
  let isNew = false;
  if (!rec) {
    rec = { device_id: devId, name: deviceDisplayName(devId), latest: null, history: [], online: false, lastSeen: 0, alarmActive: false, lastDataEventTs: 0 };
    devices.set(devId, rec);
    isNew = true;
  }
  const wasOnline = rec.online;
  const latest = {
    temperature: num(props.temperature),
    humidity: num(props.humidity),
    led: num(props.led),
    beeper: num(props.beeper),
    alarm: num(props.alarm),
    alarm_threshold: (typeof props.alarm_threshold === 'number')
      ? props.alarm_threshold
      : (rec.latest && typeof rec.latest.alarm_threshold === 'number' ? rec.latest.alarm_threshold : -20),
    event_time: evt || null,
    ts: now,
  };
  rec.latest = latest;
  rec.lastSeen = now;
  rec.online = true;
  rec.history.push({ ts: now, temperature: latest.temperature, humidity: latest.humidity, led: latest.led, beeper: latest.beeper, alarm: latest.alarm, alarm_threshold: latest.alarm_threshold });
  if (rec.history.length > CFG.historyMax) rec.history.shift();
  checkAlarm(rec);
  if (now - rec.lastDataEventTs >= 60000) {
    rec.lastDataEventTs = now;
    addEvent('data', devId + ' 温度 ' + (latest.temperature === null ? '-' : latest.temperature) + '℃ 湿度 ' + (latest.humidity === null ? '-' : latest.humidity) + '%', devId, 'info');
  }
  if (isNew) addEvent('online', '设备 ' + devId + ' 首次上线', devId, 'info');
  else if (!wasOnline) addEvent('online', '设备 ' + devId + ' 上线', devId, 'info');
}

// 直接属性上报（华为云单设备 yihao 虚拟数据）→ 一号冷库前后门映射
function handleDirectReport(topic, payload) {
  let msg;
  try { msg = JSON.parse(payload.toString('utf8')); }
  catch (e) { log('JSON 解析失败(properties/report):', e.message); return; }
  const gwId = topic.split('/')[2];
  if (gwId === CFG.gatewayId) { gatewayOnline = true; gatewayLastSeen = Date.now(); }
  if (!msg || !Array.isArray(msg.services)) return;
  const svc = msg.services[0] || {};
  const props = svc.properties || {};
  storeProps('cold01_front', props, svc.event_time || null);
  storeProps('cold01_back', props, svc.event_time || null);
  broadcast({ type: 'data', devices: getDevicesList(), stats: getStats() });
  broadcast({ type: 'status', status: getStatus() });
}

function handleReport(topic, payload) {
  let msg;
  try { msg = JSON.parse(payload.toString('utf8')); }
  catch (e) { log('JSON 解析失败(properties/report):', e.message); return; }
  const gwId = topic.split('/')[2];
  if (gwId === CFG.gatewayId) { gatewayOnline = true; gatewayLastSeen = Date.now(); }
  if (!msg || !Array.isArray(msg.devices)) return;
  const now = Date.now();
  msg.devices.forEach(function (d) {
    if (!d || !d.device_id) return;
    const devId = d.device_id;
    let rec = devices.get(devId);
    let isNew = false;
    if (!rec) {
      rec = { device_id: devId, name: deviceDisplayName(devId), latest: null, history: [], online: false, lastSeen: 0, alarmActive: false, lastDataEventTs: 0 };
      devices.set(devId, rec);
      isNew = true;
    }
    const svc = (d.services && d.services[0]) || {};
    const props = svc.properties || {};
    const wasOnline = rec.online;
    const latest = {
      temperature: num(props.temperature),
      humidity: num(props.humidity),
      led: num(props.led),
      beeper: num(props.beeper),
      alarm: num(props.alarm),
      alarm_threshold: (typeof props.alarm_threshold === 'number')
        ? props.alarm_threshold
        : (rec.latest && typeof rec.latest.alarm_threshold === 'number' ? rec.latest.alarm_threshold : -20),
      event_time: svc.event_time || null,
      ts: now,
    };
    rec.latest = latest;
    rec.lastSeen = now;
    rec.online = true;
    rec.history.push({
      ts: now, temperature: latest.temperature, humidity: latest.humidity,
      led: latest.led, beeper: latest.beeper, alarm: latest.alarm, alarm_threshold: latest.alarm_threshold,
    });
    if (rec.history.length > CFG.historyMax) rec.history.shift();
    checkAlarm(rec);
    if (now - rec.lastDataEventTs >= 60000) {
      rec.lastDataEventTs = now;
      addEvent('data', devId + ' 温度 ' + (latest.temperature === null ? '-' : latest.temperature) + '℃ 湿度 ' + (latest.humidity === null ? '-' : latest.humidity) + '%', devId, 'info');
    }
    if (isNew) {
      addEvent('online', '设备 ' + devId + ' 首次上线', devId, 'info');
    } else if (!wasOnline) {
      addEvent('online', '设备 ' + devId + ' 上线', devId, 'info');
    }
  });
  broadcast({ type: 'data', devices: getDevicesList(), stats: getStats() });
  broadcast({ type: 'status', status: getStatus() });
}

function handleCommandResponse(topic, payload) {
  let msg;
  try { msg = JSON.parse(payload.toString('utf8')); }
  catch (e) { log('JSON 解析失败(commands/response):', e.message); return; }
  const requestId = extractRequestId(topic);
  if (!requestId) return;
  const p = pending.get(requestId);
  if (!p) return;
  pending.delete(requestId);
  const ok = !!(msg && msg.result_code === 0);
  addEvent(ok ? 'command' : 'alarm', '命令 ' + p.command + ' (' + requestId + ') → ' + p.deviceId + ' ' + (ok ? '成功' : '失败'), p.deviceId, ok ? 'info' : 'warn');
  broadcast({ type: 'ack', requestId: requestId, deviceId: p.deviceId, command: p.command, result_code: msg && msg.result_code, response: msg });
}

function handlePropertiesResponse(topic, payload) {
  let msg;
  try { msg = JSON.parse(payload.toString('utf8')); }
  catch (e) { log('JSON 解析失败(properties/set/response):', e.message); return; }
  const requestId = extractRequestId(topic);
  if (!requestId) return;
  const p = pending.get(requestId);
  if (!p) return;
  pending.delete(requestId);
  const ok = !!(msg && msg.result_code === 0);
  if (ok && p.paras && typeof p.paras.value === 'number') {
    const rec = devices.get(p.deviceId);
    if (rec && rec.latest) rec.latest.alarm_threshold = p.paras.value;
  }
  addEvent(ok ? 'command' : 'alarm', '阈值设置 (' + requestId + ') → ' + p.deviceId + ' ' + (ok ? '成功' : '失败'), p.deviceId, ok ? 'info' : 'warn');
  broadcast({ type: 'ack', requestId: requestId, deviceId: p.deviceId, command: p.command, result_code: msg && msg.result_code, response: msg });
}

// ------------------------- MQTT 客户端 ----------------------------
function startMqttClient() {
  const client = mqtt.connect('mqtt://127.0.0.1:' + CFG.mqttPort, {
    clientId: 'web-backend-' + crypto.randomBytes(4).toString('hex'),
    clean: true,
    reconnectPeriod: 3000,
    connectTimeout: 10000,
  });
  client.on('connect', function () {
    log('已连接本地 broker，开始订阅主题');
    const topics = [
      '$oc/devices/+/sys/gateway/sub-devices/properties/report',
      '$oc/devices/+/sys/properties/report',
      '$oc/devices/+/sys/commands/response/#',
      '$oc/devices/+/sys/properties/set/response/#',
    ];
    client.subscribe(topics, { qos: 1 }, function (err) {
      if (err) log('订阅失败:', err.message);
      else log('订阅成功:', topics.join(', '));
    });
  });
  client.on('message', function (topic, payload) {
    if (topic.indexOf('/sys/gateway/sub-devices/properties/report') !== -1) {
      handleReport(topic, payload);
    } else if (topic.indexOf('/sys/properties/report') !== -1) {
      handleDirectReport(topic, payload);
    } else if (topic.indexOf('/sys/commands/response/') !== -1) {
      handleCommandResponse(topic, payload);
    } else if (topic.indexOf('/sys/properties/set/response/') !== -1) {
      handlePropertiesResponse(topic, payload);
    }
    // 未知 topic 静默忽略
  });
  client.on('error', function (err) { log('MQTT 客户端错误:', err.message); });
  client.on('reconnect', function () { log('MQTT 客户端重连中...'); });
  client.on('close', function () { log('MQTT 客户端断开'); });
  return client;
}

// ------------------------- 命令/属性下发 ---------------------------
const VALID_COMMANDS = {
  set_led: true,
  set_beeper: true,
  set_interval: true,
  set_alarm_threshold: true,
  query: true,   /* 状态查询：网关转发为协议 query 帧（docs/02 §4.2） */
};

function handleCommand(res, deviceId, body) {
  const command = body.command;
  if (!command || !VALID_COMMANDS[command]) {
    return json(res, { error: '未知命令: ' + command + '，支持 set_led / set_beeper / set_interval / set_alarm_threshold / query' }, 400);
  }
  const paras = body.paras || {};
  const requestId = 'req_' + crypto.randomUUID();
  const topic = '$oc/devices/' + CFG.gatewayId + '/sys/commands/request_id=' + requestId;
  const payload = { object_device_id: deviceId, command_name: command, service_id: 'coldstorage', paras: paras };
  pending.set(requestId, { requestId: requestId, deviceId: deviceId, command: command, paras: paras, ts: Date.now(), kind: 'command' });
  mqttClient.publish(topic, JSON.stringify(payload), { qos: 1 }, function (err) {
    if (err) log('命令发布失败:', err.message);
    else log('命令已发布:', topic, JSON.stringify(payload));
  });
  addEvent('command', '下发命令 ' + command + ' → ' + deviceId + ' (' + requestId + ')', deviceId, 'info');
  return json(res, { requestId: requestId, device_id: deviceId, command: command, topic: topic });
}

function handleProperties(res, deviceId, body) {
  const value = body.alarm_threshold;
  if (typeof value !== 'number' || !isFinite(value)) {
    return json(res, { error: 'alarm_threshold 必须是数字' }, 400);
  }
  const requestId = 'req_' + crypto.randomUUID();
  const topic = '$oc/devices/' + CFG.gatewayId + '/sys/properties/set/request_id=' + requestId;
  const payload = { object_device_id: deviceId, services: [{ service_id: 'coldstorage', properties: { alarm_threshold: value } }] };
  pending.set(requestId, { requestId: requestId, deviceId: deviceId, command: 'set_alarm_threshold', paras: { value: value }, ts: Date.now(), kind: 'properties' });
  mqttClient.publish(topic, JSON.stringify(payload), { qos: 1 }, function (err) {
    if (err) log('属性设置发布失败:', err.message);
    else log('属性设置已发布:', topic);
  });
  addEvent('command', '设置 ' + deviceId + ' 报警阈值 = ' + value + '℃ (' + requestId + ')', deviceId, 'info');
  return json(res, { requestId: requestId, device_id: deviceId, alarm_threshold: value, topic: topic });
}

// --------------------------- HTTP 服务 ----------------------------
function json(res, obj, status) {
  status = status || 200;
  const body = JSON.stringify(obj);
  res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Content-Length': Buffer.byteLength(body) });
  res.end(body);
}

function readBody(req) {
  return new Promise(function (resolve, reject) {
    let data = '';
    let tooLarge = false;
    req.on('data', function (c) {
      if (tooLarge) return;
      data += c;
      if (data.length > 1048576) {
        tooLarge = true;
        data = '';
        req.pause();                       /* 停止继续接收，防止内存膨胀 */
        var err = new Error('payload too large');
        err.statusCode = 413;
        reject(err);
      }
    });
    req.on('end', function () { if (!tooLarge) resolve(data); });
    req.on('error', reject);
  });
}

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'application/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
};

function serveStatic(req, res, pathname) {
  const rel = (pathname === '/' || pathname === '') ? '/index.html' : pathname;
  const publicResolved = path.resolve(publicDir);
  const filePath = path.resolve(publicDir, '.' + rel);
  if (filePath !== publicResolved && filePath.indexOf(publicResolved + path.sep) !== 0) {
    res.writeHead(403, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('Forbidden');
    return;
  }
  fs.readFile(filePath, function (err, data) {
    if (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('Not Found');
      return;
    }
    const ext = path.extname(filePath).toLowerCase();
    res.writeHead(200, { 'Content-Type': MIME[ext] || 'application/octet-stream' });
    res.end(data);
  });
}

const httpServer = http.createServer(async function (req, res) {
  let url;
  try { url = new URL(req.url, 'http://localhost'); }
  catch (e) { res.writeHead(400); res.end('Bad Request'); return; }
  let pathname;
  try { pathname = decodeURIComponent(url.pathname); }
  catch (e) { pathname = url.pathname; }
  const method = req.method || 'GET';
  try {
    if (pathname === '/api/status' && method === 'GET') return json(res, getStatus());
    if (pathname === '/api/devices' && method === 'GET') return json(res, getDevicesList());
    if (pathname === '/api/stats' && method === 'GET') return json(res, getStats());
    if (pathname === '/api/events' && method === 'GET') {
      const limit = Math.min(1000, parseInt(url.searchParams.get('limit') || '100', 10) || 100);
      return json(res, getEvents(limit));
    }
    const dm = pathname.match(/^\/api\/devices\/([^/]+)(?:\/(history|agg|commands|properties))?$/);
    if (dm) {
      const id = dm[1];
      const sub = dm[2];
      if (!sub && method === 'GET') {
        const d = getDeviceDetail(id);
        return d ? json(res, d) : json(res, { error: 'device not found' }, 404);
      }
      if (sub === 'history' && method === 'GET') {
        const points = Math.max(1, Math.min(500, parseInt(url.searchParams.get('points') || '60', 10) || 60));
        const d = getHistory(id, points);
        return d ? json(res, d) : json(res, { error: 'device not found' }, 404);
      }
      if (sub === 'agg' && method === 'GET') {
        const windowSec = Math.max(1, parseInt(url.searchParams.get('window') || '3600', 10) || 3600);
        const d = getAgg(id, windowSec);
        return d ? json(res, d) : json(res, { error: 'device not found' }, 404);
      }
      if (sub === 'commands' && method === 'POST') {
        const body = await readBody(req);
        let parsed;
        try { parsed = JSON.parse(body || '{}'); } catch (e) { return json(res, { error: 'invalid JSON body' }, 400); }
        return handleCommand(res, id, parsed);
      }
      if (sub === 'properties' && method === 'PUT') {
        const body = await readBody(req);
        let parsed;
        try { parsed = JSON.parse(body || '{}'); } catch (e) { return json(res, { error: 'invalid JSON body' }, 400); }
        return handleProperties(res, id, parsed);
      }
      return json(res, { error: 'method not allowed' }, 405);
    }
    if (method === 'GET' || method === 'HEAD') return serveStatic(req, res, pathname);
    json(res, { error: 'not found' }, 404);
  } catch (e) {
    if (e && e.statusCode) {
      /* 413 等带状态码的错误：返回明确响应（Connection: close），
         并排空剩余请求体后正常关闭，避免请求悬挂/连接被硬掐 */
      if (!res.headersSent) {
        res.setHeader('Connection', 'close');
        json(res, { error: e.message }, e.statusCode);
      }
      try { req.resume(); } catch (err) { /* ignore */ }
      return;
    }
    log('HTTP 错误:', e && e.stack ? e.stack : String(e));
    if (!res.headersSent) json(res, { error: 'internal error' }, 500);
    else res.end();
  }
});

// -------------------------- WebSocket ------------------------------
wss = new WebSocketServer({ server: httpServer, path: '/ws' });
wss.on('connection', function (ws, req) {
  log('WebSocket 客户端连接:', req.socket.remoteAddress);
  ws.send(JSON.stringify({ type: 'status', status: getStatus() }));
  ws.send(JSON.stringify({ type: 'data', devices: getDevicesList(), stats: getStats() }));
  ws.send(JSON.stringify({ type: 'event', events: getEvents(50) }));
  ws.on('error', function () {});
});

// ---------------------------- aedes broker --------------------------
aedes.on('client', function (client) {
  log('MQTT 客户端接入:', client.id);
});
aedes.on('clientReady', function (client) {
  log('MQTT 客户端就绪:', client.id);
  if (client.id === CFG.gatewayId) {
    gatewayOnline = true;
    gatewayLastSeen = Date.now();
    broadcast({ type: 'status', status: getStatus() });
  }
});
aedes.on('clientDisconnect', function (client) {
  log('MQTT 客户端断开:', client.id);
  if (client.id === CFG.gatewayId) {
    gatewayOnline = false;
    broadcast({ type: 'status', status: getStatus() });
  }
});
aedes.on('connectionError', function (client, err) {
  log('连接错误:', err && err.message);
});

// --------------------------- 持久化 ------------------------------
function persist() {
  try {
    if (!fs.existsSync(dataDir)) fs.mkdirSync(dataDir, { recursive: true });
    const snapshot = { savedAt: new Date().toISOString(), devices: {}, events: events.slice(-200) };
    devices.forEach(function (r) {
      snapshot.devices[r.device_id] = { name: r.name, latest: r.latest, lastSeen: r.lastSeen, online: r.online };
    });
    fs.writeFileSync(historyPath, JSON.stringify(snapshot, null, 2));
    log('已持久化快照到 history.json (' + devices.size + ' 设备)');
  } catch (e) { log('持久化失败:', e.message); }
}

function loadPersisted() {
  try {
    if (!fs.existsSync(historyPath)) return;
    const snap = JSON.parse(fs.readFileSync(historyPath, 'utf8'));
    if (snap.devices) {
      Object.keys(snap.devices).forEach(function (id) {
        const d = snap.devices[id];
        const online = !!d.lastSeen && (Date.now() - d.lastSeen < onlineTimeoutMs);
        devices.set(id, { device_id: id, name: d.name || deviceDisplayName(id), latest: d.latest || null, history: [], online: online, lastSeen: d.lastSeen || 0, alarmActive: false, lastDataEventTs: 0 });
      });
    }
    if (Array.isArray(snap.events)) snap.events.forEach(function (e) { events.push(e); });
    log('已加载历史快照: ' + devices.size + ' 设备, ' + events.length + ' 事件');
  } catch (e) { log('加载历史快照失败:', e.message); }
}

// --------------------------- 定时任务 ------------------------------
// 离线检测 + 网关在线检测
setInterval(function () {
  const now = Date.now();
  let changed = false;
  devices.forEach(function (r) {
    if (r.online && r.lastSeen && now - r.lastSeen > onlineTimeoutMs) {
      r.online = false;
      addEvent('offline', '设备 ' + r.device_id + ' 离线', r.device_id, 'warn');
      changed = true;
    }
  });
  if (gatewayOnline && gatewayLastSeen && now - gatewayLastSeen > onlineTimeoutMs + 10000) {
    gatewayOnline = false;
    changed = true;
  }
  if (changed) {
    broadcast({ type: 'data', devices: getDevicesList(), stats: getStats() });
    broadcast({ type: 'status', status: getStatus() });
  }
}, 5000);

// pending 命令超时清理
setInterval(function () {
  const now = Date.now();
  pending.forEach(function (p, rid) {
    if (now - p.ts > 15000) {
      pending.delete(rid);
      addEvent('command', '命令 ' + p.command + ' (' + rid + ') 超时无响应', p.deviceId, 'warn');
      broadcast({ type: 'ack', requestId: rid, deviceId: p.deviceId, command: p.command, result_code: 1, timeout: true });
    }
  });
}, 5000);

setInterval(persist, CFG.persistIntervalMs);

// ----------------------------- 启动 -------------------------------
loadPersisted();

const brokerServer = net.createServer(aedes.handle);
brokerServer.on('error', function (err) {
  if (err && err.code === 'EADDRINUSE') {
    console.error('\n[错误] MQTT broker 端口 ' + CFG.mqttPort + ' 已被占用。');
    console.error('       请先关闭占用该端口的进程（如旧的 node server.js），或设置环境变量 MQTT_PORT 换端口后重试。');
  } else {
    console.error('\n[错误] MQTT broker 启动失败:', err.message);
  }
  process.exit(1);
});
brokerServer.listen(CFG.mqttPort, function () {
  log('内嵌 MQTT broker 监听端口 ' + CFG.mqttPort + '（放行所有连接）');
  mqttClient = startMqttClient();
});

httpServer.on('error', function (err) {
  if (err && err.code === 'EADDRINUSE') {
    console.error('\n[错误] 大屏端口 ' + CFG.httpPort + ' 已被占用。');
    console.error('       请先关闭占用该端口的进程，或设置环境变量 PORT 换端口后重试。');
  } else {
    console.error('\n[错误] HTTP 服务启动失败:', err.message);
  }
  process.exit(1);
});
httpServer.listen(CFG.httpPort, function () {
  log('HTTP 服务监听端口 ' + CFG.httpPort);
  log('大屏地址: http://127.0.0.1:' + CFG.httpPort);
});

function shutdown() {
  log('收到退出信号，正在关闭...');
  persist();
  try { if (mqttClient) mqttClient.end(true); } catch (e) {}
  try { brokerServer.close(); } catch (e) {}
  try { if (wss) wss.close(); } catch (e) {}
  try { httpServer.close(); } catch (e) {}
  setTimeout(function () { process.exit(0); }, 500);
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);