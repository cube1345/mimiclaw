#include "agent_loop.h"
#include "agent/context_builder.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "memory/session_mgr.h"
#include "tools/tool_registry.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "agent";

#define TOOL_OUTPUT_SIZE  (8 * 1024)

static bool contains_substr_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || needle[0] == '\0') {
        return false;
    }

    const size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i]) {
            unsigned char hc = (unsigned char)p[i];
            unsigned char nc = (unsigned char)needle[i];

            if (hc >= 'A' && hc <= 'Z') hc = (unsigned char)(hc - 'A' + 'a');
            if (nc >= 'A' && nc <= 'Z') nc = (unsigned char)(nc - 'A' + 'a');
            if (hc != nc) {
                break;
            }
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }

    return false;
}

static bool message_has_any_keyword(const char *message, const char *const *keywords, size_t keyword_count)
{
    if (!message || !keywords) {
        return false;
    }

    for (size_t i = 0; i < keyword_count; i++) {
        if (contains_substr_ci(message, keywords[i])) {
            return true;
        }
    }

    return false;
}

static bool tool_guard_match_light_sensor_request(const char *message)
{
    static const char *const topic_keywords[] = {
        "light level", "ambient light", "illuminance", "lux", "gy-30", "gy30", "bh1750",
        "light sensor", "brightness sensor",
        "光照", "光线", "亮度", "照度", "勒克斯", "光传感器", "gy-30", "gy30", "bh1750",
    };
    static const char *const intent_keywords[] = {
        "read", "check", "measure", "detect", "show", "status", "how bright", "how much",
        "读取", "检测", "测量", "查看", "读一下", "多少", "数值", "状态", "亮不亮",
    };

    if (contains_substr_ci(message, "gy-30") ||
        contains_substr_ci(message, "gy30") ||
        contains_substr_ci(message, "bh1750") ||
        contains_substr_ci(message, "lux") ||
        contains_substr_ci(message, "光照") ||
        contains_substr_ci(message, "照度")) {
        return true;
    }

    return message_has_any_keyword(message, topic_keywords, sizeof(topic_keywords) / sizeof(topic_keywords[0])) &&
           message_has_any_keyword(message, intent_keywords, sizeof(intent_keywords) / sizeof(intent_keywords[0]));
}

static bool tool_guard_match_light_request(const char *message)
{
    static const char *const keywords[] = {
        "light", "led", "rgb", "ws2812", "neopixel", "status light", "board light",
        "颜色", "灯", "灯光", "亮灯", "板载灯", "彩灯", "状态灯", "rgb灯", "ws2812",
    };
    if (tool_guard_match_light_sensor_request(message)) {
        return false;
    }
    return message_has_any_keyword(message, keywords, sizeof(keywords) / sizeof(keywords[0]));
}

static bool tool_guard_match_air_quality_request(const char *message)
{
    static const char *const topic_keywords[] = {
        "air quality", "tvoc", "voc", "eco2", "co2", "sgp30", "indoor air", "gas sensor",
        "空气质量", "空气传感器", "气体传感器", "气体数据", "voc", "tvoc", "eco2", "co2", "sgp30",
    };
    static const char *const intent_keywords[] = {
        "read", "check", "measure", "detect", "show", "status", "how is", "what is",
        "读取", "检测", "测量", "查看", "读一下", "怎么样", "多少", "数值", "状态",
    };

    if (contains_substr_ci(message, "sgp30")) {
        return true;
    }

    return message_has_any_keyword(message, topic_keywords, sizeof(topic_keywords) / sizeof(topic_keywords[0])) &&
           message_has_any_keyword(message, intent_keywords, sizeof(intent_keywords) / sizeof(intent_keywords[0]));
}

static bool tool_guard_match_presence_request(const char *message)
{
    static const char *const topic_keywords[] = {
        "presence", "human sensor", "person", "someone", "anyone", "nearby", "proximity",
        "distance sensor", "ultrasonic", "hc-sr05", "hcsr05", "hc sr05", "trig", "echo",
        "人体传感器", "人体", "有人", "人靠近", "靠近", "距离", "测距", "超声波", "障碍", "hc-sr05", "hcsr05",
    };
    static const char *const intent_keywords[] = {
        "read", "check", "measure", "detect", "show", "status", "is there", "is anyone", "is someone",
        "读取", "检测", "测量", "查看", "读一下", "有没有", "有人吗", "靠近吗", "多少", "状态",
    };

    if (contains_substr_ci(message, "hc-sr05") ||
        contains_substr_ci(message, "hcsr05") ||
        contains_substr_ci(message, "人体传感器")) {
        return true;
    }

    return message_has_any_keyword(message, topic_keywords, sizeof(topic_keywords) / sizeof(topic_keywords[0])) &&
           message_has_any_keyword(message, intent_keywords, sizeof(intent_keywords) / sizeof(intent_keywords[0]));
}

