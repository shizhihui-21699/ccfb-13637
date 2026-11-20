#include "detect.h"
#include "stats_util.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace td {

// 通道2: 编码结构
// 一次正常编码的录像在固定 GOP 参数下产出规律性结构; 删帧/拼接/插帧会破坏它。
// 四个子信号:
//  a) GOP 间隔异常: 以全片 GOP 间隔"众数"为基线, 偏离的 I 帧即接缝嫌疑;
//  b) GOP 相位错乱: 正常 I 帧位置应落在基线周期的网格上, 接缝后整体错位;
//  c) 包大小突变(对数尺度): 接缝处重编码常伴随码率尖峰;
//  d) 运动向量断裂: MV 平均幅度逐帧跳变(画面内容不连续)。
ChannelResult channel_structure(const VideoData& vd, const Config&) {
    const auto& F = vd.frames;
    int n = (int)F.size();
    ChannelResult r;
    r.score.assign(n, 0.0f);

    std::vector<int> key_pos;
    for (int i = 0; i < n; ++i)
        if (F[i].key_flag || F[i].pict_type == 'I') key_pos.push_back(i);

    std::vector<float> gop_score(n, 0.0f);
    if (key_pos.size() >= 4) {
        // GOP 间隔众数
        std::map<int, int> hist;
        for (size_t k = 1; k < key_pos.size(); ++k)
            ++hist[key_pos[k] - key_pos[k - 1]];
        int mode_gap = 0, mode_cnt = 0;
        for (auto& p : hist) if (p.second > mode_cnt) { mode_cnt = p.second; mode_gap = p.first; }

        // a) 间隔异常, 分数落在异常 I 帧及其后 2 帧
        for (size_t k = 1; k < key_pos.size(); ++k) {
            int gap = key_pos[k] - key_pos[k - 1];
            if (mode_gap > 0 && std::abs(gap - mode_gap) > 2) {
                float rel = (float)std::abs(gap - mode_gap) / mode_gap;
                float s = clamp01(rel / 0.6f);
                for (int i = key_pos[k]; i <= std::min(n - 1, key_pos[k] + 2); ++i)
                    gop_score[i] = std::max(gop_score[i], s);
            }
        }
        // b) 相位错乱: I 帧位置相对周期网格的残差
        if (mode_gap > 0) {
            for (int kp : key_pos) {
                int grid = (int)std::round((double)kp / mode_gap) * mode_gap;
                int phase_err = std::abs(kp - grid);
                if (phase_err > 2) {
                    float s = clamp01((float)phase_err / (mode_gap * 0.5f));
                    for (int i = kp; i <= std::min(n - 1, kp + 2); ++i)
                        gop_score[i] = std::max(gop_score[i], 0.7f * s);
                }
            }
        }
    }

    // c) 包大小突变
    std::vector<float> lp(n);
    for (int i = 0; i < n; ++i) lp[i] = std::log((float)F[i].pkt_size + 1.0f);
    auto lp_base = rolling_median(lp, 25);
    std::vector<float> lp_dev(n);
    for (int i = 0; i < n; ++i) lp_dev[i] = std::fabs(lp[i] - lp_base[i]);
    float lp_sig = robust_sigma(lp_dev, median_of(lp_dev));
    std::vector<float> size_score(n);
    for (int i = 0; i < n; ++i) size_score[i] = clamp01((lp_dev[i] / lp_sig) / 6.0f);

    // QP 突变(若解码器导出)
    std::vector<float> qp_score(n, 0.0f);
    if (vd.meta.has_qp) {
        std::vector<float> qp(n);
        for (int i = 0; i < n; ++i) qp[i] = F[i].qp;
        auto qm = rolling_median(qp, 15);
        std::vector<float> dev(n);
        for (int i = 0; i < n; ++i) dev[i] = std::fabs(qp[i] - qm[i]);
        float qsig = robust_sigma(dev, median_of(dev));
        for (int i = 0; i < n; ++i) qp_score[i] = clamp01((dev[i] / qsig) / 6.0f);
    }

    // d) 运动向量断裂
    std::vector<float> mv_score(n, 0.0f);
    if (vd.meta.has_mv) {
        std::vector<float> d(n, 0.0f);
        for (int i = 1; i < n; ++i)
            d[i] = std::fabs(F[i].mv_mean_mag - F[i - 1].mv_mean_mag);
        auto base = rolling_median(d, 15);
        std::vector<float> dd(n);
        for (int i = 0; i < n; ++i) dd[i] = std::fabs(d[i] - base[i]);
        float msig = robust_sigma(dd, median_of(dd));
        for (int i = 0; i < n; ++i) mv_score[i] = clamp01((dd[i] / msig) / 6.0f);
    }

    for (int i = 0; i < n; ++i)
        r.score[i] = clamp01(0.42f * gop_score[i] + 0.26f * size_score[i] +
                             0.18f * qp_score[i] + 0.14f * mv_score[i]);
    return r;
}

} // namespace td
