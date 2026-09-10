#pragma once
#include "esp_err.h"
#include <cstddef>
struct mdns_txt_item_t { const char* key; const char* value; };
struct mdns_result_t { mdns_result_t* next; const char* instance_name; unsigned short port; int txt_count; mdns_txt_item_t* txt; };
extern mdns_result_t* supplied;
extern int queries;
inline int mdns_init(){return ESP_OK;}
inline int mdns_query_ptr(const char*,const char*,int,int,mdns_result_t** out){++queries;*out=supplied;return ESP_OK;}
inline void mdns_query_results_free(mdns_result_t*){}
