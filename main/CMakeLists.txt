idf_component_register(
    SRCS
        "mimi.c"
        "bus/message_bus.c"
        "wifi/wifi_manager.c"
        "channels/telegram/telegram_bot.c"
        "channels/feishu/feishu_bot.c"
        "llm/llm_proxy.c"
        "agent/agent_loop.c"
        "agent/context_builder.c"
        "memory/memory_store.c"
        "memory/session_mgr.c"
        "gateway/ws_server.c"
        "cli/serial_cli.c"
        "proxy/http_proxy.c"
        "cron/cron_service.c"
        "heartbeat/heartbeat.c"
        "tools/tool_registry.c"
        "tools/tool_cron.c"
        "tools/tool_web_search.c"
        "tools/tool_get_time.c"
        "tools/tool_files.c"
        "tools/tool_gpio.c"
<<<<<<< HEAD
        "tools/tool_sgp30.c"
=======
        "tools/gpio_policy.c"
>>>>>>> bb10ea0149080d506d920c09054f4c5b20409de2
        "skills/skill_loader.c"
        "onboard/wifi_onboard.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_http_client esp_http_server
        esp_https_ota esp_event json spiffs console vfs app_update esp-tls
<<<<<<< HEAD
        esp_timer esp_websocket_client esp_driver_gpio esp_driver_rmt esp_driver_i2c
=======
        esp_timer esp_websocket_client esp_driver_gpio
>>>>>>> bb10ea0149080d506d920c09054f4c5b20409de2
)
