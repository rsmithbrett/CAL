#pragma once
// The two partition-table calls App/Loader.cpp makes, and nothing else.
struct esp_partition_t {};
constexpr int ESP_PARTITION_TYPE_APP = 0;
constexpr int ESP_PARTITION_SUBTYPE_APP_FACTORY = 0;
const esp_partition_t* esp_partition_find_first(int type, int subtype, const char* label);