static bool tool_guard_match_gpio_write_request(const char *message)
{
    static const char *const keywords[] = {
        "gpio", "pin", "io", "output", "relay", "mosfet", "high", "low", "pull high", "pull low",
        "digital output", "引脚", "脚位", "io口", "输出", "继电器", "高电平", "低电平", "拉高", "拉低",
    };
    return message_has_any_keyword(message, keywords, sizeof(keywords) / sizeof(keywords[0]));
}

static bool tool_guard_match_gpio_read_request(const char *message)
{
    static const char *const keywords[] = {
        "gpio", "pin", "io", "read pin", "button", "switch", "input", "state", "level",
        "引脚", "脚位", "io口", "读取引脚", "按钮", "开关", "输入", "状态", "电平",
    };
    return message_has_any_keyword(message, keywords, sizeof(keywords) / sizeof(keywords[0]));
}

static bool tool_guard_match_cron_request(const char *message)
{
    static const char *const keywords[] = {
        "cron", "schedule", "scheduled", "timer", "timed", "remind", "reminder", "every", "later",
        "定时", "计划任务", "提醒", "定时任务", "稍后", "每隔", "到点",
    };
    return message_has_any_keyword(message, keywords, sizeof(keywords) / sizeof(keywords[0]));
}

static bool tool_guard_match_servo_request(const char *message)
{
    static const char *const keywords[] = {
        "servo", "angle", "rotate", "rotation", "turn servo", "open servo", "start servo",
        "move servo", "test servo", "steer", "steering", "pwm", "pulse width",
        "舵机", "角度", "旋转", "转动", "顺时针", "逆时针", "脉宽",
        "打开舵机", "开舵机", "启动舵机", "舵机打开", "舵机动一下", "让舵机动", "测试舵机",
    };
    return message_has_any_keyword(message, keywords, sizeof(keywords) / sizeof(keywords[0]));
}

static bool tool_guard_check(const llm_tool_call_t *call, const mimi_msg_t *msg,
                             char *output, size_t output_size)
{
    if (!call || !msg || !msg->content || call->name[0] == '\0') {
        return true;
    }

    const char *tool_name = call->name;
    const char *message = msg->content;
    bool allowed = true;
    const char *expected = NULL;

    if (strcmp(tool_name, "read_light_level") == 0) {
        allowed = tool_guard_match_light_sensor_request(message);
        expected = "ambient-light, illuminance, lux, or GY-30/BH1750 sensor reading";
    } else if (strcmp(tool_name, "set_status_light") == 0 || strcmp(tool_name, "ws2812_set") == 0) {
        allowed = tool_guard_match_light_request(message);
        expected = "board light or LED control";
    } else if (strcmp(tool_name, "servo_write") == 0) {
        allowed = tool_guard_match_servo_request(message);
        expected = "servo angle or servo pulse-width control";
    } else if (strcmp(tool_name, "read_air_quality") == 0 || strcmp(tool_name, "sgp30_read_air_quality") == 0) {
        allowed = tool_guard_match_air_quality_request(message);
        expected = "air-quality or gas-sensor reading";
    } else if (strcmp(tool_name, "read_presence") == 0 || strcmp(tool_name, "hc_sr05_read_distance") == 0) {
        allowed = tool_guard_match_presence_request(message);
        expected = "HC-SR05 presence, proximity, or distance reading";
    } else if (strcmp(tool_name, "gpio_write") == 0) {
        allowed = tool_guard_match_gpio_write_request(message);
        expected = "explicit GPIO or digital output control";
    } else if (strcmp(tool_name, "gpio_read") == 0 || strcmp(tool_name, "gpio_read_all") == 0) {
        allowed = tool_guard_match_gpio_read_request(message);
        expected = "explicit GPIO, button, switch, or input-state reading";
    } else if (strcmp(tool_name, "cron_add") == 0 || strcmp(tool_name, "cron_list") == 0 ||
               strcmp(tool_name, "cron_remove") == 0) {
        allowed = tool_guard_match_cron_request(message);
        expected = "scheduling or reminder management";
    }

    if (allowed) {
        return true;
    }

    snprintf(output, output_size,
             "Guard blocked tool '%s': the user's request does not clearly ask for %s. "
             "Do not substitute a nearby capability. Reply that this capability is not currently supported, "
             "or ask a brief clarification question if the user intent is ambiguous.",
             tool_name, expected ? expected : "this tool");
    ESP_LOGW(TAG, "Tool guard blocked %s for message: %s", tool_name, message);
    return false;
}

