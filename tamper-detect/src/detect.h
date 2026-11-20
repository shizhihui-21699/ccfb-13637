// 五个检测通道的统一接口。
// 每个通道输出: 逐帧异常分(0..1) + 可选的事件列表/文字依据。
#pragma once
#include "types.h"
#include "decoder.h"
#include <memory>

namespace td {

struct ChannelResult {
    std::vector<float> score;      // 逐帧分数
    std::vector<TsEvent> events;   // 目前仅时间戳通道使用
    // 局部替换通道: 每帧异常网格块的数量与位置标记
    std::vector<uint8_t> local_mask;   // n*GRID_N, 1=该块异常
    std::vector<int> local_count;      // 每帧异常块数
};

// 1. 时间戳/容器: 帧间隔异常、PTS 回退
ChannelResult channel_timestamp(const VideoData& vd, const Config& cfg);

// 2. 编码结构: GOP/关键帧规律、码率突变、QP 突变、运动向量断裂
ChannelResult channel_structure(const VideoData& vd, const Config& cfg);

// 3. 噪声指纹: 全帧噪声水平的时序突变(异源拼接/重编码边界)
ChannelResult channel_noise(const VideoData& vd, const Config& cfg);

// 4. 场景连续性: 场景突变、重复帧簇、运动-残差不一致
ChannelResult channel_scene(const VideoData& vd, const Config& cfg);

// 5. 局部区域: 网格块噪声与周围画面不一致且持续存在(局部替换)
ChannelResult channel_local(const VideoData& vd, const Config& cfg);

// 融合: 多通道加权 + 时间段聚合, 输出最终结论
std::vector<Finding> fuse(const VideoData& vd, const Config& cfg,
                          const ChannelResult& ts, const ChannelResult& st,
                          const ChannelResult& nz, const ChannelResult& sc,
                          const ChannelResult& lc);

} // namespace td
