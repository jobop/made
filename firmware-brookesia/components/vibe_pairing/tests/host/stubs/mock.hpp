
#pragma once
#include <string>
#include <cstdint>
#include <cstring>
#include <map>
#include <functional>
#include <cassert>
#include <iostream>
#include <deque>
#include <vector>
#include <utility>
#include <algorithm>
#if defined(__APPLE__)
#include <CommonCrypto/CommonHMAC.h>
#else
#include <openssl/hmac.h>
#endif
inline void test_hmac(const void *key, size_t key_size, const void *data, size_t data_size, unsigned char *out) {
#if defined(__APPLE__)
 CCHmac(kCCHmacAlgSHA256, key, key_size, data, data_size, out);
#else
 unsigned int length = 0;
 HMAC(EVP_sha256(), key, static_cast<int>(key_size), static_cast<const unsigned char *>(data), data_size, out, &length);
 assert(length == 32);
#endif
}
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "cJSON.h"
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=1;
inline const char *esp_err_to_name(int) { return "mock"; }
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
constexpr int ESP_MAC_WIFI_STA=0;
inline int esp_read_mac(uint8_t *m,int) { std::memcpy(m,"123456",6); return ESP_OK; }
inline uint64_t mock_clock=10000000;
inline int64_t esp_timer_get_time() { return mock_clock; }
inline void esp_fill_random(void *p,size_t n) { static uint8_t next=1; auto *b=static_cast<uint8_t*>(p); for(size_t i=0;i<n;i++) b[i]=next++; }
struct esp_sntp_config_t { void(*sync_cb)(timeval*); };
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(a) esp_sntp_config_t{}
inline int esp_netif_sntp_init(esp_sntp_config_t*) { return ESP_OK; }
inline void *esp_crt_bundle_attach=nullptr;
constexpr int HTTP_EVENT_ON_DATA=1,HTTP_METHOD_POST=1;
struct esp_http_client_event_t { int event_id; void *user_data; int data_len; void *data; };
struct esp_http_client_config_t { const char *url; bool disable_auto_redirect; void *crt_bundle_attach; int timeout_ms; int(*event_handler)(esp_http_client_event_t*); void *user_data; };
struct HttpClient {
 esp_http_client_config_t config;
 std::string url,body;
 int status=0;
};
using esp_http_client_handle_t=HttpClient*;
inline int network_http_calls=0;
inline std::vector<std::string> http_urls;
inline std::function<void()> during_http;
inline std::function<std::pair<int,std::string>(const std::string &)> http_response;
inline HttpClient *esp_http_client_init(esp_http_client_config_t *config) {
 ++network_http_calls;
 auto *client=new HttpClient;
 client->config=*config; client->url=config->url;
 return client;
}
inline void esp_http_client_set_header(HttpClient*,const char *name,const char*) { assert(std::string(name)!="Authorization"); }
inline void esp_http_client_set_method(HttpClient*,int) {}
inline void esp_http_client_set_post_field(HttpClient *client,const char *body,size_t length) { client->body.assign(body,length); }
inline int esp_http_client_perform(HttpClient *client) {
 http_urls.push_back(client->url);
 if(during_http) { auto hook=std::move(during_http); during_http=nullptr; hook(); }
 auto response=http_response ? http_response(client->url) :
   std::make_pair(client->url.find("/pair/request")!=std::string::npos ? 202 : 404,std::string("{}"));
 client->status=response.first;
 esp_http_client_event_t event{HTTP_EVENT_ON_DATA,client->config.user_data,static_cast<int>(response.second.size()),response.second.data()};
 return client->config.event_handler(&event);
}
inline int esp_http_client_get_status_code(HttpClient *client) { return client->status; }
inline void esp_http_client_cleanup(HttpClient *client) { delete client; }
using nvs_handle_t=int;
constexpr int NVS_READONLY=0,NVS_READWRITE=1;
inline std::map<std::string,std::string> nvs;
inline int nvs_open(const char*,int,int *h) { *h=1; return ESP_OK; }
inline void nvs_close(int) {}
inline int nvs_get_str(int,const char *key,char *p,size_t *n) { auto it=nvs.find(key); if(it==nvs.end()||it->second.size()+1>*n) return ESP_FAIL; *n=it->second.size()+1; std::memcpy(p,it->second.c_str(),*n); return ESP_OK; }
inline int nvs_get_u8(int,const char *key,uint8_t *v) { if(!nvs.count(key)) return ESP_FAIL; *v=std::stoi(nvs[key]); return ESP_OK; }
inline int nvs_set_str(int,const char *key,const char *v) { nvs[key]=v; return ESP_OK; }
inline int nvs_set_u8(int,const char *key,uint8_t v) { nvs[key]=std::to_string(v); return ESP_OK; }
inline int nvs_commit(int) { return ESP_OK; }
inline int nvs_erase_key(int,const char *key) { nvs.erase(key); return ESP_OK; }
constexpr int MBEDTLS_MD_SHA256=1;
struct mbedtls_md_info_t{};
inline const mbedtls_md_info_t* mbedtls_md_info_from_type(int) { static mbedtls_md_info_t s; return &s; }
inline int mbedtls_md_hmac(const mbedtls_md_info_t*,const unsigned char *key,size_t kl,const unsigned char *data,size_t dl,unsigned char *out) { test_hmac(key,kl,data,dl,out); return 0; }
namespace vibe_wifi { inline bool receiver=false; inline int reads=0; inline bool receiver_active() { ++reads; return receiver; } }
namespace vibe_usb {
inline bool enabled=true,online=true;
inline uint32_t epoch=1,gate=UINT32_MAX;
inline std::string gate_bearer;
inline bool active() { return enabled; }
inline bool connected() { return online; }
inline uint32_t connection_epoch() { return epoch; }
inline bool authorize_connection(uint32_t wanted,const std::string &bearer) { if(!enabled||!online||wanted!=epoch) return false; gate=wanted; gate_bearer=bearer; return true; }
inline void revoke_authorization() { gate=UINT32_MAX; gate_bearer.clear(); }
inline std::string computer_token(64,'b'),computer_bridge="pc_b";
inline bool approve=false;
inline int verify_calls=0,pair_calls=0,status_calls=0;
inline std::string last_code;
inline std::function<void()> during_request;
inline std::string hmac(const std::string &input) {
 uint8_t key[32],out[32];
 for(size_t i=0;i<32;i++) key[i]=std::stoi(computer_token.substr(i*2,2),nullptr,16);
 test_hmac(key,32,input.data(),input.size(),out);
 std::string result; char b[3]; for(uint8_t v:out) { std::snprintf(b,3,"%02x",v); result+=b; } return result;
}
inline esp_err_t request(const std::string &path,bool post,const std::string &authorization,const char*,const uint8_t *body,size_t length,std::string &response,int &status,size_t,int) {
 assert(authorization.empty());
 if(during_request) { auto hook=std::move(during_request); during_request=nullptr; hook(); }
 if(path.rfind("/pair/verify?",0)==0) {
  ++verify_calls; assert(!post);
  auto start=path.find("deviceId=")+9,stop=path.find('&',start),ns=path.find("nonce=")+6;
  std::string text="VIBE_BRIDGE_MANUAL_V2\n"+path.substr(start,stop-start)+"\n"+path.substr(ns)+"\n"+computer_bridge+"\nusb.vibe.local:8788";
  response="{\"bridgeId\":\""+computer_bridge+"\",\"authority\":\"usb.vibe.local:8788\",\"mac\":\""+hmac(text)+"\"}"; status=200;
 } else if(path=="/pair/request") {
  ++pair_calls; assert(post); auto *j=cJSON_Parse(std::string(reinterpret_cast<const char*>(body),length).c_str()); assert(j); last_code=cJSON_GetObjectItem(j,"code")->valuestring; cJSON_Delete(j); response="{}"; status=202;
 } else if(path.rfind("/pair/status?",0)==0) {
  ++status_calls; response=approve ? "{\"status\":\"approved\",\"bridgeId\":\""+computer_bridge+"\",\"token\":\""+computer_token+"\"}" : "{\"status\":\"pending\"}"; status=200;
 } else assert(false);
 return ESP_OK;
}
}

