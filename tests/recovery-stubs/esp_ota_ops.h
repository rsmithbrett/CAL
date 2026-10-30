#pragma once
struct esp_partition_t {};
constexpr int ESP_OK = 0;
constexpr int ESP_PARTITION_TYPE_APP = 0;
constexpr int ESP_PARTITION_SUBTYPE_APP_FACTORY = 0;
const esp_partition_t* esp_partition_find_first(int, int, const char*);
int esp_ota_set_boot_partition(const esp_partition_t* partition);
