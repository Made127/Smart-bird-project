// 默认服务地址：如果网页由 8010 端口服务，则直接使用当前来源；打包 App 中由用户填写并保存。
const DEFAULT_SERVER_URL =
  location.port === "8010" ? location.origin : "";

// 页面全局状态，集中保存服务连接、设备心跳、传感器、电池、录音和告警数据。
const state = {
  productId: "BIRD-TEST-001",
  serverUrl: localStorage.getItem("birdServerUrl") || DEFAULT_SERVER_URL,
  serverOnline: false,
  deviceOnline: false,
  lastSeen: 0,
  requestMs: null,
  collectIntervalSeconds: 7200,
  lastCollectionAt: 0,
  nextCollectionAt: 0,
  pendingCommands: 0,
  batteryVoltageMv: null,
  batteryPercent: null,
  batteryLow: false,
  batteryCharging: false,
  batteryFull: false,
  lastBatteryLow: null,
  sensors: {
    light: {
      label: "光照",
      value: null,
      unit: " lx",
      range: "实时读取",
      status: "offline",
      note: "等待 BH1750 数据",
      implemented: true
    },
    human: {
      label: "人体感应",
      value: null,
      unit: "",
      range: "有人 / 无人",
      status: "offline",
      note: "等待红外传感器数据",
      implemented: true
    },
    ion: realSensor("土壤电导率", " us/cm", "探针读取"),
    salt: realSensor("盐分", " ppm", "探针读取"),
    nitrogen: realSensor("氮含量", " mg/kg", "探针读取"),
    phosphorus: realSensor("磷含量", " mg/kg", "探针读取"),
    potassium: realSensor("钾含量", " mg/kg", "探针读取"),
    ph: realSensor("土壤酸碱度", " pH", "探针读取"),
    moisture: realSensor("土壤湿度", "%", "探针读取"),
    temp: realSensor("土壤温度", " °C", "探针读取")
  },
  history: [],
  events: [],
  voices: [],
  alerts: [],
  recorder: null,
  recordingStream: null,
  recordingChunks: [],
  pressing: false,
  recording: false,
  uploading: false,
  lastDeviceOnline: null,
  lastHuman: null
};

// 总览页优先展示的指标。
const metricKeys = ["light", "human", "moisture", "temp"];

// 导航栏 section id 到页面标题的映射。
const sections = {
  dashboard: "设备总览",
  network: "局域网连接",
  sensors: "传感器采集",
  voice: "语音与对话",
  control: "设备控制",
  alerts: "告警与提醒",
  device: "设备信息"
};

// 未实现传感器占位数据；用于明确告诉用户该项当前固件还没有接入。
function unimplementedSensor(label, unit) {
  return {
    label,
    value: "未接入",
    unit,
    range: "--",
    status: "offline",
    note: "当前项目尚未实现",
    implemented: false
  };
}

// 已接入真实硬件的传感器默认状态。
function realSensor(label, unit, range) {
  return {
    label,
    value: null,
    unit,
    range,
    status: "offline",
    note: "等待探针数据",
    implemented: true
  };
}

// DOM 查询快捷函数。
function $(selector) {
  return document.querySelector(selector);
}

// DOM 多元素查询快捷函数，统一转成数组便于 forEach/map。
function $all(selector) {
  return Array.from(document.querySelectorAll(selector));
}

// 安全设置文本；目标元素不存在时静默跳过。
function setText(selector, value) {
  const element = $(selector);
  if (element) element.textContent = value;
}

// 把时间格式化为中文本地时间，供事件列表和心跳显示使用。
function nowText(date = new Date()) {
  return date.toLocaleTimeString("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit"
  });
}

// 顶部/底部轻提示，避免频繁使用 alert 阻塞页面操作。
function showToast(message) {
  const toast = $("#toast");
  if (!toast) return;
  toast.textContent = message;
  toast.classList.add("show");
  clearTimeout(showToast.timer);
  showToast.timer = setTimeout(() => toast.classList.remove("show"), 2600);
}

// 增加普通事件记录，只保留最近 20 条。
function addEvent(title, detail) {
  state.events.unshift({ title, detail, time: new Date() });
  state.events = state.events.slice(0, 20);
  renderEvents();
}

