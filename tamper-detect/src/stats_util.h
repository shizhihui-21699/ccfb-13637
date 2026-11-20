// 稳健统计小工具: 中位数、MAD、滚动中位数、z 分数等。
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace td {

inline float median_of(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// 中位数绝对偏差 -> 稳健标准差
inline float robust_sigma(const std::vector<float>& v, float med) {
    std::vector<float> d;
    d.reserve(v.size());
    for (float x : v) d.push_back(std::fabs(x - med));
    return 1.4826f * median_of(std::move(d)) + 1e-9f;
}

// 窗口滚动中位数(含端点截断处理)
inline std::vector<float> rolling_median(const std::vector<float>& x, int half_win) {
    int n = (int)x.size();
    std::vector<float> out(n);
    for (int i = 0; i < n; ++i) {
        int lo = std::max(0, i - half_win);
        int hi = std::min(n - 1, i + half_win);
        std::vector<float> w(x.begin() + lo, x.begin() + hi + 1);
        out[i] = median_of(std::move(w));
    }
    return out;
}

// 截断到 [0,1]
inline float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

} // namespace td
