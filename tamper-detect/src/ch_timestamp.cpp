#include "detect.h"
#include "stats_util.h"
#include <algorithm>
#include <cmath>

namespace td {

// 通道1: 时间戳/容器层
// 正常录像帧间隔围绕帧率周期小幅抖动; 删帧会让间隔变成周期的整数倍,
// 插帧/重复帧会让间隔缩短, 粗暴拼接可能产生 PTS 回退或非整数倍跳变。
ChannelResult channel_timestamp(const VideoData& vd, const Config& cfg) {
    const auto& F = vd.frames;
    int n = (int)F.size();
    ChannelResult r;
    r.score.assign(n, 0.0f);

    std::vector<float> durs;
    durs.reserve(n);
    for (auto& f : F) durs.push_back((float)f.duration);
    float med_dur = median_of(durs);
    float sig = robust_sigma(durs, med_dur);

    for (int i = 0; i < n; ++i) {
        double d = F[i].duration;
        // PTS 回退
        if (i > 0 && F[i].pts < F[i - 1].pts) {
            r.events.push_back({F[i].idx, F[i].pts, d, med_dur,
                                F[i - 1].pts - F[i].pts, 3});
            r.score[i] = 1.0f;
            continue;
        }
        double dev = d - med_dur;
        if (std::fabs(dev) < cfg.ts_min_dev) continue;

        // 与"整周期倍数"的接近程度: 删帧后间隔常为 2x/3x 周期
        double ratio = d / std::max(med_dur, 1e-6f);
        double nearest_k = std::round(ratio);
        double integer_fit = (nearest_k >= 1.5)
            ? std::fabs(ratio - nearest_k) / std::max(ratio, 1e-6) : 1.0;

        float z = (float)std::fabs(dev) / sig;
        float s = clamp01(z / 6.0f);
        if (d > med_dur * 1.5) {
            int kind = integer_fit < 0.12 ? 1 : 1; // 间隔过长整体归为删帧类
            r.events.push_back({F[i].idx, F[i].pts, d, med_dur, dev, kind});
            // 整周期缺失是删帧的强证据, 提高置信度
            if (integer_fit < 0.12) s = std::max(s, 0.75f);
        } else if (d < med_dur * 0.5) {
            r.events.push_back({F[i].idx, F[i].pts, d, med_dur, dev, 2});
            s = std::max(s, 0.6f);
        }
        r.score[i] = s;
    }
    return r;
}

} // namespace td