/* Build the assistant content array from llm_response_t for the messages history.
 * Returns a cJSON array with text and tool_use blocks. */
static cJSON *build_assistant_content(const llm_response_t *resp)
{
    cJSON *content = cJSON_CreateArray();

    /* Text block */
    if (resp->text && resp->text_len > 0) {
        cJSON *text_block = cJSON_CreateObject();
        cJSON_AddStringToObject(text_block, "type", "text");
        cJSON_AddStringToObject(text_block, "text", resp->text);
        cJSON_AddItemToArray(content, text_block);
    }

    /* Tool use blocks */
    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        cJSON *tool_block = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_block, "type", "tool_use");
        cJSON_AddStringToObject(tool_block, "id", call->id);
        cJSON_AddStringToObject(tool_block, "name", call->name);

        cJSON *input = cJSON_Parse(call->input);
        if (input) {
            cJSON_AddItemToObject(tool_block, "input", input);
        } else {
            cJSON_AddItemToObject(tool_block, "input", cJSON_CreateObject());
        }

        cJSON_AddItemToArray(content, tool_block);
    }

    return content;
}

static void json_set_string(cJSON *obj, const char *key, const char *value)
{
    if (!obj || !key || !value) {
        return;
    }
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, value);
}

static void append_turn_context_prompt(char *prompt, size_t size, const mimi_msg_t *msg)
{
    if (!prompt || size == 0 || !msg) {
        return;
    }

    size_t off = strnlen(prompt, size - 1);
    if (off >= size - 1) {
        return;
    }

    int n = snprintf(
        prompt + off, size - off,
        "\n## Current Turn Context\n"
        "- source_channel: %s\n"
        "- source_chat_id: %s\n"
        "- If using cron_add for Telegram in this turn, set channel='telegram' and chat_id to source_chat_id.\n"
        "- Never use chat_id 'cron' for Telegram messages.\n",
        msg->channel[0] ? msg->channel : "(unknown)",
        msg->chat_id[0] ? msg->chat_id : "(empty)");

    if (n < 0 || (size_t)n >= (size - off)) {
        prompt[size - 1] = '\0';
    }
}

static char *patch_tool_input_with_context(const llm_tool_call_t *call, const mimi_msg_t *msg)
{
    if (!call || !msg || strcmp(call->name, "cron_add") != 0) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(call->input ? call->input : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (!root) {
        return NULL;
    }

    bool changed = false;

    cJSON *channel_item = cJSON_GetObjectItem(root, "channel");
    const char *channel = cJSON_IsString(channel_item) ? channel_item->valuestring : NULL;

    if ((!channel || channel[0] == '\0') && msg->channel[0] != '\0') {
        json_set_string(root, "channel", msg->channel);
        channel = msg->channel;
        changed = true;
    }

    if (channel && strcmp(channel, MIMI_CHAN_TELEGRAM) == 0 &&
        strcmp(msg->channel, MIMI_CHAN_TELEGRAM) == 0 && msg->chat_id[0] != '\0') {
        cJSON *chat_item = cJSON_GetObjectItem(root, "chat_id");
        const char *chat_id = cJSON_IsString(chat_item) ? chat_item->valuestring : NULL;
        if (!chat_id || chat_id[0] == '\0' || strcmp(chat_id, "cron") == 0) {
            json_set_string(root, "chat_id", msg->chat_id);
            changed = true;
        }
    }

    char *patched = NULL;
    if (changed) {
        patched = cJSON_PrintUnformatted(root);
        if (patched) {
            ESP_LOGI(TAG, "Patched cron_add target to %s:%s", msg->channel, msg->chat_id);
        }
    }

    cJSON_Delete(root);
    return patched;
}

/* Build the user message with tool_result blocks */
static cJSON *build_tool_results(const llm_response_t *resp, const mimi_msg_t *msg,
                                 char *tool_output, size_t tool_output_size)
{
    cJSON *content = cJSON_CreateArray();

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        const char *tool_input = call->input ? call->input : "{}";
        char *patched_input = patch_tool_input_with_context(call, msg);
        if (patched_input) {
            tool_input = patched_input;
        }

        tool_output[0] = '\0';
        if (!tool_guard_check(call, msg, tool_output, tool_output_size)) {
            free(patched_input);
            ESP_LOGI(TAG, "=== CONV === Tool[%s] => %s", call->name, tool_output);

            cJSON *result_block = cJSON_CreateObject();
            cJSON_AddStringToObject(result_block, "type", "tool_result");
            cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
            cJSON_AddStringToObject(result_block, "content", tool_output);
            cJSON_AddItemToArray(content, result_block);
            continue;
        }

        /* Execute tool */
        tool_registry_execute(call->name, tool_input, tool_output, tool_output_size);
        free(patched_input);

        ESP_LOGI(TAG, "=== CONV === Tool[%s] => %s", call->name, tool_output);

        /* Build tool_result block */
        cJSON *result_block = cJSON_CreateObject();
        cJSON_AddStringToObject(result_block, "type", "tool_result");
        cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
        cJSON_AddStringToObject(result_block, "content", tool_output);
        cJSON_AddItemToArray(content, result_block);
    }

    return content;
}

