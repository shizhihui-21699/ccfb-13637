#include "detect.h"
#include "stats_util.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <map>

namespace td {

static float peak_val(const std::vector<float>& s, int lo, int hi) {
    float m = 0;
    for (int i = lo; i <= hi; ++i) m = std::max(m, s[i]);
    return m;
}

// 把连续超阈值帧聚合成段, 允许小间隙合并
static std::vector<std::pair<int, int>>
segments_above(const std::vector<float>& s, float thresh, int max_gap) {
    std::vector<std::pair<int, int>> out;
    int n = (int)s.size();
    int start = -1, last = -1;
    for (int i = 0; i < n; ++i) {
        if (s[i] >= thresh) {
            if (start < 0) start = i;
            last = i;
        } else if (start >= 0 && i - last > max_gap) {
            out.push_back({start, last});
            start = last = -1;
        }
    }
    if (start >= 0) out.push_back({start, last});
    return out;
}

std::vector<Finding> fuse(const VideoData& vd, const Config& cfg,
                          const ChannelResult& ts, const ChannelResult& st,
                          const ChannelResult& nz, const ChannelResult& sc,
                          const ChannelResult& lc) {
    const auto& F = vd.frames;
    int n = (int)F.size();

    // 通道权重: 时间戳与编码结构最难以画面内容解释, 权重最高;
    // 噪声与场景次之; 局部通道仅用于局部替换判定。
    const float W_TS = 0.28f, W_ST = 0.27f, W_NZ = 0.20f, W_SC = 0.25f;
    std::vector<float> global(n);
    for (int i = 0; i < n; ++i)
        global[i] = clamp01(W_TS * ts.score[i] + W_ST * st.score[i] +
                            W_NZ * nz.score[i] + W_SC * sc.score[i]);

    std::vector<Finding> findings;

    // ---------- A0. 重复帧簇: 冻结/复制填充插帧的强独立证据 ----------
    {
        int run_start = -1, run_len = 0;
        auto emit_dup = [&](int lo, int hi) {
            if (hi - lo + 1 < 2) return;   // 单帧可能是编码器跳帧, 不报
            Finding f;
            f.type = VerdictType::FRAME_INSERTION;
            f.start = F[lo].pts;
            f.end = F[std::min(hi + 1, n - 1)].pts;
            f.start_idx = F[lo].idx; f.end_idx = F[hi].idx;
            f.s_scene = 0.9f;
            f.score = clamp01(0.65f + 0.03f * (hi - lo));
            std::ostringstream ev;
            ev << "连续 " << (hi - lo + 1) << " 帧与前一帧近乎完全相同(SAD<"
               << "阈值), 时间戳却仍在推进 -> 冻结/复制填充式插帧";
            f.evidence = ev.str();
            findings.push_back(f);
        };
        for (int i = 0; i < n; ++i) {
            if (F[i].duplicate) {
                if (run_start < 0) run_start = i;
                run_len = i - run_start + 1;
            } else {
                if (run_start >= 0) emit_dup(run_start, i - 1);
                run_start = -1; run_len = 0;
            }
        }
        if (run_start >= 0) emit_dup(run_start, n - 1);
        (void)run_len;
    }

    // ---------- A1. 接缝检测: 针对重编码后 PTS 被抹平的删帧/拼接 ----------
    // 删帧后整体重编码会重写时间戳, 时间戳通道失效; 但接缝处仍留下可联合的痕迹:
    // 非预期 I 帧(结构)、噪声阶跃、码率尖峰、帧间内容不连续。单信号都可能自然
    // 出现(场景切换), 多信号在同一帧汇聚才报警。
    {
        // I 帧集合(关键帧或 I 类型)
        std::vector<int> ikeys;
        for (int i = 0; i < n; ++i)
            if (F[i].key_flag || F[i].pict_type == 'I') ikeys.push_back(i);
        if (ikeys.size() >= 6) {
            // IDR 周期估计: scenecut 产生大量小间隔, 取出现 >=2 次的最大间隔。
            std::map<int, int> hist;
            for (size_t k = 1; k < ikeys.size(); ++k)
                ++hist[ikeys[k] - ikeys[k - 1]];
            int mode_gap = 0;
            for (auto& p : hist)
                if (p.first >= 8 && p.second >= 2) mode_gap = p.first;
            if (mode_gap == 0) mode_gap = hist.rbegin()->first;

            // 周期 IDR 帧: 与上一 I 帧间隔接近完整周期; scenecut I 帧间隔小, 排除。
            auto is_idr = [&](size_t k) {
                if (k == 0) return true;
                return std::abs((ikeys[k] - ikeys[k - 1]) - mode_gap) <= 2;
            };

            // 自适应局部相位: 以接缝前最近的周期 IDR 为原点。
            // 自然 scenecut 后下一个 IDR 仍落在 原点+整数倍周期(可恢复);
            // 删帧/拼接平移帧号后, 后续 IDR 相对原点持续偏离(不可恢复)。
            for (size_t k = 1; k + 2 < ikeys.size(); ++k) {
                int i = ikeys[k];
                if (i < 3 || i >= n - mode_gap) continue;

                int origin = -1;
                for (int j = (int)k - 1; j >= 0; --j)
                    if (is_idr((size_t)j)) { origin = ikeys[j]; break; }
                if (origin < 0) continue;

                auto local_phase = [&](int pos) {
                    int grid = origin + (int)std::round((double)(pos - origin) / mode_gap) * mode_gap;
                    return (float)std::abs(pos - grid) / mode_gap;
                };
                float here   = local_phase(i);
                float after1 = local_phase(ikeys[k + 1]);
                float after2 = local_phase(ikeys[k + 2]);
                bool phase_shift = (here > 0.3f && after1 > 0.3f && after2 > 0.3f);
                if (!phase_shift) continue;

                float sig_nz = peak_val(nz.score, i, std::min(n - 1, i + 2));
                float sig_br = peak_val(st.score, std::max(0, i - 1), std::min(n - 1, i + 1));
                float sig_cut = sc.score[i];
                float sig_gop = clamp01(here / 0.5f);
                float sig_ts  = peak_val(ts.score, std::max(0, i - 2), std::min(n - 1, i + 2));

                bool is_splice   = (sig_nz > 0.45f && (sig_br > 0.3f || sig_gop > 0.5f));
                bool is_deletion = (!is_splice &&
                                    (sig_br > 0.4f || sig_ts > 0.5f) && sig_cut < 0.55f);
                if (!is_splice && !is_deletion) continue;

                int lo2 = std::max(0, i - 2), hi2 = std::min(n - 1, i + 2);
                bool dup = false;
                for (auto& old : findings)
                    if (old.start_idx <= hi2 && old.end_idx >= lo2) { dup = true; break; }
                if (dup) continue;

                Finding f;
                f.type = is_splice ? VerdictType::SPLICE : VerdictType::FRAME_DELETION;
                f.start = F[lo2].pts; f.end = F[hi2].pts;
                f.start_idx = F[lo2].idx; f.end_idx = F[hi2].idx;
                f.s_ts = sig_ts; f.s_struct = sig_gop; f.s_noise = sig_nz; f.s_scene = sig_cut;
                f.score = is_splice
                    ? clamp01(0.5f * sig_nz + 0.3f * sig_gop + 0.2f * sig_br + 0.15f)
                    : clamp01(0.5f * sig_gop + 0.3f * sig_br + 0.2f * sig_ts + 0.15f);
                std::ostringstream ev;
                ev << "I 帧 GOP 相位在帧 " << i << " 处相对局部 IDR 网格(原点帧 " << origin
                   << ", 周期 " << mode_gap << ")持续不可恢复, 后续相位残差="
                   << after1 << "/" << after2 << "; 时间戳异常=" << sig_ts
                   << " 噪声阶跃=" << sig_nz << " 码率尖峰=" << sig_br
                   << " 自然硬切=" << sig_cut;
                if (is_splice)
                    ev << " -> 噪声指纹突变, 判定异源拼接";
                else
                    ev << " -> 噪声连续但编码相位不可恢复且非自然硬切, 判定重编码删帧";
                f.evidence = ev.str();
                findings.push_back(f);
            }
        }
    }

    // ---------- A2. 轻量接缝规则: 直接利用强噪声阶跃与帧间不连续 ----------
    {
        std::vector<float> allsad;
        for (auto& f : F) allsad.push_back(f.sad);
        float sad_med = median_of(allsad);
        for (int i = 2; i < n - 2; ++i) {
            float nzv = peak_val(nz.score, i, std::min(n - 1, i + 1));
            float stv = peak_val(st.score, std::max(0, i - 1), std::min(n - 1, i + 1));
            float sad_ratio = F[i].sad / std::max(sad_med, 1e-6f);
            float scv = sc.score[i];

            // 邻近已报拼接点 ±15 帧内, 不再报删帧(异源片段内部的内容起伏)
            bool near_splice = false;
            for (auto& old : findings)
                if (old.type == VerdictType::SPLICE &&
                    i >= old.start_idx - 15 && i <= old.end_idx + 15) { near_splice = true; break; }

            bool fire = false;
            VerdictType vt;
            std::ostringstream ev;
            // 异源接缝: 强噪声指纹阶跃 + (结构异常 或 帧间不连续)
            if (nzv > 0.6f && (stv > 0.35f || sad_ratio > 2.0f)) {
                fire = true; vt = VerdictType::SPLICE;
                ev << "噪声指纹阶跃(=" << nzv << ")伴随编码/内容不连续, 判定异源接缝";
            }
            // 同源删帧: 帧间显著不连续(>=4 倍) + 编码异常或场景突变, 无噪声阶跃。
            // sc 偏高(>=0.45)说明是整体画面硬切, 属正常剪辑, 排除; 删帧是相邻时刻
            // 内容, 硬切强度中等。
            else if (!near_splice && nzv < 0.3f && sad_ratio > 4.0f &&
                     (stv > 0.15f || scv > 0.3f) && scv < 0.45f) {
                fire = true; vt = VerdictType::FRAME_DELETION;
                ev << "帧间差异突增 " << sad_ratio << " 倍且编码异常、无自然场景硬切, 判定重编码删帧";
            }
            if (!fire) continue;

            int lo2 = std::max(0, i - 2), hi2 = std::min(n - 1, i + 2);
            bool dup = false;
            for (auto& old : findings)
                if (old.start_idx <= hi2 && old.end_idx >= lo2) { dup = true; break; }
            if (dup) continue;
            Finding f;
            f.type = vt;
            f.start = F[lo2].pts; f.end = F[hi2].pts;
            f.start_idx = F[lo2].idx; f.end_idx = F[hi2].idx;
            f.s_struct = stv; f.s_noise = nzv; f.s_scene = scv;
            f.score = vt == VerdictType::SPLICE
                ? clamp01(0.6f * nzv + 0.4f * stv + 0.1f)
                : clamp01(0.5f * stv + 0.3f * scv + 0.2f * clamp01(sad_ratio / 6.0f) + 0.15f);
            f.evidence = ev.str();
            findings.push_back(f);
        }
    }

    // ---------- A. 局部替换: 独立通道, 独立成段 ----------
    std::vector<Finding> local_raw;
    for (auto seg : segments_above(lc.score, 0.5f, cfg.local_min_frames / 2)) {
        int lo = seg.first, hi = seg.second;
        int seg_len = hi - lo + 1;
        if (seg_len < 125) continue;  // 至少 5s: 运动掠过/内容起伏仅数秒, 真实贴片/遮盖持续更久
        // 找出"位置固定"的持续异常网格块(段内命中率 >70%)。
        // 运动物体让异常块沿轨迹移动, 单块命中率低; 贴片固定在同一组块上。
        bool cell_hit[GRID_N] = {};
        int persistent = 0;
        for (int c = 0; c < GRID_N; ++c) {
            int hits = 0;
            for (int i = lo; i <= hi; ++i)
                hits += lc.local_mask[(size_t)i * GRID_N + c];
            if ((float)hits / seg_len > 0.70f) { cell_hit[c] = true; ++persistent; }
        }
        if (persistent < 2) continue;   // 无固定位置的成簇异常 -> 运动物体, 排除
        int minx = GRID_W, miny = GRID_H, maxx = -1, maxy = -1;
        for (int c = 0; c < GRID_N; ++c) if (cell_hit[c]) {
            int gx = c % GRID_W, gy = c / GRID_W;
            minx = std::min(minx, gx); maxx = std::max(maxx, gx);
            miny = std::min(miny, gy); maxy = std::max(maxy, gy);
        }
        std::ostringstream ev;
        ev << "局部区域噪声/锐度持续偏离该区域时间基线(" << (hi - lo + 1)
           << "帧); 异常块噪声 z>" << cfg.local_z_thresh << " 或高频能量异常(模糊贴片降低、异质/重噪贴片升高)";
        if (maxx >= 0) {
            float x0 = (float)minx / GRID_W, x1 = (float)(maxx + 1) / GRID_W;
            float y0 = (float)miny / GRID_H, y1 = (float)(maxy + 1) / GRID_H;
            ev << "; 可疑区域约占画面 x[" << std::fixed << std::setprecision(2) << x0
               << "~" << x1 << "] y[" << y0 << "~" << y1 << "]";
        }
        Finding f;
        f.type = VerdictType::LOCAL_REPLACE;
        f.start = F[lo].pts; f.end = F[std::min(hi + 1, n - 1)].pts;
        f.start_idx = F[lo].idx; f.end_idx = F[hi].idx;
        f.s_local = peak_val(lc.score, lo, hi);
        f.score = clamp01(0.6f * f.s_local + 0.4f * peak_val(global, lo, hi));
        f.evidence = ev.str();
        local_raw.push_back(f);
    }
    // 合并被内容运动短暂打断的相邻局部段(间隔 < 1.2s), 还原真实替换区间
    for (auto& f : local_raw) {
        bool merged = false;
        for (auto& k : findings) {
            if (k.type != VerdictType::LOCAL_REPLACE) continue;
            int gap = f.start_idx - k.end_idx;
            if (gap >= 0 && gap < 30) {
                k.end = std::max(k.end, f.end);
                k.end_idx = std::max(k.end_idx, f.end_idx);
                k.score = std::max(k.score, f.score);
                k.s_local = std::max(k.s_local, f.s_local);
                merged = true;
                break;
            }
        }
        if (!merged) findings.push_back(f);
    }

    // ---------- B. 全局异常段: 删帧 / 插帧 / 拼接 ----------
    auto segs = segments_above(global, cfg.report_score_thresh, 6);
    for (auto seg : segs) {
        int lo = seg.first, hi = seg.second;

        float s_ts = peak_val(ts.score, lo, hi);
        float s_st = peak_val(st.score, lo, hi);
        float s_nz = peak_val(nz.score, lo, hi);
        float s_sc = peak_val(sc.score, lo, hi);

        // 统计段内时间戳事件
        int long_gap = 0, short_gap = 0, backward = 0;
        double max_dev = 0;
        for (auto& e : ts.events) {
            if (e.idx < F[lo].idx || e.idx > F[hi].idx) continue;
            if (e.kind == 1) { ++long_gap; max_dev = std::max(max_dev, e.deviation); }
            if (e.kind == 2) ++short_gap;
            if (e.kind == 3) ++backward;
        }
        int dup_frames = 0;
        for (int i = lo; i <= hi; ++i) if (F[i].duplicate) ++dup_frames;
        bool has_cut = peak_val(sc.score, lo, hi) > 0.5f;

        // ---- 分类规则 ----
        VerdictType vtype;
        std::ostringstream ev;
        if (short_gap > 0 || dup_frames >= 2) {
            vtype = VerdictType::FRAME_INSERTION;
            if (short_gap) ev << "帧间隔异常缩短 " << short_gap << "处";
            if (dup_frames >= 2) {
                if (short_gap) ev << "; ";
                ev << "连续近重复帧 " << dup_frames << "帧(疑似冻结/复制填充插帧)";
            }
        } else if (long_gap > 0 && (s_nz > 0.45f || s_st > 0.45f)) {
            vtype = VerdictType::SPLICE;
            ev << "时间戳跳变 " << long_gap << "处(最大偏差 " << std::fixed
               << std::setprecision(3) << max_dev << "s), 且编码/噪声指纹同步突变 -> 异源拼接";
        } else if (long_gap > 0 || backward > 0) {
            vtype = VerdictType::FRAME_DELETION;
            ev << "帧间隔异常拉长 " << long_gap << "处(最大偏差 " << std::fixed
               << std::setprecision(3) << max_dev << "s), 接近帧周期整数倍 -> 删帧";
            if (backward) ev << "; 出现 PTS 回退 " << backward << "处";
        } else if (s_nz > 0.5f && s_st > 0.4f) {
            vtype = VerdictType::SPLICE;
            ev << "噪声水平阶跃与编码参数突变同时出现, 无对应时间戳缺口 -> 重编码拼接";
        } else if (has_cut && (s_st > 0.4f || s_nz > 0.4f)) {
            vtype = VerdictType::SPLICE;
            ev << "场景硬切处伴随编码结构/噪声指纹突变 -> 可疑拼接(自然切换不会改变指纹)";
        } else {
            vtype = VerdictType::FRAME_DELETION;
            ev << "多通道异常汇聚但时间戳缺口不明显, 疑似经重编码抹平的删帧";
        }
        if (has_cut) ev << "; 伴场景突变";
        ev << " [通道分 ts=" << std::setprecision(2) << s_ts
          << " struct=" << s_st << " noise=" << s_nz << " scene=" << s_sc << "]";

        Finding f;
        f.type = vtype;
        f.start = F[lo].pts;
        f.end = F[std::min(hi + 1, n - 1)].pts;
        f.start_idx = F[lo].idx; f.end_idx = F[hi].idx;
        f.s_ts = s_ts; f.s_struct = s_st; f.s_noise = s_nz; f.s_scene = s_sc;
        f.score = clamp01(peak_val(global, lo, hi) * 0.8f +
                          0.2f * std::max({s_ts, s_st, s_nz, s_sc}));
        f.evidence = ev.str();
        findings.push_back(f);
    }

    // 按时间排序, 合并高度重叠或时间紧邻(间隔 <0.8s)的同类段(保留高分并扩展区间)
    std::sort(findings.begin(), findings.end(),
              [](const Finding& a, const Finding& b) { return a.start < b.start; });
    std::vector<Finding> kept;
    for (auto& f : findings) {
        bool merged = false;
        for (auto& k : kept) {
            if (f.type != k.type) continue;
            double ov = std::min(f.end, k.end) - std::max(f.start, k.start);
            double uni = std::max(f.end, k.end) - std::min(f.start, k.start);
            double gap = std::max(0.0, std::max(f.start, k.start) -
                                        std::min(f.end, k.end));
            if ((uni > 0 && ov / uni > 0.6) || gap < 0.8) {
                k.start = std::min(k.start, f.start);
                k.end = std::max(k.end, f.end);
                k.start_idx = std::min(k.start_idx, f.start_idx);
                k.end_idx = std::max(k.end_idx, f.end_idx);
                k.score = std::max(k.score, f.score);
                merged = true;
                break;
            }
        }
        if (!merged) kept.push_back(f);
    }
    return kept;
}

} // namespace td
