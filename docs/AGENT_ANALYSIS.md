# MimiClaw Agent 部分分析

本文面向学习本项目 agent 实现的人，重点解释 MimiClaw 如何在 ESP32-S3 上实现一个能接收消息、构建上下文、调用 LLM、执行工具、保存会话并回复用户的轻量 agent。

## 入口文件

核心文件：

- `main/mimi.c`: 系统启动顺序，初始化 agent 依赖并启动任务。
- `main/agent/agent_loop.c`: agent 主循环，负责 ReAct tool loop。
- `main/agent/context_builder.c`: system prompt 构建。
- `main/llm/llm_proxy.c`: LLM HTTP 请求、响应解析、tool_use 适配。
- `main/tools/tool_registry.c`: 工具注册、工具 schema 构建、工具分发。
- `main/bus/message_bus.c`: inbound/outbound FreeRTOS 队列。
- `main/memory/session_mgr.c`: 每个 chat 的会话历史 JSONL。
- `main/memory/memory_store.c`: 长期记忆和 daily notes。
- `main/skills/skill_loader.c`: SPIFFS skills 摘要加载。
- `main/cache/cache_store.c`: agent 本地 RAM KV cache。

## 总体架构

MimiClaw 的 agent 不是运行在 Linux 上的多进程框架，而是 FreeRTOS 下的单 agent task。它依赖队列把 Telegram、Feishu、WebSocket、cron 等输入统一成 `mimi_msg_t`，再由 agent loop 串行处理。

```text
Telegram / Feishu / WebSocket / Cron
        |
        v
message_bus inbound queue
        |
        v
agent_loop task
        |
        +--> context_builder 构建 system prompt
        +--> session_mgr 加载历史消息
        +--> llm_proxy 调 LLM
        +--> tool_registry 执行工具
        +--> session_mgr 保存最终对话
        |
        v
message_bus outbound queue
        |
        v
Telegram / Feishu / WebSocket reply
```

这个设计的特点是：

- channel 只负责收发消息，不直接控制硬件。
- agent loop 统一决定是否调用工具。
- 工具是 C 函数，通过 JSON schema 暴露给模型。
- 会话历史保存在 SPIFFS，agent 每轮只取最近 N 条。
- 大 buffer 尽量走 PSRAM，减少 internal SRAM 压力。

## 启动顺序

`app_main()` 先初始化底层依赖，再启动网络服务和 agent。

关键顺序：

1. `init_nvs()`
2. `esp_event_loop_create_default()`
3. `init_spiffs()`
4. `message_bus_init()`
5. `memory_store_init()`
6. `cache_store_init()`
7. `skill_loader_init()`
8. `session_mgr_init()`
9. `wifi_manager_init()`
10. `llm_proxy_init()`
11. `tool_registry_init()`
12. `agent_loop_init()`
13. `serial_cli_init()`
14. WiFi connected 后启动 `agent_loop_start()`

这里有几个设计点：

- SPIFFS 必须早于 memory、session、skills。
- cache 必须早于 skills summary 缓存使用。
- tool registry 在 agent loop start 前构建好 `tools_json`。
- Serial CLI 先启动，所以即使 WiFi 未连接，也能做本地配置和排障。

## 消息模型

消息结构在 `main/bus/message_bus.h`：

```c
typedef struct {
    char channel[16];
    char chat_id[96];
    char *content;
} mimi_msg_t;
```

字段含义：

- `channel`: 来源，如 `telegram`、`feishu`、`websocket`、`system`。
- `chat_id`: 回复目标，例如 Telegram chat id、Feishu open_id、WebSocket client id。
- `content`: heap 分配的消息文本，队列传递所有权。

消息所有权规则很重要：

- push inbound/outbound 后，bus 接管 `content`。
- pop 后，消费者负责 `free(msg.content)`。
- agent 处理完 inbound 消息后会释放原始内容。
- outbound dispatch 发送完后释放回复内容。

## Agent Loop 处理流程

`agent_loop_task()` 是核心。

每收到一条 inbound 消息，它执行：

