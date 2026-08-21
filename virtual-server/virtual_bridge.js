/**
 * virtual_bridge.js - 冷库虚拟数据转发程序（Ubuntu）
 *
 * 角色：数据源 + 转发服务器
 *   - 每 dataIntervalMs(默认3s) 生成一次虚拟数据（4 温区围绕设定值随机游走）
 *     → 上报华为云 IoTDA（设备 yihao，属性上报）
 *     → 经 TCP 推送给 gec6818 终端（virt_data 帧，见 docs/02 扩展）
 *   - 每 alarmEveryMs(默认30s) 制造一次报警数据（某温区温度越限，status=2, alarm=1）
 *   - 接收终端 confirm 帧（巡查人员确认报警）→ 记录并可选上报云端
 *   - 接收云端命令（set_beeper/set_led/set_alarm_threshold/confirm_alarm）→ 转发终端 cmd 帧
 *
 * 运行：npm install && node virtual_bridge.js
 * 配置：conf.json（华为云连接、周期、温区设定；localBroker 用于本地演示）
 */
'use strict';
const net = require('net');
const fs = require('fs');
const path = require('path');
const mqtt = require('mqtt');

const conf = JSON.parse(fs.readFileSync(path.join(__dirname, 'conf.json'), 'utf8'));
const NL = String.fromCharCode(10);
const LOG_FILE = path.join(__dirname, 'forwarder.log');   /* 追加日志，便于跨端排查 */

/* ---------- 运行状态 ---------- */
const zones = conf.zones.map(z => ({ zone: z.zone, name: z.name, setpoint: z.setpoint, temp: z.setpoint, hum: 60 }));
let outdoor = 30;
let alarmActive = false;      // 当前是否有未确认报警
let alarmZone = null;
let lastAlarmAt = Date.now();   /* 启动后满 alarmEveryMs 才首次报警 */
let socket = null;           // 终端 TCP 连接
let buf = '';
let tick = 0;

/* ---------- 工具 ---------- */
function ts() { const d = new Date(); const p = n => String(n).padStart(2, '0'); return '[' + p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds()) + ']'; }
/* 带时间戳日志：控制台 + 追加写入 forwarder.log（便于跨端排查） */
function log() {
  const line = ts() + ' ' + Array.from(arguments).join(' ');
  console.log(line);
  try { fs.appendFileSync(LOG_FILE, line + '\n'); } catch (e) {}
}
/* 本地时间 "YYYY-MM-DD HH:MM:SS"（终端据此自动校时；勿用 toISOString 的 UTC） */
function iso() {
    const d = new Date();
    const p = n => String(n).padStart(2, '0');
    return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) +
           ' ' + p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
}
/* [0,1) 随机数（Math.random 别名） */
function frand() { return Math.random(); }
/* 保留 1 位小数 */
function round1(v) { return Math.round(v * 10) / 10; }

/* 每 3s 步进一次虚拟温区（随机游走 + 向设定回归） */
function stepZones() {
  zones.forEach(z => {
    z.temp += (frand() - 0.5) * 0.6 + (z.setpoint - z.temp) * 0.05;
    /* 湿度按温区差异化（真实冷库：冷冻区低、冷藏/预冷高、穿堂适中） */
    const hum_base = [52, 72, 78, 66];   /* 冷冻/冷藏/预冷/穿堂 */
    z.hum = hum_base[z.zone === 'frozen' ? 0 : z.zone === 'chilled' ? 1 : z.zone === 'precool' ? 2 : 3]
            + (frand() - 0.5) * 10;
  });
  outdoor = 28 + frand() * 6;
  /* 每 alarmEveryMs 制造一次报警：随机选一个温区，温度大幅越限并持续 */
  if (!alarmActive && Date.now() - lastAlarmAt > conf.alarmEveryMs) {
    alarmActive = true;
    alarmZone = zones[Math.floor(frand() * zones.length)];
    alarmZone.temp = alarmZone.setpoint + 8 + frand() * 6;   /* 越限 8~14℃ */
    log('制造报警: ' + alarmZone.name + ' 温度 ' + round1(alarmZone.temp) + '℃');
  }
  if (alarmActive && alarmZone) {
    alarmZone.temp = alarmZone.setpoint + 8 + frand() * 4;   /* 保持越限 */
  }
}

