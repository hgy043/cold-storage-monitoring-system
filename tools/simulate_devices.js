"use strict";
const net = require("net");

// ================= 工具函数 =================
function pad2(n) { return String(n).padStart(2, "0"); }
function randInt(min, max) { return Math.floor(Math.random() * (max - min + 1)) + min; }
function clamp(v, min, max) { return Math.min(max, Math.max(min, v)); }
function round1(v) { return Math.round(v * 10) / 10; }
function hhmmss(d) { return pad2(d.getHours()) + ":" + pad2(d.getMinutes()) + ":" + pad2(d.getSeconds()); }

function isoWithOffset(d) {
  const off = -d.getTimezoneOffset();
  const sign = off >= 0 ? "+" : "-";
  const abs = Math.abs(off);
  const oh = pad2(Math.floor(abs / 60));
  const om = pad2(abs % 60);
  return d.getFullYear() + "-" + pad2(d.getMonth() + 1) + "-" + pad2(d.getDate()) +
    "T" + pad2(d.getHours()) + ":" + pad2(d.getMinutes()) + ":" + pad2(d.getSeconds()) +
    sign + oh + ":" + om;
}

// 一帧一行的换行符（协议规定单个 \n）
const NL = String.fromCharCode(10);

// ================= 命令行参数 =================
function parseArgs(argv) {
  const args = { count: 16, host: "127.0.0.1", port: 18830, interval: 5, help: false };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--count") args.count = parseInt(argv[++i], 10);
    else if (a === "--host") args.host = argv[++i];
    else if (a === "--port") args.port = parseInt(argv[++i], 10);
    else if (a === "--interval") args.interval = parseInt(argv[++i], 10);
    else if (a === "--help") args.help = true;
  }
  return args;
}

function usage() {
  console.log("用法: node tools/simulate_devices.js [选项]");
  console.log("  --count <N>      冷库数量，默认 16（节点数 = N*2）");
  console.log("  --host <ip>      网关地址，默认 127.0.0.1");
  console.log("  --port <p>       网关端口，默认 18830");
  console.log("  --interval <s>   上报周期秒，默认 5");
  console.log("  --help           显示帮助");
}

// ================= 单个设备节点 =================
class DeviceNode {
  constructor(opts) {
    this.nodeId = opts.nodeId;
    this.name = opts.name;
    this.host = opts.host;
    this.port = opts.port;
    this.interval = opts.interval;

    this.socket = null;
    this.buf = "";
    this.registered = false;
    this.dataTimer = null;
    this.beeperTimer = null;
    this.reconnectTimer = null;
    this.reconnectAttempts = 0;
    this.closed = false;

    // 传感器状态
    this.TEMP_BASE = -22;        // 温度基准 -22℃（低于默认阈值 -20，仅在尖峰时告警）
    this.temperature = this.TEMP_BASE;
    this.humidity = 60;
    this.led = 0;
    this.beeper = 0;
    this.alarmThreshold = -20;   // 初始报警阈值
    this.alarm = 0;

    // 温度尖峰：每 60~120 步随机一次
    this.step = 0;
    this.nextSpike = randInt(60, 120);
  }

  log(msg) {
    console.log("[" + hhmmss(new Date()) + "] [" + this.nodeId + "] " + msg);
  }

  connect() {
    if (this.closed) return;
    this.log("连接网关 " + this.host + ":" + this.port + " ...");
    const sock = net.createConnection({ host: this.host, port: this.port });
    this.socket = sock;
    sock.setEncoding("utf8");
    sock.setNoDelay(true);

    sock.on("connect", () => {
      this.reconnectAttempts = 0;
      this.registered = false;
      this.buf = "";
      this.log("TCP 已连接，发送 register");
      this.sendRegister();
    });

    sock.on("data", (chunk) => this.onData(chunk));
    sock.on("error", (err) => this.log("连接错误: " + err.message));
    sock.on("close", () => this.onClose());
  }

  onData(chunk) {
    this.buf += chunk;
    let idx;
    while ((idx = this.buf.indexOf(NL)) >= 0) {
      const line = this.buf.slice(0, idx).trim();
      this.buf = this.buf.slice(idx + 1);
      if (line) this.handleFrame(line);
    }
  }

