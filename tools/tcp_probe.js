"use strict";
const net = require("net");

const NL = String.fromCharCode(10);

function pad2(n) { return String(n).padStart(2, "0"); }
function hhmmss(d) { return pad2(d.getHours()) + ":" + pad2(d.getMinutes()) + ":" + pad2(d.getSeconds()); }

function parseArgs(argv) {
  const args = { host: "127.0.0.1", port: 18830, node: "cold99_probe", send: null, help: false };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--host") args.host = argv[++i];
    else if (a === "--port") args.port = parseInt(argv[++i], 10);
    else if (a === "--node") args.node = argv[++i];
    else if (a === "--send") args.send = argv[++i];
    else if (a === "--help") args.help = true;
  }
  return args;
}

function usage() {
  console.log("TCP 调试工具（仅 net 模块）—— 模拟一个设备节点接入网关");
  console.log("用法: node tools/tcp_probe.js [选项]");
  console.log("  --host <ip>        目标主机，默认 127.0.0.1");
  console.log("  --port <p>         目标端口，默认 18830");
  console.log("  --node <id>        注册用的节点号，默认 cold99_probe");
  console.log("  --send '<json>'    连接并注册后发送一帧 JSON（缺 node_id 会自动补上），然后继续接收");
  console.log("  --help             显示帮助");
  console.log("示例:");
  console.log('  node tools/tcp_probe.js');
  console.log('  node tools/tcp_probe.js --send {"type":"data","ts":"2026-01-01T00:00:00+08:00","temperature":-18.5,"humidity":60,"led":0,"beeper":0,"alarm":0}');
  console.log("说明: 上行帧 register/data/ack 中只有 register 会收到网关的 register_ack 回复；");
  console.log("      data/ack 不直接回帧（数据进入批量上报），下行 cmd/query 由云端指令触发，");
  console.log("      可在网页大屏点按钮后在此观察设备收到的命令帧。");
  console.log("收到每帧会带时间戳打印；Ctrl+C 退出");
}

function main() {
  const args = parseArgs(process.argv);
  if (args.help) { usage(); return; }

  const sock = net.createConnection({ host: args.host, port: args.port });
  sock.setEncoding("utf8");
  sock.setNoDelay(true);
  let buf = "";

  sock.on("connect", () => {
    console.log("[" + hhmmss(new Date()) + "] 已连接 " + args.host + ":" + args.port +
      " (" + sock.localAddress + ":" + sock.localPort + " -> " + sock.remoteAddress + ":" + sock.remotePort + ")");
    /* 按协议（docs/02）先注册，网关才会接受后续帧 */
    sock.write(JSON.stringify({ type: "register", node_id: args.node, name: "TCP 调试探针", interval: 5, model: "probe" }) + NL);
    console.log("[" + hhmmss(new Date()) + "] 已发送 register (node_id=" + args.node + ")");
    if (args.send) {
      /* 缺 node_id 的帧自动补上当前节点号，避免被网关按协议忽略 */
      let frame;
      try { frame = JSON.parse(args.send); } catch (e) { frame = null; }
      if (frame && typeof frame === "object" && !frame.node_id) {
        frame.node_id = args.node;
        args.send = JSON.stringify(frame);
      }
      sock.write(args.send.trim() + NL);
      console.log("[" + hhmmss(new Date()) + "] 已发送: " + args.send);
    }
  });

  sock.on("data", (chunk) => {
    buf += chunk;
    let idx;
    while ((idx = buf.indexOf(NL)) >= 0) {
      const line = buf.slice(0, idx).trim();
      buf = buf.slice(idx + 1);
      if (line) console.log("[" + hhmmss(new Date()) + "] < " + line);
    }
  });

  sock.on("error", (err) => {
    console.error("[" + hhmmss(new Date()) + "] 错误: " + err.message);
    process.exit(1);
  });

  sock.on("close", () => {
    console.log("[" + hhmmss(new Date()) + "] 连接已关闭");
    process.exit(0);
  });

  process.on("SIGINT", () => {
    console.log(NL + "[" + hhmmss(new Date()) + "] Ctrl+C，退出");
    sock.destroy();
    setTimeout(() => process.exit(0), 200);
  });
}

main();