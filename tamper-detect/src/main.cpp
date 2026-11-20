#include "decoder.h"
#include "detect.h"
#include "stats_util.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace td;

static const char* TYPE_CN[] = {"删帧", "插帧", "拼接", "画面局部替换"};

static std::string hms(double t) {
    int h = (int)(t / 3600), m = (int)((t - h * 3600) / 60);
    double s = t - h * 3600 - m * 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%05.2f", h, m, s);
    return buf;
}

static void usage() {
    std::cerr <<
      "用法: tamper-detect [选项] <视频文件>\n"
      "  --json <file>   输出 JSON 报告\n"
      "  --csv  <file>   输出逐帧通道分数 CSV\n"
      "  --width <px>    分析灰度宽度(默认 320)\n"
      "  --threshold <f> 报警分数阈值 0..1(默认 0.45)\n"
      "  -v               verbose\n";
}

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json" && i + 1 < argc) cfg.json_out = argv[++i];
        else if (a == "--csv" && i + 1 < argc) cfg.csv_out = argv[++i];
        else if (a == "--width" && i + 1 < argc) cfg.analysis_width = std::atoi(argv[++i]);
        else if (a == "--threshold" && i + 1 < argc)
            cfg.report_score_thresh = (float)std::atof(argv[++i]);
        else if (a == "-v") cfg.verbose = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.empty() && a[0] != '-') cfg.input = a;
        else { std::cerr << "未知参数: " << a << "\n"; usage(); return 2; }
    }
    if (cfg.input.empty()) { usage(); return 2; }

    auto t0 = std::chrono::steady_clock::now();
    VideoData vd = decode_and_extract(cfg);
    auto t1 = std::chrono::steady_clock::now();

    auto ts = channel_timestamp(vd, cfg);
    auto st = channel_structure(vd, cfg);
    auto nz = channel_noise(vd, cfg);
    auto sc = channel_scene(vd, cfg);
    auto lc = channel_local(vd, cfg);
    auto findings = fuse(vd, cfg, ts, st, nz, sc, lc);
    auto t2 = std::chrono::steady_clock::now();

    const auto& M = vd.meta;
    double dec_sec = std::chrono::duration<double>(t1 - t0).count();
    double ana_sec = std::chrono::duration<double>(t2 - t1).count();
    double dur = M.duration;

    // ---------- 控制台文本报告 ----------
    std::cout << "================ 录像篡改检测报告 ================\n";
    std::cout << "文件: " << cfg.input << "\n";
    std::cout << "编码: " << M.codec << "  " << M.width << "x" << M.height
              << "  时长 " << hms(dur) << "  帧率~" << std::fixed
              << std::setprecision(2) << M.avg_fps << "fps  帧数 "
              << vd.frames.size() << "\n";
    std::cout << "可用 side data: 运动向量=" << (M.has_mv ? "是" : "否")
              << "  QP=" << (M.has_qp ? "是" : "否") << "\n";
    std::cout << "耗时: 解码 " << std::setprecision(2) << dec_sec << "s, 分析 "
              << ana_sec << "s";
    if (dec_sec + ana_sec > 0.1 && dur > 0)
        std::cout << " (处理速度 " << dur / (dec_sec + ana_sec) << "x 实时)";
    std::cout << "\n--------------------------------------------------\n";

    if (findings.empty()) {
        std::cout << "未发现超过阈值(" << cfg.report_score_thresh
                  << ")的可疑片段。\n";
    } else {
        std::cout << "发现 " << findings.size() << " 个可疑片段:\n\n";
        int no = 1;
        for (auto& f : findings) {
            std::cout << "[" << no++ << "] " << TYPE_CN[(int)f.type]
                      << "  置信度 " << std::fixed << std::setprecision(2)
                      << f.score << "\n"
                      << "    时间: " << hms(f.start) << " ~ " << hms(f.end)
                      << "  (帧 " << f.start_idx << "~" << f.end_idx << ")\n"
                      << "    依据: " << f.evidence << "\n\n";
        }
    }
    std::cout << "说明: 结论为无参考统计推断, 置信度越高越可疑; 请结合内容复核。\n";
    std::cout << "==================================================\n";

    // ---------- JSON ----------
    if (!cfg.json_out.empty()) {
        std::ofstream j(cfg.json_out);
        j << "{\n";
        j << "  \"file\": " << std::quoted(cfg.input) << ",\n";
        j << "  \"codec\": " << std::quoted(M.codec) << ",\n";
        j << "  \"width\": " << M.width << ", \"height\": " << M.height << ",\n";
        j << "  \"duration_sec\": " << std::setprecision(6) << dur << ",\n";
        j << "  \"frame_count\": " << vd.frames.size() << ",\n";
        j << "  \"avg_fps\": " << M.avg_fps << ",\n";
        j << "  \"decode_sec\": " << dec_sec << ", \"analyze_sec\": " << ana_sec << ",\n";
        j << "  \"findings\": [\n";
        for (size_t k = 0; k < findings.size(); ++k) {
            auto& f = findings[k];
            j << "    {\"type\": " << std::quoted(verdict_name(f.type))
              << ", \"start_sec\": " << f.start
              << ", \"end_sec\": " << f.end
              << ", \"start_frame\": " << f.start_idx
              << ", \"end_frame\": " << f.end_idx
              << ", \"score\": " << f.score
              << ", \"scores\": {\"timestamp\": " << f.s_ts
              << ", \"structure\": " << f.s_struct
              << ", \"noise\": " << f.s_noise
              << ", \"scene\": " << f.s_scene
              << ", \"local\": " << f.s_local << "}"
              << ", \"evidence\": " << std::quoted(f.evidence) << "}"
              << (k + 1 < findings.size() ? "," : "") << "\n";
        }
        j << "  ]\n}\n";
    }

    // ---------- 逐帧 CSV ----------
    if (!cfg.csv_out.empty()) {
        std::ofstream c(cfg.csv_out);
        c << "frame,pts,pict_type,pkt_size,qp,sad,noise_sigma,mv_mag,duplicate,"
            "s_timestamp,s_structure,s_noise,s_scene,s_local\n";
        for (size_t i = 0; i < vd.frames.size(); ++i) {
            auto& f = vd.frames[i];
            c << i << "," << std::setprecision(6) << f.pts << "," << f.pict_type
              << "," << f.pkt_size << "," << std::setprecision(2) << f.qp
              << "," << std::scientific << std::setprecision(3) << f.sad
              << "," << f.noise_sigma << "," << f.mv_mean_mag
              << std::defaultfloat << "," << (f.duplicate ? 1 : 0)
              << "," << ts.score[i] << "," << st.score[i] << "," << nz.score[i]
              << "," << sc.score[i] << "," << lc.score[i] << "\n";
        }
    }
    return findings.empty() ? 0 : 1; // 0=干净 1=有发现, 便于脚本判断
}
