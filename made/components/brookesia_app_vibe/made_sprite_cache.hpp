// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <string>
#include "provider_catalog.hpp"

// 形象素材的落盘缓存（worker 线程专用，禁止调用 LVGL）。
//
// SPIFFS 对象名上限 32 字符，且不支持目录，所以平铺命名：
//   "sp_" + provider 的 8 位 FNV 哈希 + "_" + 文件名（桥接器限 ≤20 字符）
// manifest.json 是「提交标记」：最后写入，存在且版本匹配即认为缓存完整。
namespace made_sprite_cache {

// 拉取函数：由 VibeCoding::request 注入（自动走 LAN/USB/接收器三条链路）。
using Fetch = std::function<bool(const std::string &path, std::string &out, int &status)>;

// 该助手素材的缓存目录前缀（"/spiffs/sp_<hash>_"），供 LVGL 侧拼 PNG 路径。
std::string baseDir(const std::string &provider);

// 确保缓存里是这个版本。已缓存则校验哈希后直接返回；缺哪个补哪个，全部
// 就绪后重拉 manifest.json 提交。任何一步失败返回 false，保留现场下轮重试。
bool ensure(const std::string &provider, const vibe_provider::SpriteInfo &sprite,
            const Fetch &fetch);

// 读取已提交的 manifest.json（返回原始 JSON 文本）。缓存未提交时返回 false。
bool loadManifest(const std::string &provider, std::string &json);

} // namespace made_sprite_cache