/* 温区状态：偏差>4 报警(2)、>2 预警(1) */
function zoneStatus(z) {
  const dev = z.temp - z.setpoint;
  return dev > 4 ? 2 : (dev > 2 ? 1 : 0);
}

/* ---------- 数据帧构造 ---------- */
/* 设备上报属性（华为云 IoTDA，service_id=coldstorage） */
function buildReportPayload() {
  const main = zones[0];   /* 冷冻区为代表属性 */
  return JSON.stringify({ services: [{ service_id: 'coldstorage', properties: {
    temperature: round1(main.temp),
    humidity: Math.round(main.hum),
    led: 0,
    beeper: alarmActive ? 1 : 0,
    alarm: alarmActive ? 1 : 0,
    alarm_threshold: Math.round(main.setpoint),
  } }] });
}

/* 终端 virt_data 帧（docs/02 扩展：云端虚拟数据下发，扁平字段便于设备端手写解析）
 * 字段：z1_t..z4_t 温区温度, z1_h..z4_h 湿度, z1_s..z4_s 设定, z1_st..z4_st 状态(0/1/2),
 *       outdoor 室外温度, alarm 报警标志, alarm_threshold 阈值 */
function buildVirtData() {
  const f = { type: 'virt_data', ts: iso(), outdoor: round1(outdoor), alarm: alarmActive ? 1 : 0, alarm_threshold: Math.round(zones[0].setpoint) };
  zones.forEach((z, i) => {
    f['z' + (i + 1) + '_t'] = round1(z.temp);
    f['z' + (i + 1) + '_h'] = Math.round(z.hum);
    f['z' + (i + 1) + '_s'] = z.setpoint;
    f['z' + (i + 1) + '_st'] = zoneStatus(z);
  });
  return JSON.stringify(f);
}

/* 经 TCP 推送一帧给 gec6818 终端（连接存在时） */
function sendToDevice(frame) {
  if (socket && !socket.destroyed) { socket.write(frame + NL); }
}

/* ---------- 定时：每 dataIntervalMs 生成数据并推送 ---------- */
let dataTimer = null;
let dataIntervalMs = conf.dataIntervalMs || 3000;   /* 云端 set_interval 可动态调整 */
/* 数据主循环（周期可由云端 set_interval 动态调整）：步进温区 → 上报华为云/本地 broker → 推送终端 */
function dataTick() {
  tick++;
  stepZones();
  const payload = buildReportPayload();
  const virt = buildVirtData();
  /* 1) 上报华为云（或本地演示 broker） */
  if (mqttClient && mqttClient.connected) {
    mqttClient.publish('$oc/devices/' + conf.huaweiCloud.deviceId + '/sys/properties/report', payload, { qos: 1 });
  }
  /* 1b) 本地 broker 同步发布（web 大屏显示数据，与华为云同载荷） */
  if (localClient && localClient.connected) {
    localClient.publish('$oc/devices/' + conf.huaweiCloud.deviceId + '/sys/properties/report', payload, { qos: 1 });
  }
  /* 2) 推送给 gec6818 终端 */
  sendToDevice(virt);
  if (tick % 10 === 0) log('推送虚拟数据, 报警=' + (alarmActive ? 1 : 0));
}
dataTimer = setInterval(dataTick, dataIntervalMs);

