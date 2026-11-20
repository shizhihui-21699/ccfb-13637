// 解码器封装 + 逐帧特征提取
// 输出按显示顺序(PTS)排列的 FrameInfo 序列, 只保留两份灰度图在内存。
#pragma once
#include "types.h"
#include <map>
#include <vector>

namespace td {

struct VideoMeta {
    std::string codec;
    int  width = 0, height = 0;
    int  analysis_w = 0, analysis_h = 0;
    double time_base = 0.0;
    double avg_fps = 0.0;
    double duration = 0.0;
    bool  has_mv = false;
    bool  has_qp = false;
};

struct VideoData {
    VideoMeta meta;
    std::vector<FrameInfo> frames;
};

// 打开文件并完成全文件解码分析。失败时抛出 std::runtime_error。
VideoData decode_and_extract(const Config& cfg);

} // namespace td