struct Datagram { std::string address,body; };
inline std::deque<Datagram> packets;
inline std::vector<std::string> udp_requests;
inline std::function<void(const std::string &)> on_discovery;
inline std::function<void()> during_receive;
inline uint64_t socket_timeout_us=200000;
inline bool socket_available=true;
inline int sockets_opened=0,sockets_closed=0;
inline int mock_socket(int,int,int) { if(!socket_available) return -1; ++sockets_opened; return 77; }
inline int mock_setsockopt(int,int,int opt,const void *value,socklen_t) {
 if(opt==SO_RCVTIMEO) socket_timeout_us=static_cast<const timeval*>(value)->tv_usec;
 return 0;
}
inline ssize_t mock_sendto(int,const void *body,size_t length,int,const sockaddr*,socklen_t) {
 const std::string request(static_cast<const char*>(body),length);
 udp_requests.push_back(request);
 if(on_discovery) on_discovery(request);
 return static_cast<ssize_t>(length);
}
inline ssize_t mock_recvfrom(int,void *buffer,size_t size,int,sockaddr *sender,socklen_t*) {
 if(during_receive) { auto hook=std::move(during_receive); during_receive=nullptr; hook(); }
 if(packets.empty()) { mock_clock+=socket_timeout_us; return -1; }
 auto packet=std::move(packets.front()); packets.pop_front();
 mock_clock+=1000;
 auto *address=reinterpret_cast<sockaddr_in*>(sender); address->sin_family=AF_INET;
 assert(inet_pton(AF_INET,packet.address.c_str(),&address->sin_addr)==1);
 const size_t copied=std::min(size,packet.body.size()); std::memcpy(buffer,packet.body.data(),copied);
 return static_cast<ssize_t>(copied);
}
inline int mock_close(int fd) { assert(fd==77); ++sockets_closed; return 0; }