/* ---------- 华为云 / 本地 broker 连接 ---------- */
let mqttClient = null;       // 华为云连接
let localClient = null;      // 本地 aedes broker 连接（web 大屏命令入口）
const cmdSource = {};        // request_id -> 'hc'(华为云) | 'local'(web 大屏)，ack 按来源回执
let cmdSourceCount = 0;      // 防泄漏：超 200 条未回执清空
/* 连接华为云 IoTDA：MQTT 鉴权（CONNECT-KEY）+ 订阅命令/属性设置主题 */
function connectMqtt() {
  const hc = conf.huaweiCloud;
  const url = (hc.enabled && hc.host) ? ((hc.tls ? 'mqtts://' : 'mqtt://') + hc.host + ':' + (hc.port || 8883)) : conf.localBroker;
  mqttClient = mqtt.connect(url, {
    username: hc.deviceId,
    password: hc.secret,
    clientId: hc.clientId || (hc.deviceId + '_0_0_' + Date.now()),   /* 优先用平台生成的 clientId（CONNECT-KEY） */
    reconnectPeriod: 3000,
    rejectUnauthorized: false,   /* 华为云根证书自签场景放宽（生产可配 CA） */
  });
  mqttClient.on('connect', () => {
    log('MQTT 已连接 ' + url + ' deviceId=' + hc.deviceId);
    /* 订阅云端命令 */
    mqttClient.subscribe('$oc/devices/' + hc.deviceId + '/sys/commands/#');
    mqttClient.subscribe('$oc/devices/' + hc.deviceId + '/sys/properties/set/#');
  });
  mqttClient.on('reconnect', () => log('MQTT 断线重连...'));
  mqttClient.on('error', e => log('MQTT 错误: ' + e.message));
  mqttClient.on('message', (topic, payload) => onCloudMessage(topic, payload.toString(), 'hc'));
  /* 本地 broker：web 大屏命令入口（topic 用通配符覆盖 demo_gateway_01 等任意网关前缀） */
  connectLocalMqtt();
}

/* 云端命令 → 终端 cmd 帧；属性设置 → set_alarm_threshold 等 */
function onCloudMessage(topic, payload, src) {
  const broker = (src === 'local') ? localClient : mqttClient;   /* 命令/响应按来源路由 */
  if (topic.includes('/sys/commands/response/') || topic.includes('/sys/properties/set/response/')) return;
  if (topic.includes('/sys/commands/')) {
    const rid = topic.slice(topic.lastIndexOf('=') + 1);
    let msg; try { msg = JSON.parse(payload); } catch (e) { return; }
    const cmd = msg.command_name;
    const paras = msg.paras || {};
    log('云端命令: ' + cmd + ' (rid=' + rid + ')' + (src === 'local' ? ' [web大屏]' : ''));
    cmdSource[rid] = src;
    if (++cmdSourceCount > 200) { cmdSourceCount = 0; Object.keys(cmdSource).forEach(k => delete cmdSource[k]); }
    /* 数据源由转发程序管理：阈值/上报周期在转发程序侧落地（终端显示经 virt_data 帧自动更新） */
    if (cmd === 'set_alarm_threshold' && paras.value !== undefined) {
      zones[0].setpoint = Number(paras.value);
      log('云端改报警阈值: ' + zones[0].setpoint + '℃');
    }
    if (cmd === 'set_interval' && paras.seconds !== undefined) {
      const sec = Math.max(1, Math.min(600, Number(paras.seconds)));
      dataIntervalMs = sec * 1000;
      clearInterval(dataTimer);
      dataTimer = setInterval(dataTick, dataIntervalMs);
      log('云端改上报周期: ' + sec + 's');
    }
    sendToDevice(JSON.stringify({ type: 'cmd', cmd_id: rid, command: cmd, paras: paras }));
  } else if (topic.includes('/sys/properties/set/')) {
    const rid = topic.slice(topic.lastIndexOf('=') + 1);
    let msg; try { msg = JSON.parse(payload); } catch (e) { return; }
    const props = (msg.services && msg.services[0] && msg.services[0].properties) || {};
    if (props.alarm_threshold !== undefined) {
      zones[0].setpoint = Number(props.alarm_threshold);
      sendToDevice(JSON.stringify({ type: 'cmd', cmd_id: 'set_' + rid, command: 'set_alarm_threshold', paras: { value: props.alarm_threshold } }));
    }
    if (broker) broker.publish('$oc/devices/' + conf.huaweiCloud.deviceId + '/sys/properties/set/response/request_id=' + rid, JSON.stringify({ result_code: 0, result_desc: 'success' }));
  }
}

