set(LINX_VOICE_SOURCES "")
if(CONFIG_LINX_VOICE_ENABLE)
    list(APPEND LINX_VOICE_SOURCES
         "linx_device.c"
         "linx_wifi_portal.c")
endif()

idf_component_register(
    SRCS "main.c"
         "linx_input.c"
         "linx_input_settings.c"
         "linx_demo.c"
         "linx_content.c"
         "linx_navigation.c"
         "linx_reader.c"
         "linx_ui.c"
         "linx_wifi.c"
         "linx_protocol.c"
         ${LINX_VOICE_SOURCES}
         "../assets/fonts/linx_font_12.c"
         "../assets/fonts/linx_font_14.c"
         "../assets/fonts/linx_font_16.c"
         "../assets/fonts/linx_font_20.c"
         "../assets/fonts/linx_font_16_bold.c"
         "../assets/fonts/linx_font_20_bold.c"
         "../assets/fonts/linx_font_24_bold.c"
         "../assets/fonts/linx_font_28_bold.c"
    INCLUDE_DIRS "." "${AI_PASSPORT_DIR}/main"
    REQUIRES bsp esp_timer json esp_wifi esp_netif esp_event nvs_flash lwip
             esp_http_client esp_http_server tcp_transport esp-tls esp_app_format esp_audio_codec
)
