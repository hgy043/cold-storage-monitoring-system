/**
 * 冷库数据监控系统 —— Node.js 跨平台网关
 *
 * 职责：
 *   1) TCP 服务端（端口 conf.tcpPort）：接收设备上行帧（register / data / ack），
 *      按《02-设备-网关TCP协议》逐行 JSON 解析，处理半包 / 粘包；
 *   2) MQTT 客户端：连接 broker，作为父设备直连，订阅命令 / 属性设置 / 消息主题；
 *   3) 定时批量上报：把 dirty 设备按《03-网关-云MQTT对接》聚合发布到云端；
 *   4) 下行命令转发：云端命令 -> TCP cmd 帧 -> 设备 ack -> 云端命令响应。
 *
 * 与 C 网关（AgentLite SDK 封装）协议完全一致，可互换。
 */

'use strict';

const net = require('net');
const fs = require('fs');
const path = require('path');
const util = require('util');
const mqtt = require('mqtt');

// ----------------------------- 配置加载 -----------------------------
const CONF_PATH = path.join(__dirname, 'conf.json');
let conf;
try {
  conf = JSON.parse(fs.readFileSync(CONF_PATH, 'utf8'));
} catch (err) {
  console.error('[网关] 读取配置失败 ' + CONF_PATH + ': ' + err.message);
  process.exit(1);
}

// ----------------------------- 常量 -----------------------------
const DEFAULT_ALARM_THRESHOLD = -20; // 未设置报警阈值前的默认值（℃）
const PENDING_TIMEOUT_MS = 30000;     // 下行命令等待设备 ack 的超时（毫秒）
const MAX_FRAME_BYTES = 4096;         // 单帧最大长度（文档 02）
const MQTT_KEEPALIVE_S = 60;          // MQTT 心跳（文档 03）

// ----------------------------- 运行状态 -----------------------------
// 设备表：node_id -> { node_id,name,interval,model,socket,online,lastSeen,lastData,dirty,seq,alarm_threshold }
const devices = new Map();
// 待回执命令表：cmd_id -> { requestId,commandName,nodeId,paras,fromPropertySet,ts }
const pending = new Map();

let reportTimer = null;  // 批量上报定时器
let offlineTimer = null; // 离线检测 / pending 清理定时器
let client = null;       // MQTT 客户端
let tcpServer = null;    // TCP 服务端
let shuttingDown = false;

// ----------------------------- 日志 -----------------------------
// 统一时间戳 [HH:MM:SS.mmm]
function ts() {
  const d = new Date();
  const p = (n, w) => String(n).padStart(w || 2, '0');
  return '[' + p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds()) +
    '.' + p(d.getMilliseconds(), 3) + ']';
}
function log() { console.log(ts() + ' ' + util.format.apply(null, arguments)); }
function warn() { console.log(ts() + ' [警告] ' + util.format.apply(null, arguments)); }

// 生成本地时区 ISO8601 时间（如 2025-01-01T12:00:00+08:00）
function formatISO(d) {
  d = d || new Date();
  const p = (n, w) => String(n).padStart(w || 2, '0');
  const off = -d.getTimezoneOffset(); // 本地相对 UTC 的分钟差，东八区为 +480
  const sign = off >= 0 ? '+' : '-';
  const abs = Math.abs(off);
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + 'T' +
    p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds()) +
    sign + p(Math.floor(abs / 60)) + ':' + p(abs % 60);
}

// ----------------------------- 设备表工具 -----------------------------
function getDevice(nodeId) {
  let e = devices.get(nodeId);
  if (!e) {
    e = {
      node_id: nodeId,
      name: '',
      interval: 5,
      model: '',
      socket: null,
      online: false,
      lastSeen: 0,
      lastData: null,
      dirty: false,
      seq: 0,
      alarm_threshold: DEFAULT_ALARM_THRESHOLD,
    };
    devices.set(nodeId, e);
  }
  return e;
}

// 云端 device_id（可能带前缀）-> 节点 node_id
function toNodeId(deviceId) {
  const prefix = conf.subDeviceIdPrefix || '';
  if (prefix && deviceId.indexOf(prefix) === 0) {
    return deviceId.slice(prefix.length);
  }
  return deviceId;
}

// 节点 node_id -> 云端 device_id
function toDeviceId(nodeId) {
  return (conf.subDeviceIdPrefix || '') + nodeId;
}

