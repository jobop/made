#pragma once
#include <cstddef>
#include <cstring>
#include <string>
#include <map>
using nvs_handle_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, NVS_READONLY=0, NVS_READWRITE=1;
inline std::map<std::string,std::string> stored_values, pending_values;
inline int commits=0;
inline bool fail_commit=false;
inline int nvs_open(const char *name,int mode,nvs_handle_t *out) { if(std::strcmp(name,"made_i18n")) return ESP_FAIL;if(mode==NVS_READWRITE)pending_values=stored_values;*out=1;return ESP_OK; }
inline void nvs_close(nvs_handle_t) {}
inline int nvs_get_str(nvs_handle_t,const char *key,char *out,size_t *length) { auto it=stored_values.find(key);if(it==stored_values.end()||it->second.size()+1>*length)return ESP_FAIL;std::memcpy(out,it->second.c_str(),it->second.size()+1);*length=it->second.size()+1;return ESP_OK; }
inline int nvs_set_str(nvs_handle_t,const char *key,const char *value) { if(std::strcmp(key,"locale")&&std::strcmp(key,"follow_bridge")&&std::strcmp(key,"bridge_locale"))return ESP_FAIL;pending_values[key]=value;return ESP_OK; }
inline int nvs_commit(nvs_handle_t) { if(fail_commit)return ESP_FAIL;stored_values=pending_values;++commits;return ESP_OK; }
