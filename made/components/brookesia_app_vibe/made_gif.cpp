// SPDX-License-Identifier: Apache-2.0
#include "made_gif.hpp"

#include <cstring>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>

// 实现说明（配 made_gif.hpp 的接口注释读）：
//   - 每帧 = 「应用上一帧 disposal → 找到 GCE + 图像描述符 → LZW 解码 →
//     按透明索引合成到画布 → 记下本帧 disposal 待办」。显示方在两拍之间
//     拿画布上屏，disposal 永远发生在下一次 decodeNext 里，时序安全。
//   - LZW 用 prev/suffix/first 三张 4096 项表（first 只为 KwKwK 与新建
//     词条取串首像素），栈式输出，标准 GIF 变长码 LSB-first。
//   - 色表字节序 GIF 是 RGB，画布是 BGRA（LVGL ARGB8888），写像素时换序。

namespace made_gif {
namespace {

constexpr const char *kTag = "made_gif";
constexpr int kMaxCodes = 4096;

inline uint8_t rdU8(const uint8_t *d, size_t i) { return d[i]; }
inline int rdU16(const uint8_t *d, size_t i) { return d[i] | (d[i + 1] << 8); }

} // namespace

bool Decoder::open(const uint8_t *data, size_t size)
{
    close();
    if (!data || size < 14) return false;
    if (std::memcmp(data, "GIF87a", 6) != 0 && std::memcmp(data, "GIF89a", 6) != 0)
        return false;
    auto *copy = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) return false;
    std::memcpy(copy, data, size);
    data_ = copy;
    size_ = size;
    pos_ = 6;
    if (!parseHeader()) {
        close();
        return false;
    }
    first_block_ = pos_;
    return true;
}

