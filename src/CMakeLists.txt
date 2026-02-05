idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash esp_event bt esp_wifi esp_netif esp_http_server
)

# Optimize for size to reduce IRAM usage
target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
