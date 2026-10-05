#include "vibe_i18n.hpp"
#include "dictionary.hpp"
#include "nvs.h"
#include <cassert>
#include <set>
#include <thread>
#include <iostream>
using namespace vibe_i18n;
int main(int argc,char **argv) {
    if(argc>1){
      stored_values["locale"]=argv[1];
      if(argc>2)stored_values["follow_bridge"]=argv[2];
      if(argc>3)stored_values["bridge_locale"]=argv[3];
      initialize();assert(std::string(locale_name())==(std::string(argv[1])=="en"?"en":"zh-CN"));
      assert(follows_bridge()==(argc<3||std::string(argv[2])!="0"));assert(commits==0);
      if(argc>3){assert(set_follow_bridge());assert(locale()==(std::string(argv[3])=="en"?Locale::English:Locale::Chinese));}
      std::cout<<"PASS cold restore: language/mode/remembered bridge locale\n";return 0;
    }
    initialize();assert(locale()==Locale::Chinese&&follows_bridge());assert(std::string(tr("设置"))=="设置");
    Locale out=Locale::English;
    for(const char *bad:{"","EN","en-US","zh","zh-cn","fr"," en","en ","zh-CN\n"}){
      assert(!parse_locale(bad,out));assert(out==Locale::English);assert(!set_locale(bad));assert(!apply_bridge_locale(bad));
    }
    assert(!parse_locale(nullptr,out));assert(!apply_bridge_locale(nullptr));assert(commits==0&&revision()==0);
    assert(set_locale("zh-CN")&&!follows_bridge()&&commits==1&&revision()==1); // Same locale still persists explicit mode.
    assert(stored_values["locale"]=="zh-CN"&&stored_values["follow_bridge"]=="0");
    assert(set_locale("en")&&stored_values["locale"]=="en"&&commits==2&&revision()==2);
    assert(set_locale("en")&&commits==2); // Repeated settings do not wear NVS.
    assert(std::string(tr("设置"))=="Settings");assert(std::string(tr("settings.short"))=="Setup");
    assert(std::string(tr("settings.connection"))=="Connection");
    assert(std::string(tr("settings.language"))=="Language");
    assert(std::string(tr("settings.volume"))=="Volume");
    assert(message("Bridge HTTP 503")=="Bridge HTTP 503");
    assert(transport_error("电脑接收端响应超时")=="Receiver response timed out");
    assert(transport_error("用户自定义错误：设置")=="用户自定义错误：设置");
    const auto before=revision();fail_commit=true;assert(!set_locale("zh-CN"));assert(locale()==Locale::English&&revision()==before&&!follows_bridge());fail_commit=false;
    assert(apply_bridge_locale("zh-CN"));assert(locale()==Locale::English&&!follows_bridge());
    assert(stored_values["bridge_locale"]=="zh-CN"&&stored_values["locale"]=="en");
    const auto remembered_commits=commits;assert(apply_bridge_locale("zh-CN")&&commits==remembered_commits);
    fail_commit=true;assert(!set_follow_bridge());assert(locale()==Locale::English&&!follows_bridge()&&revision()==before);fail_commit=false;
    assert(set_follow_bridge()&&follows_bridge()&&locale()==Locale::Chinese); // Cached bridge preference applies immediately.
    assert(stored_values["follow_bridge"]=="1"&&stored_values["locale"]=="zh-CN");
    assert(apply_bridge_locale("en")&&locale()==Locale::English);
    assert(!apply_bridge_locale("fr")&&locale()==Locale::English&&follows_bridge());
    assert(request_path("/device/config")=="/device/config");
    assert(set_locale("zh-CN"));assert(message("Bridge HTTP 503")=="桥接响应 HTTP 503");
    assert(std::string(tr("settings.connection"))=="连接");
    assert(std::string(tr("settings.language"))=="语音");
    assert(std::string(tr("settings.volume"))=="音量");
    assert(message("Settings")=="设置");assert(message("unknown plugin output")=="unknown plugin output");
    assert(request_path("/device/config")=="/device/config?uiLocale=zh-CN");
    assert(request_path("/device/sessions?providerId=test")=="/device/sessions?providerId=test&uiLocale=zh-CN");
    assert(request_path("/pair/request")=="/pair/request");
    assert(set_locale("en")&&request_path("/device/tasks") == "/device/tasks?uiLocale=en");
    std::set<std::string> keys;
    for(const auto &entry:dictionary){assert(*entry.zh&&*entry.en);assert(keys.insert(entry.id ? entry.id : entry.zh).second);assert(known(entry.zh));}
    std::thread reader([]{for(int i=0;i<10000;++i){auto s=std::string(tr("设置"));assert(s=="设置"||s=="Settings");}});
    for(int i=0;i<100;++i)assert(set_locale(i%2?"zh-CN":"en"));reader.join();
    std::cout<<"PASS locale validation, same-language override, isolated atomic persistence, no-op writes, failure, follow/override, remembered PC language, request metadata, notices, dictionary "<<dictionary_size<<", concurrent reads\n";
}
