(function () {
  'use strict';

  // ---------- 工具 ----------
  function pad2(n) { return (n < 10 ? '0' : '') + n; }
  function devId(i, pos) { return 'cold' + pad2(i) + '_' + pos; }
  function idxOf(id) { var m = /cold(\d+)_/.exec(id); return m ? parseInt(m[1], 10) : 999; }
  function fmtTime(ts) {
    var d = ts ? new Date(ts) : new Date();
    return pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds());
  }
  function fmtNum(v) { return (v === null || v === undefined || isNaN(v)) ? null : (Math.round(v * 10) / 10); }
  function escapeHtml(s) { return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); }
  function el(id) { return document.getElementById(id); }

  // ---------- 状态 ----------
  var devicesById = {};
  var statsData = { coldStorages: [] };
  var selectedDevice = null;
  var cardRefs = {};
  var lineChart = null;
  var barChart = null;
  var wsRetry = 1000;

  // ---------- 卡片 ----------
  function nodeHtml(i, pos, label) {
    return '<div class="node"><div class="node-label">' + label + '</div>'
      + '<div class="temp" id="t-' + pos + '-' + i + '">--</div>'
      + '<div class="hum" id="h-' + pos + '-' + i + '">--</div></div>';
  }

  function cardHtml(i) {
    return '<div class="card" id="card-' + i + '">'
      + '<div class="card-head"><span class="card-name">' + i + '号冷库</span>'
      + '<span class="badge" id="badge-' + i + '">告警</span></div>'
      + '<div class="card-body">'
      + '<div class="node-row">' + nodeHtml(i, 'front', '前门') + nodeHtml(i, 'back', '后门') + '</div>'
      + '<div class="status-row">'
      + '<span class="lbl">LED</span><span class="light off" id="led-front-' + i + '"></span><span class="light off" id="led-back-' + i + '"></span>'
      + '<span class="lbl">蜂鸣</span><span class="light off" id="beep-front-' + i + '"></span><span class="light off" id="beep-back-' + i + '"></span>'
      + '<span class="lbl">阈值</span><span class="th-val" id="th-' + i + '">-20℃</span>'
      + '</div>'
      + '<div class="ctrl-row">'
      + '<select class="node-select" id="sel-' + i + '"><option value="front">前门</option><option value="back">后门</option></select>'
      + '<button data-act="led-on" data-idx="' + i + '">LED开</button>'
      + '<button data-act="led-off" data-idx="' + i + '">LED关</button>'
      + '<button data-act="beep-on" data-idx="' + i + '">蜂鸣开</button>'
      + '<button data-act="beep-off" data-idx="' + i + '">蜂鸣关</button>'
      + '</div>'
      + '<div class="th-row">'
      + '<input type="number" class="th-input" id="in-' + i + '" value="-20" step="0.5">'
      + '<button data-act="th-set" data-idx="' + i + '">设置阈值</button>'
      + '</div>'
      + '</div></div>';
  }

  function buildCards() {
    var grid = el('cardGrid');
    var html = '';
    for (var i = 1; i <= 16; i++) html += cardHtml(i);
    grid.innerHTML = html;
    for (var j = 1; j <= 1; j++) {
      cardRefs[j] = {
        card: el('card-' + j),
        badge: el('badge-' + j),
        front: { t: el('t-front-' + j), h: el('h-front-' + j), led: el('led-front-' + j), beep: el('beep-front-' + j) },
        back: { t: el('t-back-' + j), h: el('h-back-' + j), led: el('led-back-' + j), beep: el('beep-back-' + j) },
        th: el('th-' + j),
        sel: el('sel-' + j),
        input: el('in-' + j),
      };
    }
    grid.addEventListener('click', onCardClick);
  }

  function setLight(span, on, kind) {
    span.className = 'light ' + (on ? ('on ' + kind) : 'off');
  }

  function setNode(node, d) {
    if (d && d.latest) {
      var t = fmtNum(d.latest.temperature);
      var h = fmtNum(d.latest.humidity);
      node.t.textContent = (t === null) ? '--' : (t + '℃');
      node.h.textContent = (h === null) ? '--' : (h + '%');
      setLight(node.led, d.latest.led === 1, 'led');
      setLight(node.beep, d.latest.beeper === 1, 'beep');
    } else {
      node.t.textContent = '--';
      node.h.textContent = '--';
      setLight(node.led, false, 'led');
      setLight(node.beep, false, 'beep');
    }
  }

  function updateCards() {
    for (var i = 1; i <= 16; i++) {
      var ref = cardRefs[i];
      if (!ref) continue;
      var f = devicesById[devId(i, 'front')];
      var b = devicesById[devId(i, 'back')];
      setNode(ref.front, f);
      setNode(ref.back, b);
      var th = -20;
      if (f && f.latest && typeof f.latest.alarm_threshold === 'number') th = f.latest.alarm_threshold;
      else if (b && b.latest && typeof b.latest.alarm_threshold === 'number') th = b.latest.alarm_threshold;
      ref.th.textContent = th + '℃';
      if (ref.input && document.activeElement !== ref.input) ref.input.value = th;
      var alarm = (f && f.latest && typeof f.latest.temperature === 'number' && f.latest.temperature > th)
        || (b && b.latest && typeof b.latest.temperature === 'number' && b.latest.temperature > th);
      ref.badge.style.display = alarm ? 'inline-block' : 'none';
      ref.card.className = 'card' + (alarm ? ' alarm' : '');
    }
  }

  // ---------- 顶栏状态 ----------
  function updateStatus(st) {
    if (!st) return;
    el('gwState').textContent = st.gatewayOnline ? '在线' : '离线';
    el('gwDot').className = 'dot ' + (st.gatewayOnline ? 'ok' : 'bad');
    el('devCount').textContent = st.deviceCount;
    el('onlineCount').textContent = st.onlineCount;
  }

  // ---------- 下拉框 ----------
  function rebuildSelect() {
    var sel = el('deviceSelect');
    var ids = Object.keys(devicesById).sort(function (a, b) {
      var ia = idxOf(a), ib = idxOf(b);
      if (ia !== ib) return ia - ib;
      var pa = a.indexOf('_front') >= 0 ? 0 : 1;
      var pb = b.indexOf('_front') >= 0 ? 0 : 1;
      return pa - pb;
    });
    if (ids.length === 0 || sel.options.length === ids.length) return;
    var prev = sel.value;
    sel.innerHTML = '';
    ids.forEach(function (id) {
      var opt = document.createElement('option');
      opt.value = id;
      opt.textContent = (devicesById[id] && devicesById[id].name) || id;
      sel.appendChild(opt);
    });
    if (prev && devicesById[prev]) sel.value = prev;
    selectedDevice = sel.value || ids[0] || null;
    if (selectedDevice) updateLineChart();
  }

  // ---------- 图表 ----------
  function updateLineChart() {
    if (!lineChart || !selectedDevice) return;
    fetch('/api/devices/' + encodeURIComponent(selectedDevice) + '/history?points=60')
      .then(function (r) { return r.json(); })
      .then(function (d) {
        if (!d || !d.buckets) return;
        var times = d.buckets.map(function (b) { return fmtTime(b.t); });
        var avg = d.buckets.map(function (b) { return b.temp ? b.temp.avg : null; });
        var mx = d.buckets.map(function (b) { return b.temp ? b.temp.max : null; });
        var mn = d.buckets.map(function (b) { return b.temp ? b.temp.min : null; });
        var name = (devicesById[selectedDevice] && devicesById[selectedDevice].name) || selectedDevice;
        lineChart.setOption({
          title: { text: name + ' 温度曲线（℃）', left: 'center', textStyle: { color: '#9adbe8', fontSize: 13 } },
          tooltip: { trigger: 'axis' },
          legend: { data: ['平均', '最大', '最小'], top: 24, textStyle: { color: '#b7c4d0' } },
          grid: { left: 46, right: 18, top: 56, bottom: 28 },
          xAxis: { type: 'category', data: times, boundaryGap: false, axisLabel: { color: '#7a8a99' }, axisLine: { lineStyle: { color: '#2c3e50' } } },
          yAxis: { type: 'value', name: '℃', axisLabel: { color: '#7a8a99' }, splitLine: { lineStyle: { color: '#1c2a38' } } },
          series: [
            { name: '平均', type: 'line', smooth: true, showSymbol: false, data: avg, lineStyle: { color: '#00e0ff', width: 2 }, itemStyle: { color: '#00e0ff' } },
            { name: '最大', type: 'line', smooth: true, showSymbol: false, data: mx, lineStyle: { color: '#ff9f43', width: 1.5 }, itemStyle: { color: '#ff9f43' } },
            { name: '最小', type: 'line', smooth: true, showSymbol: false, data: mn, lineStyle: { color: '#7bed9f', width: 1.5 }, itemStyle: { color: '#7bed9f' } },
          ],
        });
      })
      .catch(function () {});
  }

  function updateBarChart() {
    if (!barChart || !statsData || !statsData.coldStorages) return;
    var list = statsData.coldStorages || [];
    var names = list.map(function (s) { return s.name; });
    var vals = list.map(function (s) { return s.avg; });
    var colors = list.map(function (s) { return s.alarm ? '#ff4d4f' : '#00e0c8'; });
    barChart.setOption({
      title: { text: '1号冷库当前温度（℃）', left: 'center', textStyle: { color: '#9adbe8', fontSize: 13 } },
      tooltip: { trigger: 'axis' },
      grid: { left: 46, right: 18, top: 40, bottom: 46 },
      xAxis: { type: 'category', data: names, axisLabel: { color: '#7a8a99', rotate: 45 }, axisLine: { lineStyle: { color: '#2c3e50' } } },
      yAxis: { type: 'value', name: '℃', axisLabel: { color: '#7a8a99' }, splitLine: { lineStyle: { color: '#1c2a38' } } },
      series: [{
        type: 'bar', barMaxWidth: 22,
        data: vals.map(function (v, i) { return { value: v, itemStyle: { color: colors[i] } }; }),
      }],
    });
  }

  // ---------- 事件日志 ----------
  function typeLabel(t) {
    var m = { data: '数据', online: '上线', offline: '离线', command: '命令', alarm: '告警', event: '事件' };
    return m[t] || t;
  }
  function renderEvents(list) {
    var box = el('eventList');
    var html = '';
    list.slice(0, 100).forEach(function (ev) {
      html += '<div class="event-row ' + (ev.level || 'info') + '">'
        + '<span class="ev-time">' + fmtTime(ev.ts) + '</span>'
        + '<span class="ev-type">' + typeLabel(ev.type) + '</span>'
        + '<span class="ev-content">' + escapeHtml(ev.content || '') + '</span>'
        + '</div>';
    });
    box.innerHTML = html;
  }
  var eventRows = [];
  function addEventToLog(ev) {
    eventRows.unshift(ev);
    if (eventRows.length > 100) eventRows.pop();
    renderEvents(eventRows);
  }
  function addAckToLog(msg) {
    /* 服务端对命令回执会先推 event（内容含 requestId 与“成功/失败”），
       再推 ack；这里按 requestId 去重，避免同一回执渲染两行 */
    if (!msg.timeout && eventRows.some(function (ev) {
      return ev.content && String(ev.content).indexOf(msg.requestId) >= 0 && Date.now() - ev.ts < 5000;
    })) {
      return;
    }
    var ok = msg.result_code === 0;
    var desc = ok ? '成功' : (msg.timeout ? '超时' : '失败');
    addEventToLog({ ts: Date.now(), type: 'command', level: ok ? 'info' : 'warn',
      content: '命令 ' + msg.command + ' (' + msg.requestId + ') → ' + msg.deviceId + ' ' + desc });
  }

  // ---------- 数据应用 ----------
  function applyData(devices, stats) {
    devicesById = {};
    (devices || []).forEach(function (d) { devicesById[d.device_id] = d; });
    if (stats) statsData = stats;
    updateCards();
    updateBarChart();
    rebuildSelect();
  }

  // ---------- 按钮 ----------
  function api(path, method, body) {
    return fetch(path, {
      method: method,
      headers: { 'Content-Type': 'application/json' },
      body: body ? JSON.stringify(body) : undefined,
    }).then(function (r) { return r.json(); });
  }
  function sendCommand(id, command, paras) {
    api('/api/devices/' + encodeURIComponent(id) + '/commands', 'POST', { command: command, paras: paras })
      .then(function (r) {
        if (r && r.error) addEventToLog({ ts: Date.now(), type: 'command', level: 'warn', content: r.error });
        else addEventToLog({ ts: Date.now(), type: 'command', level: 'info', content: '已下发 ' + command + ' → ' + id + ' (' + (r && r.requestId) + ')' });
      }).catch(function () {});
  }
  function sendProperty(id, value) {
    api('/api/devices/' + encodeURIComponent(id) + '/properties', 'PUT', { alarm_threshold: value })
      .then(function (r) {
        if (r && r.error) addEventToLog({ ts: Date.now(), type: 'command', level: 'warn', content: r.error });
        else addEventToLog({ ts: Date.now(), type: 'command', level: 'info', content: '已设置 ' + id + ' 阈值 = ' + value + '℃ (' + (r && r.requestId) + ')' });
      }).catch(function () {});
  }
  function onCardClick(e) {
    var btn = e.target.closest ? e.target.closest('button[data-act]') : null;
    if (!btn) return;
    var idx = parseInt(btn.getAttribute('data-idx'), 10);
    var act = btn.getAttribute('data-act');
    var ref = cardRefs[idx];
    if (!ref) return;
    var pos = ref.sel.value === 'back' ? 'back' : 'front';
    var id = devId(idx, pos);
    if (act === 'led-on') sendCommand(id, 'set_led', { on: 1 });
    else if (act === 'led-off') sendCommand(id, 'set_led', { on: 0 });
    else if (act === 'beep-on') sendCommand(id, 'set_beeper', { on: 1, duration_ms: 3000 });
    else if (act === 'beep-off') sendCommand(id, 'set_beeper', { on: 0 });
    else if (act === 'th-set') {
      var v = parseFloat(ref.input.value);
      if (isNaN(v)) { addEventToLog({ ts: Date.now(), type: 'command', level: 'warn', content: '阈值输入无效' }); return; }
      sendProperty(id, v);
    }
  }

  // ---------- WebSocket ----------
  function connectWs() {
    var proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
    var ws = new WebSocket(proto + location.host + '/ws');
    ws.onopen = function () { console.log('WS 已连接'); wsRetry = 1000; setWsStatus(true); };
    ws.onmessage = function (e) {
      var msg;
      try { msg = JSON.parse(e.data); } catch (err) { return; }
      if (msg.type === 'status') updateStatus(msg.status);
      else if (msg.type === 'data') applyData(msg.devices, msg.stats);
      else if (msg.type === 'event') {
        if (msg.event) addEventToLog(msg.event);
        else if (msg.events) renderEvents(msg.events);
      } else if (msg.type === 'ack') addAckToLog(msg);
    };
    ws.onclose = function () {
      console.log('WS 断开，重连中');
      setWsStatus(false);
      setTimeout(connectWs, wsRetry);
      wsRetry = Math.min(wsRetry * 2, 15000);
    };
    ws.onerror = function () { try { ws.close(); } catch (e) {} };
  }

  // ---------- 时钟 ----------
  function updateClock() {
    var d = new Date();
    el('clock').textContent = d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate())
      + ' ' + pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds());
  }

  // ---------- WS 连接状态指示 ----------
  function setWsStatus(ok) {
    el('wsState').textContent = ok ? '已连接' : '已断开';
    el('wsDot').className = 'dot ' + (ok ? 'ok' : 'bad');
  }

  // ---------- 初始化兜底：先 REST 拉一次快照，WS 断线时页面仍有数据 ----------
  function loadInitial() {
    /* 设备与统计一起应用，避免 statsData 为 null 时柱状图崩 */
    Promise.all([
      fetch('/api/devices').then(function (r) { return r.json(); }).catch(function () { return []; }),
      fetch('/api/stats').then(function (r) { return r.json(); }).catch(function () { return null; }),
    ]).then(function (res) {
      var devs = res[0], st = res[1];
      if (Array.isArray(devs) && devs.length) applyData(devs, st && st.coldStorages ? st : null);
    });
    fetch('/api/events?limit=50').then(function (r) { return r.json(); })
      .then(function (evs) { if (Array.isArray(evs) && evs.length) renderEvents(evs); })
      .catch(function () {});
    fetch('/api/status').then(function (r) { return r.json(); })
      .then(function (st) { updateStatus(st); })
      .catch(function () {});
  }

  // ---------- 初始化 ----------
  function init() {
    buildCards();
    if (typeof echarts !== 'undefined') {
      lineChart = echarts.init(el('lineChart'));
      barChart = echarts.init(el('barChart'));
    } else {
      el('lineChart').textContent = 'ECharts 加载失败（检查 public/vendor/echarts.min.js）';
    }
    el('deviceSelect').addEventListener('change', function () {
      selectedDevice = el('deviceSelect').value;
      updateLineChart();
    });
    window.addEventListener('resize', function () {
      if (lineChart) lineChart.resize();
      if (barChart) barChart.resize();
    });
    updateClock();
    setInterval(updateClock, 1000);
    setInterval(function () { if (selectedDevice) updateLineChart(); }, 10000);
    loadInitial();   // REST 兜底（WS 未连上时页面也有数据）
    connectWs();
  }

  init();
})();