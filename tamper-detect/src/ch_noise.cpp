#include "detect.h"
#include "stats_util.h"
#include <algorithm>
#include <cmath>

namespace td {

// 通道3: 噪声指纹
// 同一摄像机、同一次编码的画面, 传感器+量化噪声沿时间基本平稳。
// 异源片段拼接会让噪声水平出现"阶跃": 前后两段各自平稳、水平不同。
// 检测: 计算每一点前后对称窗的均值差(阶跃响应), 再用阶跃响应自身的
// 分位跨度(IQR)归一化 —— 对量化噪声、周期性内容都稳健, 不会因 MAD≈0 而除爆。
// 周期性内容起伏的前后窗均值相互抵消, 只有持续性偏移(异源拼接)才产生大阶跃。
ChannelResult channel_noise(const VideoData& vd, const Config&) {
    const auto& F = vd.frames;
    int n = (int)F.size();
    ChannelResult r;
    r.score.assign(n, 0.0f);

    std::vector<float> nz(n);
    for (int i = 0; i < n; ++i) nz[i] = F[i].noise_sigma;

    const int W = 10;
    std::vector<float> step(n, 0.0f);
    for (int i = W; i < n - W; ++i) {
        double a = 0, b = 0;
        for (int j = i - W; j < i; ++j) a += nz[j];
        for (int j = i; j < i + W; ++j) b += nz[j];
        step[i] = (float)std::fabs(b - a) / W;
    }
    // 用阶跃响应的 75 分位作为自适应尺度(常态阶跃的典型上限)
    std::vector<float> srt(step.begin() + W, step.end() - W);
    std::sort(srt.begin(), srt.end());
    float q75 = srt.empty() ? 1e-6f : srt[(size_t)(srt.size() * 0.75)];
    float scale = std::max(q75, 1e-6f);

    for (int i = W; i < n - W; ++i) {
        // 阶跃需超过常态 q75 的 1.6 倍才计分, 4 倍封顶
        float x = (step[i] / scale - 1.6f) / 2.4f;
        r.score[i] = clamp01(x);
    }
    return r;
}

} // namespace td
