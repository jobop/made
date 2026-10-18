// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

// 精简 GIF87a/89a 解码器（行动系统的整帧动画素材用，docs/SPRITES.md）。
//
// 设计目标是在 ESP32-S3 上流式播放一张 ≤256KB 的 GIF：画布只留一份
// (w*h*4 字节，PSRAM)，逐帧解码合成、上屏、再解下一帧——与把全部帧
// 预解码进内存的做法相反，用 CPU 换内存（每帧 LZW 约几毫秒，10fps 无压力）。
//
// 支持范围（创作约束见 made-kit 技能包 sprite 分册）：
//   - 全局/局部色表、帧间差分（只编码变化矩形）、透明索引、
//     disposal 0/1（保留）/2（清回透明）/3（还原上一帧）
//   - 交错存储（interlace）
// 不支持：纯文本扩展、LZW 码长异常等按损坏处理（decodeNext 返回 -1）。
namespace made_gif {

struct Decoder {
    Decoder() = default;
    ~Decoder() { close(); }
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    // 解析头并分配画布（数据被完整拷贝进解码器内部，调用方的缓冲可立即释放）。
    // 失败返回 false 且不占用任何内存。
    bool open(const uint8_t *data, size_t size);
    void close();

    // 解码下一帧并合成到画布（含上一帧的 disposal 处理）。
    // 返回 1 = 有新帧（frame_ms 为该帧建议展示时长，≥20ms）
    //       0 = 已到流尾
    //      -1 = 数据损坏 / 分配失败
    int decodeNext(uint32_t &frame_ms);

    // 回到第一帧之前（时长内循环播放用）。画布清回全透明。
    void rewind();

    // open 成功后有效：
    uint8_t *canvas = nullptr;   // BGRA 字节序（= LVGL ARGB8888 内存布局）
    int w = 0, h = 0;

private:
    uint8_t *data_ = nullptr;    // open 时拷贝的文件副本
    size_t size_ = 0;
    size_t pos_ = 0;
    size_t first_block_ = 0;     // 头部结束、块序列开始处（rewind 用）
    uint8_t palette_[768];       // 当前帧生效色表（全局或局部）
    int palette_n_ = 0;
    int transparent_ = -1;       // 当前帧透明索引（GCE 生命周期内有效）
    int disposal_ = 0;           // 当前帧播完后的处理方式
    uint8_t *backup_ = nullptr;  // disposal 3 的整画布快照（懒分配）
    // 上一帧遗留的 disposal 待办（下一帧解码前执行）
    int pd_mode_ = 0;
    int pd_x_ = 0, pd_y_ = 0, pd_w_ = 0, pd_h_ = 0;
    bool eof_ = false;
    // LZW 字典（跨帧保留省反复分配；uint16 语义：4096 项 × 3 张 ≈ 20KB）
    uint16_t lzw_prev_[4096];
    uint16_t lzw_first_[4096];
    uint8_t lzw_suffix_[4096];
    uint8_t lzw_stack_[4097];

    bool parseHeader();
    bool lzwDecode(int min_code_size, int rx, int ry, int rw, int rh, bool interlace);
    void applyPendingDisposal();
    void skipSubBlocks();
};

} // namespace made_gif