// ----------------------------- TCP 服务端 -----------------------------
function startTcpServer() {
  const server = net.createServer(onConnection);
  server.listen(conf.tcpPort, () => {
    log('TCP 服务已监听端口 %d', conf.tcpPort);
  });
  server.on('error', (err) => {
    if (err && err.code === 'EADDRINUSE') {
      console.error(ts() + ' [错误] TCP 端口 ' + conf.tcpPort + ' 已被占用，请关闭占用进程或修改 conf.json 的 tcpPort 后重试。');
    } else {
      console.error(ts() + ' TCP 服务错误: ' + err.message);
    }
    process.exit(1);
  });
  return server;
}

function onConnection(socket) {
  const conn = {
    socket: socket,
    nodeId: null,
    remote: socket.remoteAddress + ':' + socket.remotePort,
    buf: Buffer.alloc(0),
  };
  log('TCP 连接接入 %s', conn.remote);
  socket.on('data', (chunk) => onSocketData(conn, chunk));
  socket.on('error', (err) => warn('连接 %s 异常: %s', conn.remote, err.message));
  socket.on('close', () => {
    log('TCP 连接断开 %s', conn.remote);
    // 仅当该连接仍是对应节点的当前连接时才标记离线，避免旧连接关闭误伤新连接
    if (conn.nodeId) {
      const e = devices.get(conn.nodeId);
      if (e && e.socket === socket) {
        e.online = false;
        log('设备离线 node_id=%s（连接断开）', conn.nodeId);
      }
    }
  });
}

// 按 \n 切分缓冲：半包留在缓冲，粘包一次处理多帧
function onSocketData(conn, chunk) {
  conn.buf = Buffer.concat([conn.buf, chunk]);
  let idx;
  while ((idx = conn.buf.indexOf(0x0a)) !== -1) {
    const line = conn.buf.slice(0, idx);
    conn.buf = conn.buf.slice(idx + 1);
    handleLine(conn, line.toString('utf8'));
  }
  // 一直收不到换行符时缓冲会持续增长，超过上限丢弃（文档 02：超长丢弃并告警）
  if (conn.buf.length > MAX_FRAME_BYTES) {
    warn('连接 %s 缓冲超长(%dB)，丢弃', conn.remote, conn.buf.length);
    conn.buf = Buffer.alloc(0);
  }
}

function handleLine(conn, line) {
  if (!line) return;
  line = line.replace(/\r$/, ''); // 兼容 CRLF 结尾
  if (Buffer.byteLength(line, 'utf8') > MAX_FRAME_BYTES) {
    warn('连接 %s 收到超长帧(%dB)，丢弃', conn.remote, Buffer.byteLength(line, 'utf8'));
    return;
  }
  let frame;
  try {
    frame = JSON.parse(line);
  } catch (err) {
    warn('连接 %s 收到非法 JSON，忽略: %s', conn.remote, line.slice(0, 64));
    return;
  }
  if (!frame || typeof frame !== 'object') return;
  if (!frame.type) { warn('连接 %s 帧缺少 type，忽略', conn.remote); return; }
  if (!frame.node_id || typeof frame.node_id !== 'string') {
    warn('连接 %s 帧缺少 node_id，忽略', conn.remote);
    return;
  }
  // 连接注册后，后续帧的 node_id 必须与注册一致（文档 02 异常约定）
  if (conn.nodeId && conn.nodeId !== frame.node_id) {
    warn('连接 %s 的节点 %s 与已注册 %s 不一致，忽略', conn.remote, frame.node_id, conn.nodeId);
    return;
  }
  switch (frame.type) {
    case 'register': onRegister(conn, frame); break;
    case 'data': onData(conn, frame); break;
    case 'ack': onAck(conn, frame); break;
    default: warn('连接 %s 未知帧类型 %s，忽略', conn.remote, frame.type);
  }
}

// 发送一帧（单行 JSON + \n）
function sendFrame(socket, obj) {
  if (!socket || socket.destroyed) return false;
  try {
    socket.write(JSON.stringify(obj) + '\n');
    return true;
  } catch (err) {
    warn('发送帧失败: %s', err.message);
    return false;
  }
}

// 3.1 注册 register -> register_ack
function onRegister(conn, frame) {
  const nodeId = frame.node_id;
  conn.nodeId = nodeId;
  const e = getDevice(nodeId);
  // 同一节点重复上线：关闭旧连接，保留新连接
  if (e.socket && e.socket !== conn.socket && !e.socket.destroyed) {
    warn('节点 %s 重复上线，关闭旧连接', nodeId);
    e.socket.destroy();
  }
  e.name = frame.name || e.name || '';
  e.interval = Number(frame.interval) || e.interval;
  e.model = frame.model || e.model || '';
  e.socket = conn.socket;
  e.online = true;
  e.lastSeen = Date.now();
  sendFrame(conn.socket, {
    type: 'register_ack',
    node_id: nodeId,
    result: 'success',
    server_time: formatISO(),
  });
  log('设备上线 node_id=%s name="%s" interval=%ss model=%s (%s)',
    nodeId, e.name, e.interval, e.model || '-', conn.remote);
}

