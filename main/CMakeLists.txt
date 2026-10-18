set(embed "")
if(CONFIG_EMU_WEB)
    list(APPEND embed "web/macfiles.js")
endif()

idf_component_register(SRCS "main.c" "settings.c" "rom_store.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES ${embed}
                       PRIV_REQUIRES emu mac3 esp_partition nvs_flash esp_psram esp_timer)