  onClose() {
    if (this.closed) return;
    this.clearDataTimer();
    this.clearBeeperTimer();
    this.registered = false;
    if (this.socket) {
      this.socket.removeAllListeners();
      this.socket = null;
    }
    this.scheduleReconnect();
  }

  scheduleReconnect() {
    const delay = Math.min(30, Math.pow(2, this.reconnectAttempts)); // 1,2,4,8,16,30
    this.reconnectAttempts++;
    this.log("连接断开，" + delay + "s 后重连（第 " + this.reconnectAttempts + " 次）");
    this.clearReconnectTimer();
    this.reconnectTimer = setTimeout(() => this.connect(), delay * 1000);
  }

  send(obj) {
    if (!this.socket || this.socket.destroyed) return;
    this.socket.write(JSON.stringify(obj) + NL);
  }

  sendRegister() {
    this.send({
      type: "register",
      node_id: this.nodeId,
      name: this.name,
      interval: this.interval,
      model: "gec6818-sim"
    });
    this.log("已发送 register");
  }

  handleFrame(line) {
    this.log("收到下行帧: " + line);
    let frame;
    try {
      frame = JSON.parse(line);
    } catch (e) {
      this.log("忽略：非合法 JSON");
      return;
    }
    if (!frame || typeof frame !== "object" || !frame.type) {
      this.log("忽略：缺 type");
      return;
    }
    if (frame.node_id && frame.node_id !== this.nodeId) {
      this.log("忽略：node_id 不匹配 (" + frame.node_id + ")");
      return;
    }
    switch (frame.type) {
      case "register_ack": this.handleRegisterAck(frame); break;
      case "cmd": this.handleCmd(frame); break;
      case "query": this.handleQuery(frame); break;
      default: this.log("未知帧类型: " + frame.type);
    }
  }

  handleRegisterAck(frame) {
    if (frame.result === "success") {
      this.registered = true;
      this.log("注册成功，开始上报（周期 " + this.interval + "s）");
      this.sendData();       // 立即上报一次
      this.scheduleData();   // 之后按周期
    } else {
      this.log("注册失败: " + JSON.stringify(frame));
    }
  }

  scheduleData() {
    this.clearDataTimer();
    this.dataTimer = setTimeout(() => {
      this.sendData();
      this.scheduleData();
    }, this.interval * 1000);
  }

  updateSensor() {
    this.step++;
    // 温度随机游走 ±0.3/步 + 向基准弱回归（3%/步，长期挂机不漂移）
    this.temperature += (Math.random() - 0.5) * 0.6 + (this.TEMP_BASE - this.temperature) * 0.03;
    // 每 60~120 步随机一次尖峰（-8~5℃，模拟故障/开门）
    if (this.step >= this.nextSpike) {
      this.temperature = randInt(-8, 5);
      this.log("温度尖峰模拟（故障/开门）: " + this.temperature + "℃");
      this.nextSpike = this.step + randInt(60, 120);
    }
    // 湿度随机游走，限制 55~75
    this.humidity = clamp(this.humidity + (Math.random() - 0.5) * 2, 55, 75);
    // 偶尔随机切换 LED
    if (Math.random() < 0.03) {
      this.led = this.led ? 0 : 1;
      this.log("LED 状态随机变化 -> " + this.led);
    }
    // 偶尔随机切换蜂鸣器
    if (Math.random() < 0.02) {
      this.beeper = this.beeper ? 0 : 1;
      this.log("蜂鸣器状态随机变化 -> " + this.beeper);
      if (this.beeper) this.scheduleBeeperOff(3000);
    }
    // 本地报警判定
    this.alarm = this.temperature > this.alarmThreshold ? 1 : 0;
  }

  sendData() {
    this.updateSensor();
    const frame = {
      type: "data",
      node_id: this.nodeId,
      ts: isoWithOffset(new Date()),
      temperature: round1(this.temperature),
      humidity: round1(this.humidity),
      led: this.led,
      beeper: this.beeper,
      alarm: this.alarm
    };
    this.send(frame);
    this.log("上报 data: temp=" + frame.temperature + "℃ hum=" + frame.humidity +
      "% led=" + frame.led + " beeper=" + frame.beeper + " alarm=" + frame.alarm);
  }