// 3.2 数据上报 data
function onData(conn, frame) {
  const e = getDevice(frame.node_id);
  e.online = true;      // 数据帧兼作心跳
  e.lastSeen = Date.now();
  e.lastData = {
    temperature: frame.temperature,
    humidity: frame.humidity,
    led: frame.led === undefined ? 0 : frame.led,
    beeper: frame.beeper === undefined ? 0 : frame.beeper,
    alarm: frame.alarm === undefined ? 0 : frame.alarm,
  };
  e.dirty = true;
  e.seq++;              // 版本号：上报成功后据此判断期间是否又有新数据
}

// 3.3 命令回执 ack
function onAck(conn, frame) {
  const e = getDevice(frame.node_id);
  e.online = true;
  e.lastSeen = Date.now();
  const cmdId = frame.cmd_id;
  const p = pending.get(cmdId);
  if (!p) {
    warn('收到未登记的 ack cmd_id=%s (node_id=%s)', cmdId, frame.node_id);
    return;
  }
  pending.delete(cmdId);
  const ok = frame.result === 'success';
  const detail = frame.detail || (ok ? 'success' : 'failed');
  log('命令回执 cmd_id=%s command=%s result=%s detail="%s"',
    cmdId, p.commandName, frame.result || '?', detail);

  // set_alarm_threshold 成功后更新网关侧记录的阈值
  if (p.commandName === 'set_alarm_threshold' && ok && p.paras && p.paras.value !== undefined) {
    e.alarm_threshold = p.paras.value;
  }

  // 属性设置已在 properties/set/response 即时回，这里不再重复回 commands/response
  if (p.fromPropertySet) return;

  publishCommandResponse(p.requestId, ok ? 0 : 1, p.commandName, detail);
}

// ----------------------------- MQTT 客户端 -----------------------------
const BASE = '$oc/devices/' + conf.deviceId;
const SUB_TOPICS = [
  BASE + '/sys/commands/#',
  BASE + '/sys/properties/set/#',
  BASE + '/sys/messages/down',
];
const TOPIC_REPORT = BASE + '/sys/gateway/sub-devices/properties/report';

function startMqtt() {
  client = mqtt.connect(conf.broker, {
    username: conf.deviceId,   // 文档 03：username = 网关 deviceId
    password: conf.secret,     // password = secret
    clientId: conf.deviceId,
    keepalive: MQTT_KEEPALIVE_S,
    reconnectPeriod: 3000,     // mqtt 包本身会自动重连，这里给个兜底间隔
  });
  client.on('connect', () => {
    log('MQTT 已连接 %s（deviceId=%s）', conf.broker, conf.deviceId);
    client.subscribe(SUB_TOPICS, { qos: 1 }, (err) => {
      if (err) warn('订阅主题失败: %s', err.message);
      else log('已订阅主题: %s', SUB_TOPICS.join(' , '));
    });
  });
  client.on('reconnect', () => warn('MQTT 断线，正在自动重连...'));
  client.on('close', () => warn('MQTT 连接关闭'));
  client.on('offline', () => warn('MQTT 离线'));
  client.on('error', (err) => warn('MQTT 错误: %s', err.message));
  client.on('message', (topic, payload) => onMqttMessage(topic, payload.toString('utf8')));
}

function onMqttMessage(topic, payload) {
  /* 防自回环：本网关发布的 commands/response 与 properties/set/response
   * 也会被 commands/#、properties/set/# 订阅命中，必须忽略，否则无限自回复。 */
  if (topic.indexOf(BASE + '/sys/commands/response/') === 0) return;
  if (topic.indexOf(BASE + '/sys/properties/set/response/') === 0) return;

  if (topic.indexOf(BASE + '/sys/commands/') === 0) {
    onCommand(topic, payload);
  } else if (topic.indexOf(BASE + '/sys/properties/set/') === 0) {
    onPropertiesSet(topic, payload);
  } else if (topic === BASE + '/sys/messages/down') {
    onMessageDown(payload);
  }
}