1. 从 inbound queue 阻塞读取 `mimi_msg_t`。
2. 调 `context_build_system_prompt()` 构建 system prompt。
3. 追加当前 turn context，包括 `source_channel` 和 `source_chat_id`。
4. 调 `session_get_history_json()` 读取最近历史。
5. 把当前用户消息 append 到 `messages`。
6. 进入 ReAct tool loop。
7. 如果 LLM 正常结束，保存 user/final assistant 到 session。
8. 把最终回复推到 outbound queue。
9. 释放 inbound message。

简化伪代码：

```c
while (1) {
    msg = pop_inbound();

    system_prompt = build_system_prompt();
    append_turn_context_prompt(system_prompt, msg);

    messages = session_history(chat_id);
    messages.append({"role": "user", "content": msg.content});

    for (i = 0; i < MIMI_AGENT_MAX_TOOL_ITER; i++) {
        resp = llm_chat_tools(system_prompt, messages, tools_json);

        if (!resp.tool_use) {
            final_text = resp.text;
            break;
        }

        messages.append(assistant tool_use blocks);
        tool_results = execute_tools(resp.calls);
        messages.append(user tool_result blocks);
    }

    save_session(user, final_text);
    push_outbound(final_text);
}
```

## ReAct Tool Loop

当前 agent 使用的是经典 ReAct 风格：

```text
User message
  -> LLM
  -> tool_use
  -> execute tool
  -> tool_result
  -> LLM
  -> final answer
```

最大迭代次数由 `MIMI_AGENT_MAX_TOOL_ITER` 控制，当前配置是 10。

一次 tool use 的消息结构大致是：

```text
assistant:
  content: [
    {"type": "text", "text": "..."},
    {"type": "tool_use", "id": "...", "name": "...", "input": {...}}
  ]

user:
  content: [
    {"type": "tool_result", "tool_use_id": "...", "content": "..."}
  ]
```

这个结构贴近 Anthropic Messages API。OpenAI-compatible provider 会在 `llm_proxy.c` 内部做格式转换。

## Prompt 构建

`context_build_system_prompt()` 负责构建 system prompt。

它包含：

1. 固定身份：MimiClaw 是运行在 ESP32-S3 上的个人 AI assistant。
2. 内置工具说明：web_search、time、sensor、GPIO、servo、file、cron 等。
3. GPIO 和硬件安全规则。
4. Memory 使用规则。
5. Skills 使用规则。
6. `/spiffs/config/SOUL.md`
7. `/spiffs/config/USER.md`
8. long-term memory: `/spiffs/memory/MEMORY.md`
9. recent notes: 最近 3 天 daily notes。
10. skills summary: `/spiffs/skills/*.md` 摘要。

当前 turn context 不放在 `context_builder.c` 里，而是在 agent loop 里追加：

```text
## Current Turn Context
- source_channel: ...
- source_chat_id: ...
```

这样可以把相对稳定的系统 prompt 和每轮动态信息分开，后续更利于 provider prompt cache。

## Tools 机制

工具定义在 `mimi_tool_t`：

```c
typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;
    esp_err_t (*execute)(const char *input_json, char *output, size_t output_size);
} mimi_tool_t;
```

每个工具有四个部分：

- `name`: 给模型调用的工具名。
- `description`: 告诉模型什么时候使用。
- `input_schema_json`: JSON schema，描述参数。
- `execute`: 实际 C 函数。

`tool_registry_init()` 注册所有内置工具，然后调用 `build_tools_json()` 一次性生成 `s_tools_json`。

这个 `s_tools_json` 会被 agent loop 复用，不会每轮重新构建。这本身就是一个 prompt/tool schema 稳定化机制。

当前工具大类：

- 网络: `web_search`
- 时间: `get_current_time`
- 文件: `read_file`、`write_file`、`edit_file`、`list_dir`
- GPIO: `gpio_write`、`gpio_read`、`gpio_read_all`
- RGB: `set_status_light`、`ws2812_set`
- 舵机: `servo_write`
- 传感器: AHT10、SGP30、BH1750、HC-SR05/presence
- 计划任务: `cron_add`、`cron_list`、`cron_remove`