static void agent_loop_task(void *arg)
{
    ESP_LOGI(TAG, "Agent loop started on core %d", xPortGetCoreID());

    /* Allocate large buffers from PSRAM */
    char *system_prompt = heap_caps_calloc(1, MIMI_CONTEXT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *history_json = heap_caps_calloc(1, MIMI_LLM_STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *tool_output = heap_caps_calloc(1, TOOL_OUTPUT_SIZE, MALLOC_CAP_SPIRAM);

    if (!system_prompt || !history_json || !tool_output) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
        vTaskDelete(NULL);
        return;
    }

    const char *tools_json = tool_registry_get_tools_json();

    while (1) {
        mimi_msg_t msg;
        esp_err_t err = message_bus_pop_inbound(&msg, UINT32_MAX);
        if (err != ESP_OK) continue;

        ESP_LOGI(TAG, "Processing message from %s:%s", msg.channel, msg.chat_id);
        ESP_LOGI(TAG, "=== CONV ==================================================");
        ESP_LOGI(TAG, "=== CONV === [%s/%s] >> USER: %s",
                 msg.channel, msg.chat_id, msg.content);

        /* 1. Build system prompt */
        context_build_system_prompt(system_prompt, MIMI_CONTEXT_BUF_SIZE);
        append_turn_context_prompt(system_prompt, MIMI_CONTEXT_BUF_SIZE, &msg);
        ESP_LOGI(TAG, "LLM turn context: channel=%s chat_id=%s", msg.channel, msg.chat_id);

        /* 2. Load session history into cJSON array */
        session_get_history_json(msg.chat_id, history_json,
                                 MIMI_LLM_STREAM_BUF_SIZE, MIMI_AGENT_MAX_HISTORY);

        cJSON *messages = cJSON_Parse(history_json);
        if (!messages) messages = cJSON_CreateArray();

        /* 3. Append current user message */
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", msg.content);
        cJSON_AddItemToArray(messages, user_msg);

        /* 4. ReAct loop */
        char *final_text = NULL;
        int iteration = 0;
        bool sent_working_status = false;

        while (iteration < MIMI_AGENT_MAX_TOOL_ITER) {
            /* Send "working" indicator before each API call */
#if MIMI_AGENT_SEND_WORKING_STATUS
            if (!sent_working_status && strcmp(msg.channel, MIMI_CHAN_SYSTEM) != 0) {
                mimi_msg_t status = {0};
                strncpy(status.channel, msg.channel, sizeof(status.channel) - 1);
                strncpy(status.chat_id, msg.chat_id, sizeof(status.chat_id) - 1);
                status.content = strdup("\xF0\x9F\x90\xB1mimi is working...");
                if (status.content) {
                    if (message_bus_push_outbound(&status) != ESP_OK) {
                        ESP_LOGW(TAG, "Outbound queue full, drop working status");
                        free(status.content);
                    } else {
                        sent_working_status = true;
                    }
                }
            }
#endif

            llm_response_t resp;
            err = llm_chat_tools(system_prompt, messages, tools_json, &resp);

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "LLM call failed: %s", esp_err_to_name(err));
                break;
            }

            if (!resp.tool_use) {
                /* Normal completion — save final text and break */
                if (resp.text && resp.text_len > 0) {
                    final_text = strdup(resp.text);
                    ESP_LOGI(TAG, "=== CONV === << LLM: %.*s",
                             (int)resp.text_len, resp.text);
                }
                llm_response_free(&resp);
                break;
            }

            ESP_LOGI(TAG, "Tool use iteration %d: %d calls", iteration + 1, resp.call_count);
            for (int ci = 0; ci < resp.call_count; ci++) {
                ESP_LOGI(TAG, "=== CONV === << LLM tool[%d]: %s(%s)",
                         ci, resp.calls[ci].name, resp.calls[ci].input ? resp.calls[ci].input : "{}");
            }

            /* Append assistant message with content array */
            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");
            cJSON_AddItemToObject(asst_msg, "content", build_assistant_content(&resp));
            cJSON_AddItemToArray(messages, asst_msg);

            /* Execute tools and append results */
            cJSON *tool_results = build_tool_results(&resp, &msg, tool_output, TOOL_OUTPUT_SIZE);
            cJSON *result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "user");
            cJSON_AddItemToObject(result_msg, "content", tool_results);
            cJSON_AddItemToArray(messages, result_msg);

            llm_response_free(&resp);
            iteration++;
        }

        cJSON_Delete(messages);

        /* 5. Send response */
        if (final_text && final_text[0]) {
            /* Save to session (only user text + final assistant text) */
            esp_err_t save_user = session_append(msg.chat_id, "user", msg.content);
            esp_err_t save_asst = session_append(msg.chat_id, "assistant", final_text);
            if (save_user != ESP_OK || save_asst != ESP_OK) {
                ESP_LOGW(TAG, "Session save failed for chat %s (user=%s, assistant=%s)",
                         msg.chat_id,
                         esp_err_to_name(save_user),
                         esp_err_to_name(save_asst));
            } else {
                ESP_LOGI(TAG, "Session saved for chat %s", msg.chat_id);
            }

            /* Push response to outbound */
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = final_text;  /* transfer ownership */
            ESP_LOGI(TAG, "Queue final response to %s:%s (%d bytes)",
                     out.channel, out.chat_id, (int)strlen(final_text));
            ESP_LOGI(TAG, "=== CONV === >> RESPONSE [%s/%s]: %s",
                     out.channel, out.chat_id, out.content);
            if (message_bus_push_outbound(&out) != ESP_OK) {
                ESP_LOGW(TAG, "Outbound queue full, drop final response");
                free(final_text);
            } else {
                final_text = NULL;
            }
        } else {
            /* Error or empty response */
            free(final_text);
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = strdup("Sorry, I encountered an error.");
            if (out.content) {
                if (message_bus_push_outbound(&out) != ESP_OK) {
                    ESP_LOGW(TAG, "Outbound queue full, drop error response");
                    free(out.content);
                }
            }
        }

        /* Free inbound message content */
        free(msg.content);

        /* Log memory status */
        ESP_LOGI(TAG, "Free PSRAM: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

esp_err_t agent_loop_init(void)
{
    ESP_LOGI(TAG, "Agent loop initialized");
    return ESP_OK;
}

esp_err_t agent_loop_start(void)
{
    const uint32_t stack_candidates[] = {
        MIMI_AGENT_STACK,
        20 * 1024,
        16 * 1024,
        14 * 1024,
        12 * 1024,
    };

    for (size_t i = 0; i < (sizeof(stack_candidates) / sizeof(stack_candidates[0])); i++) {
        uint32_t stack_size = stack_candidates[i];
        BaseType_t ret = xTaskCreatePinnedToCore(
            agent_loop_task, "agent_loop",
            stack_size, NULL,
            MIMI_AGENT_PRIO, NULL, MIMI_AGENT_CORE);

        if (ret == pdPASS) {
            ESP_LOGI(TAG, "agent_loop task created with stack=%u bytes", (unsigned)stack_size);
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "agent_loop create failed (stack=%u, free_internal=%u, largest_internal=%u), retrying...",
                 (unsigned)stack_size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    return ESP_FAIL;
}