bool Decoder::parseHeader()
{
    const uint8_t *d = data_;
    w = rdU16(d, pos_);
    h = rdU16(d, pos_ + 2);
    pos_ += 4;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return false;
    const uint8_t packed = rdU8(d, pos_++);
    pos_ += 2;   // 背景色索引 + 长宽比（画布初始即全透明，都不用）
    std::memset(palette_, 0, sizeof(palette_));
    palette_n_ = 0;
    if (packed & 0x80) {
        palette_n_ = 2 << (packed & 0x07);
        if (pos_ + static_cast<size_t>(palette_n_) * 3 > size_) return false;
        std::memcpy(palette_, d + pos_, static_cast<size_t>(palette_n_) * 3);
        pos_ += static_cast<size_t>(palette_n_) * 3;
    }
    canvas = static_cast<uint8_t *>(heap_caps_malloc(
        static_cast<size_t>(w) * h * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!canvas) return false;
    std::memset(canvas, 0, static_cast<size_t>(w) * h * 4);
    return true;
}

void Decoder::close()
{
    if (canvas) heap_caps_free(canvas);
    if (backup_) heap_caps_free(backup_);
    if (data_) heap_caps_free(data_);
    canvas = nullptr;
    backup_ = nullptr;
    data_ = nullptr;
    w = h = 0;
    size_ = pos_ = first_block_ = 0;
    pd_mode_ = 0;
    eof_ = false;
    transparent_ = -1;
    disposal_ = 0;
}

void Decoder::rewind()
{
    pos_ = first_block_;
    eof_ = false;
    transparent_ = -1;
    disposal_ = 0;
    pd_mode_ = 0;
    if (canvas) std::memset(canvas, 0, static_cast<size_t>(w) * h * 4);
}

void Decoder::skipSubBlocks()
{
    while (pos_ < size_) {
        const uint8_t len = rdU8(data_, pos_++);
        if (len == 0) return;
        pos_ += len;
    }
}

void Decoder::applyPendingDisposal()
{
    if (pd_mode_ == 2) {
        for (int y = 0; y < pd_h_; ++y) {
            uint8_t *row = canvas + (static_cast<size_t>(pd_y_ + y) * w + pd_x_) * 4;
            std::memset(row, 0, static_cast<size_t>(pd_w_) * 4);
        }
    } else if (pd_mode_ == 3 && backup_) {
        std::memcpy(canvas, backup_, static_cast<size_t>(w) * h * 4);
    }
    pd_mode_ = 0;
}

int Decoder::decodeNext(uint32_t &frame_ms)
{
    if (eof_ || !data_) return 0;
    applyPendingDisposal();
    const uint8_t *d = data_;
    while (pos_ < size_) {
        const uint8_t marker = rdU8(d, pos_++);
        if (marker == 0x3B) {           // trailer
            eof_ = true;
            return 0;
        }
        if (marker == 0x00) continue;   // 容错：个别编码器补零
        if (marker == 0x21) {           // 扩展块
            if (pos_ >= size_) return -1;
            const uint8_t label = rdU8(d, pos_++);
            if (label == 0xF9 && pos_ + 6 <= size_ && rdU8(d, pos_) == 4) {
                const uint8_t packed = rdU8(d, pos_ + 1);
                const int delay_cs = rdU16(d, pos_ + 2);
                disposal_ = (packed >> 2) & 0x07;
                transparent_ = (packed & 0x01) ? rdU8(d, pos_ + 4) : -1;
                frame_ms = delay_cs == 0 ? 100 : static_cast<uint32_t>(delay_cs) * 10;
                if (frame_ms < 20) frame_ms = 20;
                pos_ += 5;
                skipSubBlocks();
            } else {
                skipSubBlocks();        // 应用/注释/纯文本扩展：跳过
            }
            continue;
        }
        if (marker == 0x2C) {           // 图像描述符
            if (pos_ + 9 > size_) return -1;
            const int rx = rdU16(d, pos_);
            const int ry = rdU16(d, pos_ + 2);
            const int rw = rdU16(d, pos_ + 4);
            const int rh = rdU16(d, pos_ + 6);
            const uint8_t packed = rdU8(d, pos_ + 8);
            pos_ += 9;
            if (packed & 0x80) {
                const int n = 2 << (packed & 0x07);
                if (pos_ + static_cast<size_t>(n) * 3 > size_) return -1;
                std::memcpy(palette_, d + pos_, static_cast<size_t>(n) * 3);
                palette_n_ = n;
                pos_ += static_cast<size_t>(n) * 3;
            }
            if (palette_n_ <= 0 || rw <= 0 || rh <= 0 ||
                rx + rw > w || ry + rh > h) return -1;
            // disposal 3：合成前先存整画布快照（懒分配）
            if (disposal_ == 3 && !backup_) {
                backup_ = static_cast<uint8_t *>(heap_caps_malloc(
                    static_cast<size_t>(w) * h * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!backup_) return -1;
            }
            if (disposal_ == 3 && backup_)
                std::memcpy(backup_, canvas, static_cast<size_t>(w) * h * 4);
            if (pos_ >= size_) return -1;
            const int mcs = rdU8(d, pos_++);
            if (!lzwDecode(mcs, rx, ry, rw, rh, (packed & 0x40) != 0)) return -1;
            // 记下本帧 disposal 待办；透明索引只对当前帧有效
            pd_mode_ = disposal_;
            pd_x_ = rx; pd_y_ = ry; pd_w_ = rw; pd_h_ = rh;
            transparent_ = -1;
            disposal_ = 0;
            return 1;
        }
        return -1;                      // 未知块标记：按损坏处理
    }
    eof_ = true;
    return 0;
}

bool Decoder::lzwDecode(int min_code_size, int rx, int ry, int rw, int rh, bool interlace)
{
    if (min_code_size < 2 || min_code_size > 8 || pos_ >= size_) return false;
    const int clear = 1 << min_code_size;
    const int end = clear + 1;

    int code_size = min_code_size + 1;
    int next = end + 1;
    int prev_code = -1;
    uint32_t acc = 0;
    int acc_bits = 0;
    size_t block_left = 0;          // 当前子块剩余字节
    bool at_end = false;            // 已读到 0 长子块
    int sp = 0;

    // 行序表：交错存储按 4 遍扫描重排；顺序存储就是恒等映射。
    // （此前非交错也走了 4 遍填充，写穿 rows 三倍堆内存——崩溃与画面
    // 错位的共同根因。）
    std::vector<int> rows(static_cast<size_t>(rh));
    {
        int n = 0;
        if (interlace) {
            for (int pass = 0; pass < 4; ++pass) {
                const int start = pass == 0 ? 0 : 1 << (3 - pass);
                const int step = 8 >> pass;
                for (int y = start; y < rh; y += step) rows[static_cast<size_t>(n++)] = y;
            }
        } else {
            for (int y = 0; y < rh; ++y) rows[static_cast<size_t>(n++)] = y;
        }
    }

    int out_row = 0, out_col = 0;

    auto writePixel = [&](int idx) {
        if (out_row < rh) {
            const int py = ry + rows[static_cast<size_t>(out_row)];
            const int px = rx + out_col;
            uint8_t *dst = canvas + (static_cast<size_t>(py) * w + px) * 4;
            if (idx == transparent_) {
                // 保留画布原像素（帧间差分区域）
            } else {
                const uint8_t *c = palette_ + static_cast<size_t>(idx) * 3;
                dst[0] = c[2];   // B
                dst[1] = c[1];   // G
                dst[2] = c[0];   // R
                dst[3] = 255;
            }
            if (++out_col >= rw) { out_col = 0; ++out_row; }
        }
    };

    while (true) {
        // 取下一个码（LSB-first，跨子块）
        while (acc_bits < code_size) {
            if (block_left == 0) {
                if (at_end || pos_ >= size_) return false;
                block_left = rdU8(data_, pos_++);
                if (block_left == 0) { at_end = true; return false; }
            }
            acc |= static_cast<uint32_t>(rdU8(data_, pos_++)) << acc_bits;
            acc_bits += 8;
            --block_left;
        }
        const int code = static_cast<int>(acc & ((1u << code_size) - 1));
        acc >>= code_size;
        acc_bits -= code_size;

        if (code == clear) {
            code_size = min_code_size + 1;
            next = end + 1;
            prev_code = -1;
            continue;
        }
        if (code == end) {
            // 帧内提前收尾：跳过剩余子块
            while (block_left > 0) { --block_left; ++pos_; }
            skipSubBlocks();
            break;
        }

        uint8_t first_pix;
        if (code < next) {
            int cur = code;
            sp = 0;
            while (cur >= clear) {
                lzw_stack_[static_cast<size_t>(sp++)] = lzw_suffix_[static_cast<size_t>(cur)];
                cur = lzw_prev_[static_cast<size_t>(cur)];
            }
            lzw_stack_[static_cast<size_t>(sp++)] = static_cast<uint8_t>(cur);
            first_pix = code < clear ? static_cast<uint8_t>(code)
                                     : static_cast<uint8_t>(lzw_first_[static_cast<size_t>(code)]);
        } else if (code == next && prev_code >= 0) {
            // KwKwK：串 = string(prev) + first(prev)，共 len(prev)+1 个像素。
            // 栈是倒序压入、正序弹出：首像素必须压在最底下（最后弹出），
            // 否则每遇一次 KwKwK 就少画一个像素——累积后画面横向剪切错位。
            first_pix = prev_code < clear ? static_cast<uint8_t>(prev_code)
                                          : static_cast<uint8_t>(lzw_first_[static_cast<size_t>(prev_code)]);
            lzw_stack_[0] = first_pix;
            int cur = prev_code;
            sp = 1;
            while (cur >= clear) {
                lzw_stack_[static_cast<size_t>(sp++)] = lzw_suffix_[static_cast<size_t>(cur)];
                cur = lzw_prev_[static_cast<size_t>(cur)];
            }
            lzw_stack_[static_cast<size_t>(sp++)] = static_cast<uint8_t>(cur);
        } else {
            return false;
        }
        while (sp > 0) writePixel(lzw_stack_[static_cast<size_t>(--sp)]);

        if (prev_code >= 0 && next < kMaxCodes) {
            lzw_prev_[static_cast<size_t>(next)] = static_cast<uint16_t>(prev_code);
            lzw_suffix_[static_cast<size_t>(next)] = first_pix;
            lzw_first_[static_cast<size_t>(next)] = static_cast<uint16_t>(
                prev_code < clear ? prev_code : lzw_first_[static_cast<size_t>(prev_code)]);
            ++next;
            if (next == (1 << code_size) && code_size < 12) ++code_size;
        }
        prev_code = code;
        if (out_row >= rh) {
            // 像素已齐：跳过本帧剩余子块
            while (block_left > 0) { --block_left; ++pos_; }
            skipSubBlocks();
            break;
        }
    }
    return true;
}

} // namespace made_gif
