#include "detect.h"
#include "stats_util.h"
#include <algorithm>
#include <cmath>

namespace td {

// 通道4: 场景连续性
// 真实摄像画面的帧间差异是连续变化的。三类痕迹:
//  a) 场景突变(硬切): SAD 远超局部常态 —— 拼接点或删帧造成的内容跳变;
//  b) 重复帧簇: 连续多帧近乎完全相同 —— 插帧常用的冻结/复制填充;
//  c) 亮度阶跃: 平均亮度在一帧内跳变而 SAD 却不高 —— 调包/叠层痕迹。
ChannelResult channel_scene(const VideoData& vd, const Config& cfg) {
    const auto& F = vd.frames;
    int n = (int)F.size();
    ChannelResult r;
    r.score.assign(n, 0.0f);

    std::vector<float> sad(n), luma(n);
    for (int i = 0; i < n; ++i) { sad[i] = F[i].sad; luma[i] = F[i].mean_luma; }
    auto sad_base = rolling_median(sad, 25);

    std::vector<float> cut(n, 0.0f);
    // 全局 SAD 分布作为绝对门槛, 避免局部中位数过低导致阈值失效
    std::vector<float> sad_sorted = sad;
    float sad_glob_med = median_of(sad_sorted);
    for (int i = 2; i < n; ++i) {
        float base = std::max(sad_base[i], 1e-4f);
        float ratio = sad[i] / base;
        // 相对倍数 或 超过全局中位数 2.2 倍, 任一满足即视为突变候选
        bool hit = ratio > cfg.scene_cut_ratio ||
                   sad[i] > sad_glob_med * 2.2f;
        if (hit)
            cut[i] = clamp01((ratio - cfg.scene_cut_ratio) / cfg.scene_cut_ratio + 0.4f);
    }

    // 重复帧簇: 统计当前帧向前连续 duplicate 的长度
    std::vector<float> dup(n, 0.0f);
    int run = 0;
    for (int i = 0; i < n; ++i) {
        if (F[i].duplicate) {
            ++run;
            // 单个重复帧可能是编码器跳帧, 连续 >=2 帧才可疑, 越长置信度越高
            if (run >= 2) dup[i] = clamp01(0.5f + 0.15f * (run - 2));
        } else {
            run = 0;
        }
    }

    // 亮度阶跃
    std::vector<float> lstep(n, 0.0f);
    {
        std::vector<float> d(n, 0.0f);
        for (int i = 1; i < n; ++i) d[i] = std::fabs(luma[i] - luma[i - 1]);
        float med = median_of(d);
        float sig = robust_sigma(d, med);
        for (int i = 1; i < n; ++i) {
            float z = std::fabs(d[i] - med) / sig;
            // 同时要求 SAD 没有同步爆表(否则只是普通场景切换, 已由 cut 覆盖)
            if (cut[i] < 0.3f) lstep[i] = clamp01(z / 6.0f);
        }
    }

    for (int i = 0; i < n; ++i)
        r.score[i] = clamp01(0.55f * cut[i] + 0.35f * dup[i] + 0.10f * lstep[i]);
    return r;
}

} // namespace td