// 增加告警记录，只保留最近 20 条。
function addAlert(title, detail, level = "medium") {
  state.alerts.unshift({ title, detail, level, time: new Date() });
  state.alerts = state.alerts.slice(0, 20);
  renderAlerts();
}

// 拼接服务端 API 地址，并去掉用户输入末尾多余斜杠。
function apiUrl(path) {
  return `${state.serverUrl.replace(/\/+$/, "")}${path}`;
}

// 请求 JSON 接口的统一封装；非 2xx 状态直接抛错给调用方处理。
async function requestJson(path, options = {}) {
  const response = await fetch(apiUrl(path), {
    cache: "no-store",
    ...options
  });
  if (!response.ok) {
    throw new Error(`HTTP ${response.status}`);
  }
  return response.json();
}

// 刷新电脑服务、ESP32 心跳、传感器、电池和历史数据。
async function refreshDeviceStatus(showResult = false) {
  const started = performance.now();
  try {
    // 这行很重要：先请求 /health，用来区分“电脑服务离线”和“ESP32 设备离线”。
    await requestJson("/health");
    const status = await requestJson("/device/status");
    state.requestMs = Math.round(performance.now() - started);
    state.serverOnline = true;
    state.deviceOnline = Boolean(status.online);
    state.lastSeen = Number(status.last_seen || 0);
    state.collectIntervalSeconds = Number(status.collect_interval_seconds || 7200);
    state.lastCollectionAt = Number(status.last_collection_at || 0);
    state.nextCollectionAt = Number(status.next_collection_at || 0);
    state.pendingCommands = Number(status.pending_commands || 0);
    state.batteryVoltageMv =
      typeof status.battery_voltage_mv === "number" ? status.battery_voltage_mv : null;
    state.batteryPercent =
      typeof status.battery_percent === "number" ? status.battery_percent : null;
    state.batteryLow = Boolean(status.battery_low);
    state.batteryCharging = Boolean(status.battery_charging);
    state.batteryFull = Boolean(status.battery_full);

    // 这行很重要：只在低电量状态从 false 变 true 时新增告警，避免每次刷新重复刷屏。
    if (state.lastBatteryLow !== null && state.lastBatteryLow !== state.batteryLow && state.batteryLow) {
      addAlert("电量过低", `当前电量约 ${state.batteryPercent ?? "--"}%，请尽快充电。`, "high");
    }
    state.lastBatteryLow = state.batteryLow;

    const light = state.sensors.light;
    if (typeof status.lux === "number") {
      light.value = Number(status.lux.toFixed(1));
      light.status = state.deviceOnline ? "good" : "offline";
      light.note = describeLight(status.lux);
    } else {
      light.value = null;
      light.status = "offline";
      light.note = "尚未收到光照数据";
    }

    const human = state.sensors.human;
    // 这行很重要：红外值只接受 0/1，其他值视为未收到有效硬件数据。
    if (status.ir_raw === 0 || status.ir_raw === 1) {
      const detected = status.ir_raw === 1;
      human.value = detected ? "有人" : "无人";
      human.status = detected ? "watch" : "good";
      human.note = detected ? "红外传感器检测到人体" : "红外传感器未检测到人体";
      if (state.lastHuman !== null && state.lastHuman !== detected && detected) {
        addEvent("人体感应", "红外传感器检测到有人靠近。");
        addAlert("检测到人体", "ESP32 红外传感器报告有人。");
      }
      state.lastHuman = detected;
    } else {
      human.value = null;
      human.status = "offline";
      human.note = "尚未收到红外数据";
    }

    setSoilSensor("temp", status.soil_temperature, "土壤温度");
    setSoilSensor("moisture", status.soil_humidity, "土壤湿度");
    setSoilSensor("ion", status.soil_ec, "土壤电导率");
    setSoilSensor("salt", status.soil_salt, "盐分");
    setSoilSensor("nitrogen", status.soil_nitrogen, "氮含量");
    setSoilSensor("phosphorus", status.soil_phosphorus, "磷含量");
    setSoilSensor("potassium", status.soil_potassium, "钾含量");
    setSoilSensor("ph", status.soil_ph, "土壤 PH");

    // 这行很重要：设备在线状态变化时才写事件/告警，避免轮询期间重复记录。
    if (state.lastDeviceOnline !== null && state.lastDeviceOnline !== state.deviceOnline) {
      if (state.deviceOnline) {
        addEvent("设备上线", "ESP32 已恢复局域网心跳。");
      } else {
        addAlert("设备离线", "电脑服务未收到 ESP32 心跳。", "high");
      }
    }
    state.lastDeviceOnline = state.deviceOnline;

    if (showResult) {
      showToast(state.deviceOnline ? "设备连接正常" : "服务正常，但 ESP32 未在线");
    }

    try {
      // 历史曲线是附加信息，读取失败不能影响当前状态展示。
      const history = await requestJson("/device/history?limit=30");
      if (Array.isArray(history.history)) {
        state.history = history.history
          .map((point) => ({
            time: new Date(Number(point.time || 0) * 1000),
            light: point.lux,
            temp: point.soil_temperature,
            moisture: point.soil_humidity,
            ion: point.soil_ec,
            salt: point.soil_salt,
            nitrogen: point.soil_nitrogen,
            phosphorus: point.soil_phosphorus,
            potassium: point.soil_potassium,
            ph: point.soil_ph,
            batteryPercent: point.battery_percent,
            batteryVoltageMv: point.battery_voltage_mv,
            source: point.source || "device"
          }))
          .filter((point) =>
            ["light", "temp", "moisture", "ion", "salt", "nitrogen", "phosphorus", "potassium", "ph"]
              .some((key) => typeof point[key] === "number")
          );
      }
    } catch (error) {
      // 历史表还没创建时，实时状态仍然有用，所以这里不报错。
    }
  } catch (error) {
    state.requestMs = Math.round(performance.now() - started);
    state.serverOnline = false;
    state.deviceOnline = false;
    if (showResult) showToast(`连接失败：${error.message}`);
  }
  renderAll();
}

