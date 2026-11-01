#pragma once
// esp_restart() is declared here rather than in an esp_system.h stub because
// App/Loader.cpp calls it and includes only this header, matching what the
// Arduino core pulls in transitively.
#include "esp_partition.h"
constexpr int ESP_OK = 0;
int esp_ota_set_boot_partition(const esp_partition_t* partition);
[[noreturn]] void esp_restart();