/* ---------- TCP 服务端（gec6818 终端接入） ---------- */
const server = net.createServer(s => {
  log('终端接入 ' + s.remoteAddress + ':' + s.remotePort);
  socket = s;
  s.setEncoding('utf8');
  s.on('data', chunk => {
    buf += chunk;
    let i;
    while ((i = buf.indexOf(NL)) >= 0) {
      const line = buf.slice(0, i).trim();
      buf = buf.slice(i + 1);
      if (line) handleDeviceFrame(line);
    }
  });
  s.on('close', () => { log('终端断开'); if (socket === s) socket = null; });
  s.on('error', () => {});
});

/* 终端上行帧：register / confirm / ack */
function handleDeviceFrame(line) {
  log('上行帧: ' + line);
  let f; try { f = JSON.parse(line); } catch (e) { return; }
  if (f.type === 'register') {
    log('终端注册: ' + f.node_id);
    socket.write(JSON.stringify({ type: 'register_ack', node_id: f.node_id, result: 'success', server_time: iso() }) + NL);
  } else if (f.type === 'confirm') {
    /* 巡查人员确认报警：取消报警状态，蜂鸣停止（终端已执行），记录并可选上报云 */
    log('收到报警确认: ' + JSON.stringify(f));
    alarmActive = false;
    alarmZone = null;
    lastAlarmAt = Date.now();
  } else if (f.type === 'ack') {
    /* 终端命令回执 → 按命令来源回云端/web 命令响应 */
    const rid = f.cmd_id && f.cmd_id.startsWith('set_') ? f.cmd_id.slice(4) : f.cmd_id;
    const src = cmdSource[rid] || 'hc';
    const broker = (src === 'local') ? localClient : mqttClient;
    delete cmdSource[rid];
    if (rid && broker && broker.connected) {
      const ok = f.result === 'success';
      broker.publish('$oc/devices/' + conf.huaweiCloud.deviceId + '/sys/commands/response/request_id=' + rid,
        JSON.stringify({ result_code: ok ? 0 : 1, response_name: f.command || 'cmd', paras: { result: ok ? 'success' : 'failed', detail: f.detail || '' } }));
    }
  }
}

/* 本地 aedes broker 连接：接收 web 大屏下发的命令（命令响应按来源回执） */
function connectLocalMqtt() {
  if (!conf.localBroker) { log('未配置 localBroker，跳过本地连接'); return; }
  localClient = mqtt.connect(conf.localBroker, { clientId: 'virtbridge-local-' + Date.now() });
  localClient.on('connect', () => {
    log('本地 broker 已连接 ' + conf.localBroker);
    localClient.subscribe('$oc/devices/+/sys/commands/#');
    localClient.subscribe('$oc/devices/+/sys/properties/set/#');
  });
  localClient.on('message', (topic, payload) => onCloudMessage(topic, payload.toString(), 'local'));
  localClient.on('reconnect', () => log('本地 broker 重连...'));
  localClient.on('error', e => log('本地 broker 错误: ' + e.message));
}

server.listen(conf.tcpPort, () => log('TCP 服务监听 ' + conf.tcpPort + '（等待 gec6818 终端接入）'));
connectMqtt();
log('虚拟数据转发程序启动：数据周期 ' + (conf.dataIntervalMs / 1000) + 's，报警周期 ' + (conf.alarmEveryMs / 1000) + 's');

process.on('SIGINT', () => { log('退出'); try { mqttClient && mqttClient.end(true); } catch (e) {} server.close(); process.exit(0); });
process.on('SIGTERM', () => process.exit(0));