  handleCmd(frame) {
    const cmdId = frame.cmd_id || "";
    const command = frame.command;
    const paras = frame.paras || {};
    let result = "success";
    let detail = "";

    switch (command) {
      case "set_led": {
        const on = (paras.on === 1 || paras.on === true || paras.on === "1") ? 1 : 0;
        this.led = on;
        detail = on ? "LED on" : "LED off";
        break;
      }
      case "set_beeper": {
        const on = (paras.on === 1 || paras.on === true || paras.on === "1") ? 1 : 0;
        if (on) {
          this.beeper = 1;
          const ms = Number(paras.duration_ms);
          const dur = Number.isFinite(ms) && ms > 0 ? Math.floor(ms) : 3000;
          this.scheduleBeeperOff(dur);
          detail = "Beeper on (" + dur + "ms)";
        } else {
          this.beeper = 0;
          this.clearBeeperTimer();
          detail = "Beeper off";
        }
        break;
      }
      case "set_interval": {
        const sec = parseInt(paras.seconds, 10);
        if (Number.isFinite(sec) && sec >= 1 && sec <= 600) {
          this.interval = sec;
          this.log("上报周期修改为 " + sec + "s");
          if (this.registered) this.scheduleData();
          detail = "interval=" + sec + "s";
        } else {
          result = "failed";
          detail = "无效周期: " + paras.seconds;
        }
        break;
      }
      case "set_alarm_threshold": {
        const v = Number(paras.value);
        if (Number.isFinite(v)) {
          this.alarmThreshold = v;
          this.alarm = this.temperature > v ? 1 : 0;
          detail = "threshold=" + v + "℃";
        } else {
          result = "failed";
          detail = "无效阈值: " + paras.value;
        }
        break;
      }
      default:
        result = "failed";
        detail = "未知命令: " + command;
    }

    this.send({ type: "ack", node_id: this.nodeId, cmd_id: cmdId, result: result, detail: detail });
    this.log("回执 ack: cmd_id=" + cmdId + " result=" + result + " detail=" + detail);
  }

  handleQuery(frame) {
    this.log("收到查询: " + frame.query + "，立即补发 data");
    this.sendData();
    this.send({ type: "ack", node_id: this.nodeId, cmd_id: frame.cmd_id || "", result: "success", detail: "status" });
    this.log("回执 ack: cmd_id=" + (frame.cmd_id || "") + " result=success detail=status");
  }

  scheduleBeeperOff(ms) {
    this.clearBeeperTimer();
    this.beeperTimer = setTimeout(() => {
      this.beeper = 0;
      this.beeperTimer = null;
      this.log("蜂鸣器自动关闭（" + ms + "ms 到）");
    }, ms);
  }

  clearDataTimer() { if (this.dataTimer) { clearTimeout(this.dataTimer); this.dataTimer = null; } }
  clearBeeperTimer() { if (this.beeperTimer) { clearTimeout(this.beeperTimer); this.beeperTimer = null; } }
  clearReconnectTimer() { if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null; } }

  close() {
    this.closed = true;
    this.clearDataTimer();
    this.clearBeeperTimer();
    this.clearReconnectTimer();
    if (this.socket) {
      this.socket.removeAllListeners();
      this.socket.destroy();
      this.socket = null;
    }
  }
}

// ================= 主流程 =================
function main() {
  const args = parseArgs(process.argv);
  if (args.help) { usage(); return; }

  const count = args.count > 0 ? args.count : 16;
  const interval = args.interval >= 1 ? args.interval : 5;

  const nodes = [];
  for (let n = 1; n <= count; n++) {
    const pad = pad2(n);
    nodes.push(new DeviceNode({
      nodeId: "cold" + pad + "_front",
      name: n + "号冷库-前门",
      host: args.host, port: args.port, interval: interval
    }));
    nodes.push(new DeviceNode({
      nodeId: "cold" + pad + "_back",
      name: n + "号冷库-后门",
      host: args.host, port: args.port, interval: interval
    }));
  }

  console.log("[" + hhmmss(new Date()) + "] 模拟设备启动: " + count + " 个冷库 x 前/后 = " +
    nodes.length + " 个节点 -> " + args.host + ":" + args.port + "，上报周期 " + interval + "s");

  for (const node of nodes) node.connect();

  const shutdown = () => {
    console.log(NL + "[" + hhmmss(new Date()) + "] 收到退出信号，正在关闭全部节点...");
    for (const node of nodes) node.close();
    setTimeout(() => process.exit(0), 300);
  };
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}

main();