// 从主题末尾解析 request_id（request_id={xxx} 中 = 后面的段）
function parseRequestId(topic) {
  const idx = topic.lastIndexOf('=');
  return idx >= 0 ? topic.slice(idx + 1) : '';
}

// 命令下发（文档 03 §3）
function onCommand(topic, payload) {
  const requestId = parseRequestId(topic);
  let msg;
  try { msg = JSON.parse(payload); }
  catch (err) { warn('命令载荷非法 JSON，忽略: %s', payload.slice(0, 64)); return; }
  const nodeId = toNodeId(msg.object_device_id || '');
  const commandName = msg.command_name;
  const paras = msg.paras || {};
  log('收到命令 request_id=%s command=%s 目标=%s', requestId, commandName, msg.object_device_id);

  const e = devices.get(nodeId);
  if (!e || !e.online || !e.socket || e.socket.destroyed) {
    warn('节点离线或不存在，直接回失败: node_id=%s', nodeId);
    publishCommandResponse(requestId, 1, commandName, '节点离线: ' + nodeId);
    return;
  }
  // query 是协议下行查询帧（非 cmd 帧）：云端 command_name="query" 时按 docs/02 §4.2 下发
  if (commandName === 'query') {
    sendFrame(e.socket, { type: 'query', cmd_id: requestId, query: (paras && paras.query) || 'status' });
    pending.set(requestId, {
      requestId: requestId,
      commandName: 'query',
      nodeId: nodeId,
      paras: paras,
      fromPropertySet: false,
      ts: Date.now(),
    });
    log('已下发 TCP 查询帧 cmd_id=%s query=%s -> node_id=%s', requestId, (paras && paras.query) || 'status', nodeId);
    return;
  }
  // 下发 TCP cmd 帧，cmd_id 复用云端 request_id
  sendFrame(e.socket, { type: 'cmd', cmd_id: requestId, command: commandName, paras: paras });
  pending.set(requestId, {
    requestId: requestId,
    commandName: commandName,
    nodeId: nodeId,
    paras: paras,
    fromPropertySet: false,
    ts: Date.now(),
  });
  log('已下发 TCP 命令 cmd_id=%s command=%s -> node_id=%s', requestId, commandName, nodeId);
}

// 命令响应（文档 03 §3）
function publishCommandResponse(requestId, resultCode, responseName, detail) {
  const payload = JSON.stringify({
    result_code: resultCode,
    response_name: responseName,
    paras: {
      result: resultCode === 0 ? 'success' : 'failed',
      detail: detail,
    },
  });
  const topic = BASE + '/sys/commands/response/request_id=' + requestId;
  client.publish(topic, payload, { qos: 1 }, (err) => {
    if (err) warn('发布命令响应失败: %s', err.message);
    else log('已发布命令响应 request_id=%s result_code=%d detail="%s"', requestId, resultCode, detail);
  });
}

// 属性设置（文档 03 §4）：把可写属性（alarm_threshold）转成对应命令
function onPropertiesSet(topic, payload) {
  const requestId = parseRequestId(topic);
  let msg;
  try { msg = JSON.parse(payload); }
  catch (err) { warn('属性设置载荷非法 JSON，忽略'); return; }
  const nodeId = toNodeId(msg.object_device_id || '');
  const props = (msg.services && msg.services[0] && msg.services[0].properties) || {};
  log('收到属性设置 request_id=%s 目标=%s props=%s', requestId, msg.object_device_id, JSON.stringify(props));

  const e = devices.get(nodeId);
  if (!e || !e.online || !e.socket || e.socket.destroyed) {
    publishPropertiesSetResponse(requestId, 1);
    return;
  }
  for (const prop of Object.keys(props)) {
    if (prop === 'alarm_threshold') {
      const value = props[prop];
      const cmdId = 'set_' + requestId; // TCP 帧专用 cmd_id，避免与命令命名空间冲突
      sendFrame(e.socket, { type: 'cmd', cmd_id: cmdId, command: 'set_alarm_threshold', paras: { value: value } });
      pending.set(cmdId, {
        requestId: requestId,
        commandName: 'set_alarm_threshold',
        nodeId: nodeId,
        paras: { value: value },
        fromPropertySet: true,
        ts: Date.now(),
      });
      e.alarm_threshold = value; // 乐观更新，便于即时上报
      log('已转发 set_alarm_threshold value=%s -> node_id=%s (cmd_id=%s)', value, nodeId, cmdId);
    } else {
      warn('忽略不支持的属性设置: %s', prop);
    }
  }
  publishPropertiesSetResponse(requestId, 0);
}

