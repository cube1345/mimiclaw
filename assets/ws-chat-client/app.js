const hostInput = document.getElementById("hostInput");
const portInput = document.getElementById("portInput");
const chatIdInput = document.getElementById("chatIdInput");
const connectBtn = document.getElementById("connectBtn");
const disconnectBtn = document.getElementById("disconnectBtn");
const voiceBtn = document.getElementById("voiceBtn");
const sendBtn = document.getElementById("sendBtn");
const clearBtn = document.getElementById("clearBtn");
const chatForm = document.getElementById("chatForm");
const messageInput = document.getElementById("messageInput");
const chatLog = document.getElementById("chatLog");
const statusBadge = document.getElementById("statusBadge");
const messageTpl = document.getElementById("messageTpl");

let ws = null;
let reconnectTimer = null;
let speech = null;

function timestamp() {
  return new Date().toLocaleTimeString("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

function appendMessage(role, text) {
  const node = messageTpl.content.firstElementChild.cloneNode(true);
  node.classList.add(role);
  node.querySelector(".msg-role").textContent = roleLabel(role);
  node.querySelector(".msg-time").textContent = timestamp();
  node.querySelector(".msg-body").textContent = text;
  chatLog.appendChild(node);
  chatLog.scrollTop = chatLog.scrollHeight;
}

function roleLabel(role) {
  if (role === "user") return "你";
  if (role === "assistant") return "MimiClaw";
  return "系统";
}

function setStatus(state, text) {
  statusBadge.className = `badge ${state}`;
  statusBadge.textContent = text;
}

function syncUi() {
  const online = ws && ws.readyState === WebSocket.OPEN;
  const connecting = ws && ws.readyState === WebSocket.CONNECTING;
  connectBtn.disabled = online || connecting;
  disconnectBtn.disabled = !ws || (!online && !connecting);
  sendBtn.disabled = !online;
  voiceBtn.disabled = !("webkitSpeechRecognition" in window || "SpeechRecognition" in window);
}

function websocketUrl() {
  const host = hostInput.value.trim();
  const port = portInput.value.trim() || "18789";
  return `ws://${host}:${port}/`;
}

function connect() {
  const host = hostInput.value.trim();
  if (!host) {
    appendMessage("system", "请先填写 ESP32 的 IP 地址。");
    return;
  }

  clearTimeout(reconnectTimer);

  ws = new WebSocket(websocketUrl());
  setStatus("connecting", "连接中");
  syncUi();

  ws.addEventListener("open", () => {
    setStatus("online", "已连接");
    appendMessage("system", `已连接到 ${websocketUrl()}`);
    syncUi();
  });

  ws.addEventListener("message", (event) => {
    try {
      const payload = JSON.parse(event.data);
      if (payload.type === "response") {
        appendMessage("assistant", payload.content || "");
      } else {
        appendMessage("system", `收到未知消息：${event.data}`);
      }
    } catch (err) {
      appendMessage("system", `收到非 JSON 数据：${event.data}`);
    }
  });

  ws.addEventListener("close", () => {
    setStatus("offline", "未连接");
    appendMessage("system", "WebSocket 已断开。");
    syncUi();
  });

  ws.addEventListener("error", () => {
    appendMessage("system", "WebSocket 连接失败，请检查 ESP32 IP、端口和网络。");
  });
}

function disconnect() {
  clearTimeout(reconnectTimer);
  if (ws) {
    ws.close();
    ws = null;
  }
  setStatus("offline", "未连接");
  syncUi();
}

function sendMessage(text) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    appendMessage("system", "尚未连接到 ESP32。");
    return;
  }

  const content = text.trim();
  if (!content) {
    return;
  }

  const payload = {
    type: "message",
    content,
    chat_id: chatIdInput.value.trim() || "pc_demo",
  };

  ws.send(JSON.stringify(payload));
  appendMessage("user", content);
}

function initSpeech() {
  const Recognition = window.SpeechRecognition || window.webkitSpeechRecognition;
  if (!Recognition) {
    voiceBtn.disabled = true;
    return;
  }

  speech = new Recognition();
  speech.lang = "zh-CN";
  speech.interimResults = false;
  speech.maxAlternatives = 1;

  speech.addEventListener("start", () => {
    voiceBtn.textContent = "正在听...";
  });

  speech.addEventListener("result", (event) => {
    const transcript = event.results?.[0]?.[0]?.transcript?.trim() || "";
    if (!transcript) {
      appendMessage("system", "没有识别到有效语音内容。");
      return;
    }
    messageInput.value = transcript;
    sendMessage(transcript);
    messageInput.value = "";
  });

  speech.addEventListener("end", () => {
    voiceBtn.textContent = "语音输入";
  });

  speech.addEventListener("error", (event) => {
    voiceBtn.textContent = "语音输入";
    appendMessage("system", `语音识别失败：${event.error || "unknown"}`);
  });
}

connectBtn.addEventListener("click", connect);
disconnectBtn.addEventListener("click", disconnect);

chatForm.addEventListener("submit", (event) => {
  event.preventDefault();
  sendMessage(messageInput.value);
  messageInput.value = "";
  messageInput.focus();
});

clearBtn.addEventListener("click", () => {
  chatLog.innerHTML = "";
  appendMessage("system", "聊天记录已清空。");
});

voiceBtn.addEventListener("click", () => {
  if (!speech) {
    appendMessage("system", "当前浏览器不支持 Web Speech API。");
    return;
  }
  speech.start();
});

initSpeech();
appendMessage("system", "准备就绪。先连接到 ESP32，再开始聊天。");
syncUi();
