// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <string>
#include "provider_catalog.hpp"

// 形象素材的落盘缓存（worker 线程专用，禁止调用 LVGL）。
//
// SPIFFS 对象名上限 31 字节（SPIFFS_OBJ_NAME_LEN-1，且算的是 VFS 传给
// SPIFFS_open 的整串，含前导 '/'），所以平铺命名：
//   '/' + "s" + provider 的 6 位 FNV 哈希 + "_" + 文件名 + "~"（临时文件）
// 桥接器把文件名限在 ≤20 字符，于是 1+8+20+1 = 30 ≤ 31，留 1 字节余量。
// manifest.json 是「提交标记」：最后写入，存在且版本匹配即认为缓存完整。
namespace made_sprite_cache {

// 拉取函数：由 VibeCoding::request 注入（自动走 LAN/USB/接收器三条链路）。
using Fetch = std::function<bool(const std::string &path, std::string &out, int &status)>;

// 该助手素材的缓存目录前缀（"/spiffs/s<hash6>_"），供 LVGL 侧拼 PNG 路径。
std::string baseDir(const std::string &provider);

// 确保缓存里是这个版本。已缓存则校验哈希后直接返回；缺哪个补哪个，全部
// 就绪后重拉 manifest.json 提交。任何一步失败返回 false，保留现场下轮重试。
bool ensure(const std::string &provider, const vibe_provider::SpriteInfo &sprite,
            const Fetch &fetch);

// 读取已提交的 manifest.json（返回原始 JSON 文本）。缓存未提交时返回 false。
bool loadManifest(const std::string &provider, std::string &json);

// 读取已提交 manifest 的 version 字段（不触碰网络）。缓存未提交或 JSON
// 无版本号时返回 false。切助手时用它判断「本地缓存即可信」，直接换装。
bool cachedVersion(const std::string &provider, uint32_t &version);

// 整套删除该助手的缓存（含 manifest.json）。用于解码连败等「缓存不可信」
// 场景：删完后 cachedVersion/ loadManifest 都失败，ensure() 会完整重下。
// worker 线程专用（LVGL 侧只置请求标志）。
void purge(const std::string &provider);

} // namespace made_sprite_cache