## Tool Guard

`agent_loop.c` 里还有一层 tool guard。它不是模型 prompt，而是固件侧硬规则。

例如：

- 用户没有明确问灯，就阻止 `set_status_light` / `ws2812_set`。
- 用户没有明确问 GPIO，就阻止 `gpio_write`。
- 用户没有明确问 presence/distance，就阻止 `read_presence`。
- 用户没有明确问 cron/reminder，就阻止 `cron_add`。

guard 被触发时，不执行真实工具，而是把一段 tool_result 返回给模型，要求模型说明能力不匹配或询问澄清。

这对 IoT 项目很关键，因为模型可能会“联想”调用相近工具。固件侧 guard 可以减少误触发硬件动作。

## LLM Proxy

`llm_chat_tools()` 负责：

1. 构建 HTTP body。
2. 根据 provider 选择 Anthropic 或 OpenAI-compatible 格式。
3. 加入 system prompt、messages、tools。
4. 发 HTTPS 请求。
5. 解析 JSON 响应。
6. 输出统一的 `llm_response_t`。

统一响应结构：

```c
typedef struct {
    char *text;
    size_t text_len;
    llm_tool_call_t calls[MIMI_MAX_TOOL_CALLS];
    int call_count;
    bool tool_use;
} llm_response_t;
```

Anthropic 判断：

- `stop_reason == "tool_use"` 表示需要工具。
- `content[]` 里解析 `text` 和 `tool_use` blocks。

OpenAI-compatible 判断：

- `finish_reason == "tool_calls"` 表示需要工具。
- `message.tool_calls[]` 里解析 tool name 和 arguments。

agent loop 不需要关心 provider 差异，只处理 `llm_response_t`。

## Session 记忆

短期会话历史由 `session_mgr.c` 管理。

文件路径格式：

```text
/spiffs/sessions/tg_<chat_id>.jsonl
```

每行是一个 JSON：

```json
{"role":"user","content":"...","ts":1234567890}
```

读取历史时：

- 用 ring buffer 保留最近 `max_msgs` 条。
- 输出成 LLM 需要的 messages array。
- 只保留 `role` 和 `content`，不把 `ts` 发给模型。

保存历史时：

- 当前实现只保存用户原始消息和最终 assistant 文本。
- 中间 tool_use/tool_result 不写入 session。

这个取舍可以节省 SPIFFS 和 token，但缺点是下一轮无法看到完整工具调用轨迹，只能看到最终回答。

## Long-term Memory

长期记忆由 `memory_store.c` 提供，prompt 里会读：

- `/spiffs/memory/MEMORY.md`
- 最近 3 天 daily notes

system prompt 会告诉模型主动用 file tools 写 memory：

- 先 `read_file MEMORY.md`
- 再 `edit_file` 或 `write_file`
- 需要日期时先 `get_current_time`

也就是说，当前长期记忆不是 agent loop 自动写，而是通过模型调用 SPIFFS 文件工具实现。

## Skills 机制

skills 是放在 SPIFFS 的 markdown 文件：

```text
/spiffs/skills/*.md
```

`skill_loader_build_summary()` 会扫描这些文件，取标题和描述，加入 system prompt 的 `Available Skills` 区域。

当任务匹配某个 skill 时，模型可以通过 `read_file` 读取完整 skill 文件。

近期加入了 `prompt:skills_summary` RAM cache：

- 首次构建 summary 时扫描 SPIFFS。
- 后续 300 秒内命中 cache。
- `write_file` / `edit_file` 修改 `/spiffs/skills/` 时自动失效。

这减少了每轮 agent 重扫 SPIFFS 的开销。

## Cache 在 Agent 中的位置

当前 cache 是应用层 KV cache，不是模型内部 tensor KV cache。

已接入：

- `prompt:skills_summary`

CLI 可观测：

```text
cache_stats
cache_clear
```

`cache_stats` 会显示：

```text
Cache entries
Cache bytes
Cache hits
Cache misses
Cache hit rate
Cache evictions
Cache expired
```

命中率计算：