// 手机端手动触发一次 ESP32 采集。
async function collectNow() {
  try {
    const result = await requestJson("/app/collect", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ source: "app" })
    });
    addEvent("立即采集", result.device_online ? "已下发到 ESP32，等待采集上报。" : "命令已排队，等待 ESP32 上线。");
    showToast(result.device_online ? "已下发立即采集" : "设备暂未在线，采集命令已排队");
    await refreshDeviceStatus(false);
  } catch (error) {
    showToast(`立即采集失败：${error.message}`);
  }
}

// 根据 lux 粗略描述光照状态。
function describeLight(lux) {
  if (lux < 10) return "环境很暗";
  if (lux < 50) return "环境比较暗";
  if (lux < 200) return "环境光线偏暗";
  if (lux < 500) return "环境光线正常";
  return "环境比较明亮";
}

// 更新单个土壤类传感器的显示状态。
function setSoilSensor(key, value, label) {
  const sensor = state.sensors[key];
  if (!sensor) return;
  if (typeof value === "number") {
    const rounded = Number.isInteger(value) ? value : Number(value.toFixed(1));
    sensor.value = rounded;
    sensor.status = state.deviceOnline ? "good" : "offline";
    sensor.note = `${label}来自土壤探针最近一次上报`;
  } else {
    sensor.value = null;
    sensor.status = "offline";
    sensor.note = "尚未收到探针数据";
  }
}

// 格式化下一次自动采集倒计时。
function formatCountdown(targetSeconds) {
  const target = Number(targetSeconds || 0);
  if (!target) return "等待首次采集";
  const remaining = Math.max(0, Math.round(target - Date.now() / 1000));
  const hours = Math.floor(remaining / 3600);
  const minutes = Math.floor((remaining % 3600) / 60);
  if (hours > 0) return `${hours}小时${minutes}分钟`;
  return `${minutes}分钟`;
}

