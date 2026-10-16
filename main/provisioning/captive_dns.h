// Tiny DNS server for the captive portal: answers every A query with 192.168.4.1.
#pragma once
#include "esp_err.h"
esp_err_t captive_dns_start(void);
void      captive_dns_stop(void);