```text
hits / (hits + misses)
```

后续适合接入：

- MCP `tools/list`
- web_search 结果摘要
- 非安全类传感器短 TTL 读数
- prompt stable block hash

## 当前 Agent 的优点

1. 结构清晰  
   channel、bus、agent、tool、LLM、memory 边界比较明确。

2. MCU 友好  
   没有复杂 runtime，主要是 C、FreeRTOS queue、SPIFFS、PSRAM buffer。

3. 工具机制完整  
   有工具 schema、tool_use 解析、tool_result 回填、多轮迭代。

4. 硬件安全意识较强  
   prompt 有硬件约束，固件侧还有 tool guard。

5. provider 差异被封装  
   agent loop 只看统一 `llm_response_t`。

6. 已具备 skills / memory / cron / file tools  
   这让它已经不是简单 chatbot，而是有本地状态和执行能力的 IoT agent。

## 当前限制

1. Agent 是单 worker 串行处理  
   一个长 LLM 请求会阻塞后续 inbound 消息。

2. 没有真正的 subagent  
   cron 可以注入消息，但还没有独立背景 agent 实例。

3. session 只保存最终对话  
   tool_use/tool_result 不持久化，调试和长期上下文会少一部分信息。

4. memory 写入依赖模型主动调用文件工具  
   没有固件侧强制的 memory consolidation。

5. tool guard 是关键词规则  
   简单可靠，但会有误拦截或漏拦截，需要持续调词。

6. system prompt 较大且集中在 C 字符串中  
   修改不够灵活，后续可以拆到 SPIFFS bootstrap 文件或 generated prompt fragments。

7. 缓存还处于第一阶段  
   目前只缓存 skills summary，还没缓存 MCP tools list、web search、prompt hash。

## 学习路线

建议按这个顺序读代码：

1. `main/bus/message_bus.h`  
   先理解消息结构和所有权。

2. `main/mimi.c`  
   看初始化顺序，明确每个子系统什么时候可用。

3. `main/agent/agent_loop.c`  
   重点看 `agent_loop_task()`、`build_tool_results()`、`tool_guard_check()`。

4. `main/agent/context_builder.c`  
   理解 system prompt 由哪些稳定块和动态块组成。

5. `main/tools/tool_registry.c`  
   学会新增一个 AI-callable tool 的完整路径。

6. `main/llm/llm_proxy.c`  
   看 provider 适配、请求体构造、tool_use 解析。

7. `main/memory/session_mgr.c`  
   理解短期会话如何保存和回放。

8. `main/skills/skill_loader.c` 与 `main/cache/cache_store.c`  
   理解 skills 和 cache 如何降低 prompt 构建成本。

## 新增一个工具的最小路径

如果要新增一个传感器或动作工具，路径通常是：

1. 在 `main/tools/` 新建 `tool_xxx.c/h`。
2. 实现：

```c
esp_err_t tool_xxx_execute(const char *input_json, char *output, size_t output_size);
```

3. 在 `main/tools/tool_registry.c` include 头文件。
4. 在 `tool_registry_init()` 注册工具名、description、schema、execute。
5. 在 `main/CMakeLists.txt` 加入新的 `.c`。
6. 在 `context_builder.c` 增加工具使用 guidance。
7. 如果是硬件动作，考虑在 `agent_loop.c` 加 tool guard。
8. 通过 CLI `tool_exec <name> <json>` 单测工具。
9. 再通过 Telegram/Feishu/WebSocket 端到端测试。

## Agent 部分的核心心智模型

可以把 MimiClaw agent 理解成四层：

```text
1. Transport layer
   Telegram / Feishu / WebSocket / Cron

2. Agent orchestration layer
   message_bus + agent_loop + session

3. Reasoning interface layer
   context_builder + llm_proxy + tools_json

4. Action layer
   tool_registry + concrete tools + hardware drivers + SPIFFS
```

学习时不要一开始陷进某个传感器驱动。先把消息怎么进来、prompt 怎么构建、工具怎么调用、结果怎么返回这条链打通，后面新增 MCP、缓存、传感器工具都会更容易。