// 渲染页面顶部、侧栏和设备摘要状态。
function renderStatus() {
  setText("#productId", state.productId);
  setText("#wifiStatus", state.deviceOnline ? "已连接" : "未连接");
  setText("#batteryText", formatBatteryText());
  setText("#modeText", "未接入");
  setText("#probeText", state.deviceOnline ? "红外 / 光照 / 土壤探针在线" : "等待设备");
  setText(
    "#commandStatus",
    state.pendingCommands > 0 ? `待执行 ${state.pendingCommands} 条` : "真实模式"
  );
  setText("#sideOnline", state.deviceOnline ? "设备在线" : "设备离线");
  setText(
    "#sideLastSync",
    state.lastSeen ? `${nowText(new Date(state.lastSeen * 1000))} 心跳` : "等待心跳"
  );

  const signal = $(".signal-dot");
  if (signal) {
    signal.classList.toggle("online", state.deviceOnline);
    signal.classList.toggle("offline", !state.deviceOnline);
  }

  setText(
    "#deviceSummary",
    state.deviceOnline
      ? `ESP32 已连接，光照 ${formatSensorValue(state.sensors.light)}，土壤湿度 ${formatSensorValue(state.sensors.moisture)}，PH ${formatSensorValue(state.sensors.ph)}。下一次自动采集约 ${formatCountdown(state.nextCollectionAt)} 后执行。`
      : state.serverOnline
        ? "电脑 AI 服务已连接，正在等待 ESP32 心跳。"
        : "无法连接电脑 AI 服务，请检查服务地址和 WiFi。"
  );
}

// 格式化传感器值；空值统一显示 "--"。
function formatSensorValue(sensor) {
  if (sensor.value === null) return "--";
  return `${sensor.value}${sensor.unit}`;
}

// 格式化电池百分比、实时电压和充电状态，硬件规格在设备信息页固定展示。
function formatBatteryText() {
  if (typeof state.batteryPercent !== "number") return "未接入";
  const parts = [`${state.batteryPercent}%`];
  if (typeof state.batteryVoltageMv === "number") {
    parts.push(`${(state.batteryVoltageMv / 1000).toFixed(2)}V`);
  }
  if (state.batteryCharging) parts.push("充电中");
  else if (state.batteryFull) parts.push("已充满");
  else if (state.batteryLow) parts.push("低电量");
  return parts.join(" · ");
}

// 渲染总览页四个指标卡片。
function renderMetrics() {
  const grid = $("#metricGrid");
  if (!grid) return;
  grid.innerHTML = metricKeys.map((key) => {
    const item = state.sensors[key];
    return `
      <article class="metric-card ${item.status}">
        <h3>${item.label}</h3>
        <div class="metric-value">${formatSensorValue(item)}</div>
        <div class="metric-foot">
          <span>${item.range}</span>
          <strong>${statusLabel(item)}</strong>
        </div>
      </article>
    `;
  }).join("");
}

// 把内部状态转换为用户可读状态文本。
function statusLabel(sensor) {
  if (!sensor.implemented) return "未接入";
  if (sensor.status === "good") return "正常";
  if (sensor.status === "watch") return "触发";
  return "离线";
}

// 渲染传感器表格。
function renderSensorTable() {
  const table = $("#sensorTable");
  if (!table) return;
  table.innerHTML = Object.values(state.sensors).map((item) => `
    <tr>
      <td>${item.label}</td>
      <td><strong>${formatSensorValue(item)}</strong></td>
      <td>${item.range}</td>
      <td><span class="badge ${item.status}">${statusLabel(item)}</span></td>
      <td>${item.note}</td>
    </tr>
  `).join("");
}