function publishPropertiesSetResponse(requestId, resultCode) {
  const payload = JSON.stringify({
    result_code: resultCode,
    result_desc: resultCode === 0 ? 'success' : 'failed',
  });
  const topic = BASE + '/sys/properties/set/response/request_id=' + requestId;
  client.publish(topic, payload, { qos: 1 }, (err) => {
    if (err) warn('发布属性设置响应失败: %s', err.message);
    else log('已发布属性设置响应 request_id=%s result_code=%d', requestId, resultCode);
  });
}

// 平台消息下发（文档 03 §5，仅打印）
function onMessageDown(payload) {
  let content = payload;
  try { content = JSON.parse(payload).content || payload; } catch (err) { /* 非 JSON 直接打印 */ }
  log('收到平台消息: %s', content);
}

// ----------------------------- 定时批量上报 -----------------------------
function startReportTimer() {
  reportTimer = setInterval(reportDirtyDevices, conf.reportIntervalMs);
}

function reportDirtyDevices() {
  if (!client || !client.connected) return; // 断线时保留 dirty，重连后补报
  const items = [];
  devices.forEach((e, nodeId) => {
    if (e.dirty && e.online && e.lastData) {
      items.push({ nodeId: nodeId, seq: e.seq, props: buildProperties(e) });
    }
  });
  if (items.length === 0) return;
  const payload = JSON.stringify({
    devices: items.map((it) => ({
      device_id: toDeviceId(it.nodeId),
      services: [{
        service_id: 'coldstorage',
        properties: it.props,
        event_time: formatISO(),
      }],
    })),
  });
  client.publish(TOPIC_REPORT, payload, { qos: 1 }, (err) => {
    if (err) { warn('批量上报失败: %s', err.message); return; }
    let cleared = 0;
    for (const it of items) {
      const e = devices.get(it.nodeId);
      if (e && e.seq === it.seq) { e.dirty = false; cleared++; }
    }
    log('批量上报 %d 台设备（成功清 dirty %d 台）', items.length, cleared);
  });
}

function buildProperties(e) {
  return {
    temperature: e.lastData.temperature,
    humidity: e.lastData.humidity,
    led: e.lastData.led,
    beeper: e.lastData.beeper,
    alarm: e.lastData.alarm,
    alarm_threshold: e.alarm_threshold,
  };
}

// ----------------------------- 离线检测与 pending 清理 -----------------------------
function startOfflineTimer() {
  const period = Math.min(5000, Math.max(1000, Math.floor(conf.offlineTimeoutMs / 2)));
  offlineTimer = setInterval(() => {
    const now = Date.now();
    // 离线检测（文档 02 §5：offlineTimeoutMs 无数据标记离线）
    devices.forEach((e, nodeId) => {
      if (e.online && now - e.lastSeen > conf.offlineTimeoutMs) {
        e.online = false;
        log('设备离线 node_id=%s（%dms 无数据）', nodeId, conf.offlineTimeoutMs);
      }
    });
    // 清理超时未回执的 pending 命令
    pending.forEach((p, cmdId) => {
      if (now - p.ts > PENDING_TIMEOUT_MS) {
        pending.delete(cmdId);
        warn('命令回执超时，移除 pending: cmd_id=%s command=%s', cmdId, p.commandName);
      }
    });
  }, period);
}

// ----------------------------- 优雅退出 -----------------------------
function shutdown(signal) {
  if (shuttingDown) return;
  shuttingDown = true;
  log('收到 %s，开始优雅退出...', signal);
  if (reportTimer) clearInterval(reportTimer);
  if (offlineTimer) clearInterval(offlineTimer);
  if (client) { try { client.end(true); } catch (err) { /* 忽略 */ } }
  devices.forEach((e) => {
    if (e.socket && !e.socket.destroyed) e.socket.destroy();
  });
  if (tcpServer) {
    tcpServer.close(() => {
      log('TCP 服务已关闭，退出');
      process.exit(0);
    });
  }
  // 兜底：2s 后强制退出
  setTimeout(() => process.exit(0), 2000).unref();
}

// ----------------------------- 启动 -----------------------------
tcpServer = startTcpServer();
startMqtt();
startReportTimer();
startOfflineTimer();
process.on('SIGINT', () => shutdown('SIGINT'));
process.on('SIGTERM', () => shutdown('SIGTERM'));
log('冷库网关启动完成：tcpPort=%d reportInterval=%dms offlineTimeout=%dms prefix="%s"',
  conf.tcpPort, conf.reportIntervalMs, conf.offlineTimeoutMs, conf.subDeviceIdPrefix || '');
