// tamper-detect: 无参考视频篡改检测 —— 公共数据结构
// 所有时间单位: 秒(浮点); 帧索引为显示顺序(解码后按 PTS 重排)。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace td {

// 画面分析网格: 把画面划分为 GRID_W x GRID_H 个块, 统计局部噪声,
// 用于检测"画面局部替换"(替换区域的噪声水平与周围不一致)。
constexpr int GRID_W = 8;
constexpr int GRID_H = 6;
constexpr int GRID_N = GRID_W * GRID_H;

// 单帧特征记录(流式解码时逐帧生成, 全部保留在内存: 每帧约 0.5KB,
// 3 小时 25fps ≈ 27 万帧 ≈ 140MB, 可接受)。
struct FrameInfo {
    int64_t idx = 0;              // 显示顺序帧号
    double  pts = 0.0;           // 时间戳(秒)
    double  duration = 0.0;      // 与下一帧的时间间隔(秒), 末帧沿用前一帧
    char    pict_type = '?';     // I/P/B
    int64_t pkt_size = 0;        // 压缩包大小(字节)
    float   qp = -1.0f;          // 帧级量化参数(无 side data 时为 -1)
    bool    key_flag = false;    // packet keyframe 标志

    // 画面内容(灰度, 统一缩放到分析分辨率后计算)
    float   mean_luma = 0.0f;    // 平均亮度
    float   ssd = 0.0f;          // 与前一帧的灰度差平方和(归一化到 0..1 像素)
    float   sad = 0.0f;          // 与前一帧的灰度绝对差均值
    bool    duplicate = false;   // 与前一帧近乎完全相同
    float   noise_sigma = 0.0f;  // 全帧噪声水平估计
    float   cell_sigma[GRID_N] = {}; // 各网格块噪声水平
    float   cell_hf[GRID_N] = {};    // 各网格块高频能量(锐度), 模糊贴片会显著偏低

    // 运动向量统计(无 MV side data 时 count=0)
    int     mv_count = 0;
    float   mv_mean_mag = 0.0f;  // 平均运动幅度(像素)
    float   mv_frac = 0.0f;      // 亚像素 MV 占比(0..1)
};

// 时间戳/容器层异常
struct TsEvent {
    int64_t idx;
    double  time;
    double  duration;            // 实际帧间隔
    double  expected;            // 由稳健估计得到的期望间隔
    double  deviation;           // 偏差秒数(正=跳变/缺帧, 负=插入/重复)
    int     kind;                // 1=间隔过长(疑似删帧) 2=间隔过短(疑似插帧) 3=PTS回退/乱序
};

// 检测结论
enum class VerdictType {
    FRAME_DELETION,    // 删帧
    FRAME_INSERTION,   // 插帧(含重复帧填充)
    SPLICE,            // 拼接(异源/重编码边界)
    LOCAL_REPLACE,     // 画面局部替换
};

struct Segment {
    double start, end;            // 时间段(秒)
    int64_t start_idx, end_idx;
};

struct Finding {
    VerdictType type;
    double  start, end;           // 可疑时间段
    int64_t start_idx, end_idx;
    double  score;                // 融合得分 0..1
    std::string evidence;         // 人类可读依据
    // 触发的各通道分数(调试/报告用)
    float s_ts=0, s_struct=0, s_noise=0, s_scene=0, s_local=0;
};

struct Config {
    // 分析用灰度分辨率(保持宽高比, 宽度固定, 高度按 16 对齐)
    int analysis_width = 320;
    // 连续近重复帧判定阈值(归一化 SAD)
    float dup_sad_thresh = 0.4f;
    // 场景突变判定: SAD 相对局部中位数的倍数
    float scene_cut_ratio = 4.0f;
    // 局部替换: 块噪声与全帧中位数的 z 分数阈值
    float local_z_thresh = 4.0f;
    // 局部替换需持续的最少帧数
    int   local_min_frames = 12;
    // 融合报警分数阈值
    float report_score_thresh = 0.45f;
    // 时间戳异常的最小偏差(秒), 小于该值忽略(容忍容器抖动)
    float ts_min_dev = 0.005f;
    std::string input;
    std::string json_out;
    std::string csv_out;
    bool      verbose = false;
};

const char* verdict_name(VerdictType t);

} // namespace td