// 绘制历史趋势图。
function renderTrend() {
  const canvas = $("#trendCanvas");
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  const selected = $("#trendSelect")?.value || "light";
  const width = canvas.width;
  const height = canvas.height;
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = "#fbfcfb";
  ctx.fillRect(0, 0, width, height);

  const historyKeyBySelect = {
    light: "light",
    moisture: "moisture",
    temp: "temp",
    ion: "ion",
    ph: "ph"
  };
  const historyKey = historyKeyBySelect[selected] || "light";
  // 这行很重要：趋势图只绘制数值型历史点，避免 null/字符串导致 canvas 坐标变 NaN。
  const values = state.history
    .map((point) => point[historyKey])
    .filter((value) => typeof value === "number");

  if (values.length < 2) {
    ctx.fillStyle = "#66736c";
    ctx.font = "16px Microsoft YaHei, Arial";
    ctx.textAlign = "center";
    ctx.fillText("等待更多真实历史数据", width / 2, height / 2);
    ctx.textAlign = "left";
    return;
  }

  const pad = 36;
  // 给最大/最小值留一点边距，避免折线贴到画布边缘。
  const min = Math.min(...values) - 2;
  const max = Math.max(...values) + 2;
  const xFor = (index) => pad + index * ((width - pad * 2) / (values.length - 1));
  const yFor = (value) =>
    height - pad - ((value - min) / Math.max(1, max - min)) * (height - pad * 2);

  ctx.strokeStyle = "#dfe6df";
  ctx.lineWidth = 1;
  for (let i = 0; i < 4; i += 1) {
    const y = pad + i * ((height - pad * 2) / 3);
    ctx.beginPath();
    ctx.moveTo(pad, y);
    ctx.lineTo(width - pad, y);
    ctx.stroke();
  }

  ctx.beginPath();
  values.forEach((value, index) => {
    const x = xFor(index);
    const y = yFor(value);
    if (index === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.strokeStyle = "#237c9d";
  ctx.lineWidth = 3;
  ctx.stroke();
}

// 渲染最近事件列表。
function renderEvents() {
  const list = $("#eventList");
  if (!list) return;
  if (!state.events.length) {
    list.innerHTML = "<li><strong>暂无真实事件</strong><time>等待设备上报</time></li>";
    return;
  }
  list.innerHTML = state.events.map((event) => `
    <li>
      <strong>${event.title}</strong>
      <div>${event.detail}</div>
      <time>${nowText(event.time)}</time>
    </li>
  `).join("");
}

// 渲染手机语音对话历史。
function renderVoices() {
  const list = $("#voiceList");
  if (!list) return;
  if (!state.voices.length) {
    list.innerHTML = "<li><strong>暂无对话</strong><div>按住左侧按钮开始说话。</div><time>等待输入</time></li>";
    return;
  }
  list.innerHTML = state.voices.map((voice) => `
    <li>
      <strong>${voice.title}</strong>
      <div>${voice.detail}</div>
      <time>${nowText(voice.time)}</time>
    </li>
  `).join("");
}

// 渲染告警列表。
function renderAlerts() {
  const list = $("#alertList");
  if (!list) return;
  if (!state.alerts.length) {
    list.innerHTML = '<div class="alert-item"><span class="alert-level"></span><div><h3>暂无真实告警</h3><time>设备运行状态正常</time></div><span class="badge good">正常</span></div>';
    return;
  }
  list.innerHTML = state.alerts.map((alert, index) => `
    <article class="alert-item">
      <span class="alert-level ${alert.level === "high" ? "high" : ""}"></span>
      <div>
        <h3>${alert.title}</h3>
        <div>${alert.detail}</div>
        <time>${nowText(alert.time)}</time>
      </div>
      <button class="text-btn" data-alert="${index}" type="button">处理</button>
    </article>
  `).join("");
}

// 渲染局域网连接状态卡片。
function renderConnection() {
  const stateBox = $("#connectionState");
  if (stateBox) {
    const online = state.serverOnline && state.deviceOnline;
    stateBox.innerHTML = `
      <span class="large-dot ${online ? "online" : "offline"}"></span>
      <h3>${online ? "设备已连接" : state.serverOnline ? "等待 ESP32" : "服务未连接"}</h3>
      <p>${online ? "手机、电脑服务和 ESP32 的局域网链路正常。" : "请确认三台设备连接同一个 WiFi，并启动电脑服务。"}</p>
    `;
  }
  setText("#ipText", state.serverUrl);
  setText("#connectTime", state.requestMs === null ? "--" : `${state.requestMs} ms`);
  setText(
    "#networkTime",
    state.lastSeen ? nowText(new Date(state.lastSeen * 1000)) : "--"
  );
}

// 统一刷新所有视图。
function renderAll() {
  renderStatus();
  renderMetrics();
  renderSensorTable();
  renderTrend();
  renderEvents();
  renderVoices();
  renderAlerts();
  renderConnection();
}

// 绑定左侧导航切换。
function bindNavigation() {
  $all(".nav-item").forEach((button) => {
    button.addEventListener("click", () => {
      const section = button.dataset.section;
      $all(".nav-item").forEach((item) => item.classList.remove("is-active"));
      button.classList.add("is-active");
      $all(".section").forEach((item) => {
        item.classList.toggle("is-active", item.id === section);
      });
      history.replaceState(null, "", `#${section}`);
      setText("#pageTitle", sections[section]);
      if (section === "dashboard") renderTrend();
    });
  });
}

// 禁用当前固件尚未实现的控制项，避免用户误以为命令已经支持。
function disableUnsupportedControls() {
  [
    "#sleepToggle",
    "#dndToggle",
    "#dndStart",
    "#dndEnd",
    "#probeToggleBtn",
    "#rebootBtn",
    "#factoryBtn"
  ].forEach((selector) => {
    const element = $(selector);
    if (element) {
      element.disabled = true;
      element.title = "当前固件尚未实现";
    }
  });
  $all(".light-chip, .action-btn").forEach((element) => {
    if (element.dataset.command === "collect") return;
    element.disabled = true;
    element.title = "当前固件尚未实现";
  });
  setText("#lightText", "未接入");
  setText("#commandStatus", "真实模式");
  setText("#firmwareText", "bird_test（版本未上报）");
}

// 绑定按钮、表单、趋势选择和告警处理事件。
function bindControls() {
  $("#refreshBtn")?.addEventListener("click", () => collectNow());
  $("#collectBtn")?.addEventListener("click", () => collectNow());
  $("#sendAllBtn")?.addEventListener("click", () => collectNow());
  $("#trendSelect")?.addEventListener("change", renderTrend);

  $("#clearEventsBtn")?.addEventListener("click", () => {
    state.events = [];
    renderEvents();
  });
  $("#clearAlertsBtn")?.addEventListener("click", () => {
    state.alerts = [];
    renderAlerts();
  });
  $("#alertList")?.addEventListener("click", (event) => {
    const button = event.target.closest("[data-alert]");
    if (!button) return;
    state.alerts.splice(Number(button.dataset.alert), 1);
    renderAlerts();
  });

  $("#serverForm")?.addEventListener("submit", (event) => {
    event.preventDefault();
    const value = $("#serverUrlInput").value
      .replace(/\s+/g, "")
      .replace(/\/+$/, "");
    $("#serverUrlInput").value = value;
    // 这行很重要：服务地址必须带 http/https 和端口，否则 fetch 会请求到错误位置。
    if (!/^https?:\/\/[^/]+(?::\d+)?$/i.test(value)) {
      showToast("请输入完整地址，例如 http://192.168.1.10:8010");
      return;
    }
    state.serverUrl = value;
    localStorage.setItem("birdServerUrl", value);
    refreshDeviceStatus(true);
  });

  const collectAction = $('.action-btn[data-command="collect"]');
  collectAction?.addEventListener("click", () => collectNow());

  bindPushToTalk();
  disableUnsupportedControls();
}

// 更新语音按钮旁的状态文本。
function setVoiceState(text) {
  setText("#voiceState", text);
}

// 绑定“按住说话”交互，支持松开、取消和窗口失焦自动停止录音。
function bindPushToTalk() {
  const button = $("#talkButton");
  if (!button) return;

  button.addEventListener("pointerdown", (event) => {
    event.preventDefault();
    state.pressing = true;
    button.setPointerCapture?.(event.pointerId);
    startRecording();
  });

  const release = (event) => {
    if (event) event.preventDefault();
    state.pressing = false;
    stopRecording();
  };
  button.addEventListener("pointerup", release);
  button.addEventListener("pointercancel", release);
  button.addEventListener("lostpointercapture", release);
  window.addEventListener("blur", release);
}

// 请求麦克风权限并开始录音。
async function startRecording() {
  if (state.recording || state.uploading) return;
  if (!navigator.mediaDevices?.getUserMedia || !window.MediaRecorder) {
    showToast("当前系统不支持手机录音");
    return;
  }

  try {
    setVoiceState("正在请求麦克风");
    // 这行很重要：开启浏览器回声消除/降噪，减少手机外放和环境噪声进入 ASR。
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: {
        echoCancellation: true,
        noiseSuppression: true,
        autoGainControl: true
      }
    });
    // 用户授权过程中可能已经松手，此时必须立刻关闭麦克风轨道。
    if (!state.pressing) {
      stream.getTracks().forEach((track) => track.stop());
      setVoiceState("等待说话");
      return;
    }

    const preferredTypes = [
      "audio/webm;codecs=opus",
      "audio/webm",
      "audio/mp4",
      "audio/ogg;codecs=opus"
    ];
    // 这行很重要：不同手机支持的录音格式不同，必须按兼容性选择 MIME。
    const mimeType = preferredTypes.find((type) => MediaRecorder.isTypeSupported(type));
    const recorder = mimeType
      ? new MediaRecorder(stream, { mimeType })
      : new MediaRecorder(stream);

    state.recordingStream = stream;
    state.recorder = recorder;
    state.recordingChunks = [];
    state.recording = true;
    $("#talkButton").classList.add("is-recording");
    setVoiceState("正在录音");

    recorder.addEventListener("dataavailable", (event) => {
      if (event.data.size > 0) state.recordingChunks.push(event.data);
    });
    recorder.addEventListener("stop", handleRecordingStopped, { once: true });
    // 每 200ms 产出一次数据块，避免长录音结束时一次性占用太多内存。
    recorder.start(200);

    clearTimeout(startRecording.limitTimer);
    // 这行很重要：限制单次录音最长 15 秒，避免用户误触导致持续占用麦克风。
    startRecording.limitTimer = setTimeout(() => {
      state.pressing = false;
      stopRecording();
    }, 15000);
  } catch (error) {
    state.recording = false;
    setVoiceState("麦克风不可用");
    showToast(`无法录音：${error.message}`);
  }
}

// 停止录音；真正上传逻辑在 MediaRecorder 的 stop 事件里执行。
function stopRecording() {
  clearTimeout(startRecording.limitTimer);
  if (!state.recording || !state.recorder) return;
  state.recording = false;
  $("#talkButton")?.classList.remove("is-recording");
  setVoiceState("正在发送");
  if (state.recorder.state !== "inactive") state.recorder.stop();
}

// 录音停止后整理 Blob，并过滤过短的误触录音。
async function handleRecordingStopped() {
  state.recordingStream?.getTracks().forEach((track) => track.stop());
  const mimeType = state.recorder?.mimeType || "application/octet-stream";
  const blob = new Blob(state.recordingChunks, { type: mimeType });
  state.recorder = null;
  state.recordingStream = null;
  state.recordingChunks = [];

  // 这行很重要：过小的 Blob 通常是误触或空音频，上传会浪费一次 AI 请求。
  if (blob.size < 1000) {
    setVoiceState("录音太短");
    showToast("按住时间太短，请重新说");
    return;
  }

  await uploadVoice(blob);
}

// 上传手机录音给服务端，服务端会把回复音频加入 ESP32 播放队列。
async function uploadVoice(blob) {
  state.uploading = true;
  $("#talkButton").disabled = true;
  setVoiceState("AI 正在处理");

  try {
    // 这行很重要：这里上传的是原始音频 Blob，不要 JSON.stringify，否则服务端无法解码音频。
    const result = await requestJson("/app/voice", {
      method: "POST",
      headers: {
        "Content-Type": blob.type || "application/octet-stream"
      },
      body: blob
    });
    state.voices.unshift({
      title: `你：${result.text || "未识别"}`,
      detail: `AI：${result.reply || "无回复"}（已发送到硬件扬声器）`,
      time: new Date()
    });
    state.voices = state.voices.slice(0, 20);
    addEvent("手机语音发送成功", "AI 回复已进入 ESP32 播放队列。");
    setVoiceState("已发送到扬声器");
    renderVoices();
  } catch (error) {
    setVoiceState("发送失败");
    showToast(`语音发送失败：${error.message}`);
  } finally {
    state.uploading = false;
    $("#talkButton").disabled = false;
    setTimeout(() => {
      if (!state.recording && !state.uploading) setVoiceState("等待说话");
    }, 1800);
  }
}

// 页面初始化入口。
function init() {
  if ("serviceWorker" in navigator) {
    // 当前开发阶段禁用旧 Service Worker，避免缓存导致页面代码更新后仍加载旧版本。
    navigator.serviceWorker.getRegistrations()
      .then((registrations) => registrations.forEach((item) => item.unregister()))
      .catch(() => {});
  }
  const serverInput = $("#serverUrlInput");
  if (serverInput) serverInput.value = state.serverUrl;
  bindNavigation();
  bindControls();
  renderAll();
  const initialSection = location.hash.slice(1);
  if (sections[initialSection]) {
    $(`.nav-item[data-section="${initialSection}"]`)?.click();
  }
  refreshDeviceStatus();
  setInterval(() => refreshDeviceStatus(false), 2000);
}

init();
