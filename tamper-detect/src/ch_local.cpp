#include "detect.h"
#include "stats_util.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace td {

// 通道5: 画面局部替换
// 两类痕迹, 均以"每个网格块自身的时间基线"为参照:
//  A) 噪声指纹异常: 块噪声持续偏离时间中位数(异质贴片/重编码覆盖);
//  B) 锐度异常下降: 块高频能量持续低于时间基线(模糊贴片、打码遮盖)。
// 四重抗误报约束:
//  1) 噪声地板/锐度绝对幅度;
//  2) 时间持续性(滑动窗口高频命中), 运动物体不会在固定位置长期异常;
//  3) 空间紧凑成簇且面积受限: 真实贴片是有界矩形, 全画面的自然纹理演化被排除;
//  4) 锐边: 簇内块必须与相邻正常块存在显著指标差(贴片边界), 弥漫性变化被排除。
ChannelResult channel_local(const VideoData& vd, const Config& cfg) {
    const auto& F = vd.frames;
    int n = (int)F.size();
    ChannelResult r;
    r.score.assign(n, 0.0f);
    r.local_mask.assign((size_t)n * GRID_N, 0);
    r.local_count.assign(n, 0);

    std::vector<float> fnz;
    fnz.reserve(n);
    for (int i = 0; i < n; ++i) fnz.push_back(F[i].noise_sigma);
    const float floor_dev = 0.30f * median_of(fnz);

    float nz_med[GRID_N], nz_sig[GRID_N], hf_med[GRID_N], hf_sig[GRID_N];
    for (int c = 0; c < GRID_N; ++c) {
        std::vector<float> a, b;
        a.reserve(n); b.reserve(n);
        for (int i = 0; i < n; ++i) { a.push_back(F[i].cell_sigma[c]); b.push_back(F[i].cell_hf[c]); }
        nz_med[c] = median_of(a); nz_sig[c] = robust_sigma(a, nz_med[c]);
        hf_med[c] = median_of(b); hf_sig[c] = robust_sigma(b, hf_med[c]);
    }

    // 逐帧逐块异常标记
    for (int i = 0; i < n; ++i) {
        int cnt = 0;
        for (int c = 0; c < GRID_N; ++c) {
            float nz_dev = F[i].cell_sigma[c] - nz_med[c];
            bool A = std::fabs(nz_dev) / nz_sig[c] > cfg.local_z_thresh &&
                     std::fabs(nz_dev) > floor_dev;
            // 高频能量异常(双向): 模糊贴片导致显著下降, 强噪/异质贴片导致显著升高。
            // 主判据用相对基线的偏离比例(对静止背景下极小的稳健 σ 不敏感),
            // z 分数作为辅助确认。
            float hf_dev = (F[i].cell_hf[c] - hf_med[c]);
            float hf_rel = std::fabs(hf_dev) / std::max(hf_med[c], 1e-3f);
            float hf_z = std::fabs(hf_dev) / hf_sig[c];
            bool B = (hf_rel > 0.6f) && (hf_z > 3.0f || hf_rel > 1.2f);
            if (A || B) { r.local_mask[(size_t)i * GRID_N + c] = 1; ++cnt; }
        }
        r.local_count[i] = cnt;
    }

    // 滑动窗口持续性 -> active
    const int W = cfg.local_min_frames;
    std::vector<uint8_t> active((size_t)n * GRID_N, 0);
    std::vector<int> hit(GRID_N, 0);
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < GRID_N; ++c)
            if (r.local_mask[(size_t)i * GRID_N + c]) ++hit[c];
        if (i >= W)
            for (int c = 0; c < GRID_N; ++c)
                if (r.local_mask[(size_t)(i - W) * GRID_N + c]) --hit[c];
        int window = std::min(i + 1, W);
        for (int c = 0; c < GRID_N; ++c)
            if ((float)hit[c] / window >= 0.6f) active[(size_t)i * GRID_N + c] = 1;
    }

    auto neighbors = [](int c) {
        std::vector<int> nb;
        int gx = c % GRID_W, gy = c / GRID_W;
        if (gx > 0)        nb.push_back(c - 1);
        if (gx < GRID_W-1) nb.push_back(c + 1);
        if (gy > 0)        nb.push_back(c - GRID_W);
        if (gy < GRID_H-1) nb.push_back(c + GRID_W);
        return nb;
    };

    // 贴片面积上限: 真实局部替换不超过约一半画面; 超出视为全画面自然变化
    const int MAX_CELLS = GRID_N / 2;   // 24 / 48
    const int ONSET_WIN = 15;           // 起变点检测窗口(约 0.6s)

    // 先逐帧提取"合格簇"的块集合与强度
    std::vector<uint8_t> ok_mask((size_t)n * GRID_N, 0);
    std::vector<float>   ok_strength(n, 0.0f);
    for (int i = 0; i < n; ++i) {
        const uint8_t* act = active.data() + (size_t)i * GRID_N;
        uint8_t seen[GRID_N] = {};
        for (int s = 0; s < GRID_N; ++s) {
            if (!act[s] || seen[s]) continue;
            int queue[GRID_N], qh = 0, qt = 0;
            queue[qt++] = s; seen[s] = 1;
            while (qh < qt) {
                int c = queue[qh++];
                for (int nb : neighbors(c))
                    if (act[nb] && !seen[nb]) { seen[nb] = 1; queue[qt++] = nb; }
            }
            int cluster = qt;
            if (cluster < 2 || cluster > MAX_CELLS) continue;

            int minx = GRID_W, miny = GRID_H, maxx = -1, maxy = -1;
            for (int k = 0; k < qt; ++k) {
                int c = queue[k], gx = c % GRID_W, gy = c / GRID_W;
                minx = std::min(minx, gx); maxx = std::max(maxx, gx);
                miny = std::min(miny, gy); maxy = std::max(maxy, gy);
            }
            int box = (maxx - minx + 1) * (maxy - miny + 1);
            float fill = (float)cluster / box;
            if (fill < 0.4f) continue;

            int sharp_edges = 0;
            for (int k = 0; k < qt; ++k) {
                int c = queue[k];
                for (int nb : neighbors(c)) {
                    if (act[nb]) continue;
                    float dn = std::fabs(F[i].cell_sigma[c] - F[i].cell_sigma[nb]);
                    float dh = std::fabs(F[i].cell_hf[c] - F[i].cell_hf[nb]);
                    if (dn > floor_dev || dh > 0.35f * std::max(hf_med[c], 1e-3f)) {
                        ++sharp_edges; break;
                    }
                }
            }
            if (sharp_edges < 2) continue;

            float zsum = 0;
            for (int k = 0; k < qt; ++k) {
                int c = queue[k];
                float zn = std::fabs(F[i].cell_sigma[c] - nz_med[c]) / nz_sig[c];
                float zh = std::fabs(F[i].cell_hf[c] - hf_med[c]) / hf_sig[c];
                zsum += std::max(zn, zh);
            }
            float strength = clamp01(0.55f + 0.03f * (cluster - 2) +
                                     0.02f * (zsum / cluster) + 0.1f * fill);
            for (int k = 0; k < qt; ++k)
                ok_mask[(size_t)i * GRID_N + queue[k]] = 1;
            ok_strength[i] = std::max(ok_strength[i], strength);
        }
    }

    // 时间起变点: 合格块必须在 ONSET_WIN 内从无到有突然形成。
    // 平滑纹理漂移是渐变的(窗口前后命中率都高), 被排除;
    // 贴片在贴入瞬间突变(此前几乎无命中、此后高命中)。
    std::vector<uint8_t> onset((size_t)n * GRID_N, 0);
    for (int i = ONSET_WIN; i < n; ++i) {
        for (int c = 0; c < GRID_N; ++c) {
            if (!ok_mask[(size_t)i * GRID_N + c]) continue;
            int before = 0, after = 0;
            for (int j = i - ONSET_WIN; j < i; ++j)
                if (ok_mask[(size_t)j * GRID_N + c]) ++before;
            for (int j = i; j < std::min(n, i + ONSET_WIN); ++j)
                if (ok_mask[(size_t)j * GRID_N + c]) ++after;
            float br = (float)before / ONSET_WIN;
            float ar = (float)after / std::min(ONSET_WIN, n - i);
            if (br < 0.3f && ar > 0.6f) onset[(size_t)i * GRID_N + c] = 1;
        }
    }

    // 起变帧一旦触发, 该块在"持续为合格簇成员"的整个区间内保持激活,
    // 直到它不再合格(贴片移除)。这样输出覆盖贴片的完整持续期, 而非仅起变瞬间。
    std::vector<uint8_t> latched(GRID_N, 0);
    std::vector<uint8_t> final_mask((size_t)n * GRID_N, 0);
    for (int i = 0; i < n; ++i) {
        int active_cells = 0;
        for (int c = 0; c < GRID_N; ++c) {
            if (onset[(size_t)i * GRID_N + c]) latched[c] = 1;
            // 不再合格则解除锁定(贴片结束)
            if (!ok_mask[(size_t)i * GRID_N + c]) latched[c] = 0;
            if (latched[c] && ok_mask[(size_t)i * GRID_N + c]) {
                final_mask[(size_t)i * GRID_N + c] = 1;
                ++active_cells;
            }
        }
        if (active_cells >= 2)
            r.score[i] = std::max(r.score[i], ok_strength[i]);
    }
    for (int i = 0; i < n; ++i) {
        int cnt = 0;
        for (int c = 0; c < GRID_N; ++c) {
            r.local_mask[(size_t)i * GRID_N + c] = final_mask[(size_t)i * GRID_N + c];
            cnt += final_mask[(size_t)i * GRID_N + c];
        }
        r.local_count[i] = cnt;
    }
    return r;
}

} // namespace td
