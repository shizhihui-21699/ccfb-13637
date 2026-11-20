// vfcheck — 录像篡改取证检测工具
//
// 不依赖原始录像, 仅从单份文件本身检测:
//   1. 时间戳层  : DTS/PTS 间隔缺口、不单调 (删帧/插帧的容器痕迹)
//   2. 编码结构层: GOP 长度异常、帧大小(码率)突变 (剪切点重编码/拼接痕迹)
//   3. 噪声层    : 逐帧传感器噪声强度, 水平突变 -> 异源片段/二次压缩
//   4. 连续性层  : 帧间差分 -> 运动跳变(删帧/拼接); 近零差分 -> 重复/冻结帧(插帧)
//   5. 局部替换层: 分块噪声网格, 持续异常的低噪矩形 -> 画面局部覆盖
//
// 单遍扫描: 读包时做 1/2, 同时送解码器做 3/4/5 (解码后缩放到小灰度图, 速度快)。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

// ----------------------------- 数据结构 ------------------------------------
struct PacketRec {
  double time;   // 秒 (按 dts)
  int64_t dts;
  int size;
  bool key;
};

struct FrameRec {
  double time;
  double noise;             // 整帧噪声 sigma (Immerkær)
  double diff;              // 与上一帧的平均绝对差
  std::vector<float> cell;  // 分块噪声 (GRID_W x GRID_H)
};

struct Event {
  double t0, t1;
  double score;       // 0..1
  std::string type;   // 检测器名
  std::string detail; // 依据描述
};

static constexpr int AW = 192, AH = 108;   // 分析用缩略图
static constexpr int GW = 8, GH = 6;       // 噪声网格
static constexpr int GC = GW * GH;

// ----------------------------- 图像度量 ------------------------------------
// 噪声估计: Laplacian 响应的截尾均值 (去掉 15% 最大的边缘响应, 对画面内容稳健)
static double lapSigma(const uint8_t *g, int x0, int y0, int x1, int y1, int stride) {
  static std::vector<int> resp;
  resp.clear();
  for (int y = y0 + 1; y < y1 - 1; y++) {
    const uint8_t *r0 = g + (y - 1) * stride, *r1 = g + y * stride, *r2 = g + (y + 1) * stride;
    for (int x = x0 + 1; x < x1 - 1; x++)
      resp.push_back(std::abs(4 * r1[x] - r1[x - 1] - r1[x + 1] - r0[x] - r2[x]));
  }
  if (resp.size() < 16) return 0;
  size_t cut = resp.size() * 85 / 100;
  std::nth_element(resp.begin(), resp.begin() + cut, resp.end());
  double sum = 0;
  for (size_t i = 0; i < cut; i++) sum += resp[i];
  return (sum / cut) / 3.564;  // 5 点 Laplacian: E|r| = σ·√20·√(2/π)
}

// 3x3 盒式模糊 (抑制逐像素噪声, 让内容跳变在帧差中凸显)
static void boxBlur3(const uint8_t *src, uint8_t *dst, int w, int h, int stride) {
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int s = 0;
      for (int dy = -1; dy <= 1; dy++) {
        int yy = std::min(h - 1, std::max(0, y + dy));
        const uint8_t *r = src + yy * stride;
        s += r[std::max(0, x - 1)] + r[x] + r[std::min(w - 1, x + 1)];
      }
      dst[y * w + x] = (uint8_t)(s / 9);
    }
  }
}

// ----------------------------- 统计工具 ------------------------------------
static double medianOf(std::vector<double> v) {
  if (v.empty()) return 0;
  size_t m = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + m, v.end());
  return v[m];
}

static std::vector<double> rollingMedian(const std::vector<double> &x, int w) {
  int n = (int)x.size();
  std::vector<double> out(n);
  std::vector<double> buf;
  buf.reserve(w + 1);
  for (int i = 0; i < n; i++) {
    int a = std::max(0, i - w / 2), b = std::min(n - 1, i + w / 2);
    buf.assign(x.begin() + a, x.begin() + b + 1);
    size_t m = buf.size() / 2;
    std::nth_element(buf.begin(), buf.begin() + m, buf.end());
    out[i] = buf[m];
  }
  return out;
}

static double madOf(const std::vector<double> &x, double med) {
  std::vector<double> d;
  d.reserve(x.size());
  for (double v : x) d.push_back(std::fabs(v - med));
  return medianOf(std::move(d));
}

// 把帧下标的连续区间转成事件
static void runsToEvents(const std::vector<char> &flag, const std::vector<FrameRec> &fr,
                         int minRun, int mergeGap, double score, const char *type,
                         const std::string &detail, std::vector<Event> &out) {
  int n = (int)flag.size(), i = 0;
  while (i < n) {
    if (!flag[i]) { i++; continue; }
    int last = i, j = i;
    while (j < n) {
      if (flag[j]) { last = j; j++; }
      else if (j - last <= mergeGap) j++;
      else break;
    }
    if (last - i + 1 >= minRun)
      out.push_back({fr[i].time, fr[last].time, score, type, detail});
    i = last + 1;
  }
}

// ----------------------------- 检测器 --------------------------------------
// 1. 时间戳: DTS 间隔缺口 / 不单调
static void detectTimestamps(const std::vector<PacketRec> &pk, std::vector<Event> &ev) {
  if (pk.size() < 10) return;
  std::vector<double> deltas;
  for (size_t i = 1; i < pk.size(); i++)
    if (pk[i].dts > pk[i - 1].dts) deltas.push_back((double)(pk[i].dts - pk[i - 1].dts));
  if (deltas.empty()) return;
  double mode = medianOf(deltas);
  if (mode <= 0) return;
  for (size_t i = 1; i < pk.size(); i++) {
    double d = (double)(pk[i].dts - pk[i - 1].dts);
    if (d > 1.6 * mode)
      ev.push_back({pk[i - 1].time, pk[i].time, 0.8, "timestamp",
                    "DTS 缺口: 间隔 " + std::to_string(d / mode) + " 倍正常帧距 (缺失约 " +
                        std::to_string((int)std::lround(d / mode) - 1) + " 帧)"});
    else if (d <= 0)
      ev.push_back({pk[i - 1].time, pk[i].time, 0.5, "timestamp",
                    "DTS 不单调/重叠 (拼接或重打包痕迹)"});
  }
}

// 2a. GOP 结构: 关键帧间隔异常
static void detectGOP(const std::vector<PacketRec> &pk, std::vector<Event> &ev) {
  std::vector<size_t> keys;
  for (size_t i = 0; i < pk.size(); i++)
    if (pk[i].key) keys.push_back(i);
  if (keys.size() < 4) return;
  std::vector<double> sizes;
  for (size_t i = 1; i < keys.size(); i++) sizes.push_back((double)(keys[i] - keys[i - 1]));
  double mode = medianOf(sizes);
  if (mode < 2) return;
  for (size_t i = 1; i < keys.size(); i++) {
    double g = (double)(keys[i] - keys[i - 1]);
    if (std::fabs(g - mode) > 1.5 && i != keys.size() - 1) {
      ev.push_back({pk[keys[i - 1]].time, pk[keys[i]].time, 0.5, "gop",
                    "GOP 长度 " + std::to_string((int)g) + " 帧, 正常为 " +
                        std::to_string((int)mode) + " 帧 (剪切/拼接点常破坏 GOP 周期)"});
    }
  }
}

// 2b. 帧大小(码率) 水平突变: 局部中值 vs 全局中值
static void detectSizeRegime(const std::vector<PacketRec> &pk, std::vector<Event> &ev) {
  std::vector<double> sz, tm;
  for (auto &p : pk)
    if (!p.key) { sz.push_back((double)p.size); tm.push_back(p.time); }
  if (sz.size() < 200) return;
  double gm = medianOf(sz);
  if (gm <= 0) return;
  auto rm = rollingMedian(sz, 151);
  std::vector<char> flag(sz.size());
  for (size_t i = 0; i < sz.size(); i++)
    flag[i] = (rm[i] > 1.7 * gm || rm[i] < 0.55 * gm) ? 1 : 0;
  // 转成事件 (复用帧事件工具不方便, 手写)
  int n = (int)flag.size(), i = 0;
  while (i < n) {
    if (!flag[i]) { i++; continue; }
    int j = i;
    while (j + 1 < n && flag[j + 1]) j++;
    if (j - i + 1 >= 30)
      ev.push_back({tm[i], tm[j], 0.45, "bitrate",
                    "P 帧码率水平突变: 局部中值 " + std::to_string((int)rm[i]) + " 字节 vs 全局 " +
                        std::to_string((int)gm) + " 字节 (异源编码器/重压缩片段)"});
    i = j + 1;
  }
}

// 3. 噪声水平: a) 瞬时偏离滚动中值  b) 滚动中值相对全局中值的持续漂移 (拼接/异源片段)
static void detectNoise(const std::vector<FrameRec> &fr, std::vector<Event> &ev) {
  if (fr.size() < 100) return;
  std::vector<double> x;
  for (auto &f : fr) x.push_back(f.noise);
  auto rm = rollingMedian(x, 151);
  double gm = medianOf(x);
  // a) 瞬时偏离
  {
    std::vector<double> res;
    for (size_t i = 0; i < x.size(); i++) res.push_back(x[i] - rm[i]);
    double mad = 1.4826 * madOf(res, 0.0);
    if (mad > 1e-6) {
      std::vector<char> flag(x.size());
      for (size_t i = 0; i < x.size(); i++) flag[i] = std::fabs(res[i]) / mad > 6.0 ? 1 : 0;
      runsToEvents(flag, fr, 8, 2, 0.7, "noise",
                   "噪声强度瞬时偏离局部基线 (片段被重压缩或来自其他摄像头)", ev);
    }
  }
  // b) 持续漂移: 滚动中值偏离全局中值 ±35% 以上
  if (gm > 0.2) {
    std::vector<char> flag(x.size());
    for (size_t i = 0; i < x.size(); i++)
      flag[i] = (rm[i] > 1.35 * gm || rm[i] < 0.65 * gm) ? 1 : 0;
    int n = (int)flag.size(), i = 0;
    while (i < n) {
      if (!flag[i]) { i++; continue; }
      int j = i;
      while (j + 1 < n && flag[j + 1]) j++;
      if (j - i + 1 >= 50) {  // 持续 2 秒以上
        double m = 0;
        for (int k = i; k <= j; k++) m += rm[k];
        m /= (j - i + 1);
        ev.push_back({fr[i].time, fr[j].time, 0.7, "noise",
                      "噪声水平与全片主体不一致: 该段 σ≈" + std::to_string(m).substr(0, 5) +
                          " vs 全片 σ≈" + std::to_string(gm).substr(0, 5) +
                          (m < gm ? " (重压缩或异源片段)" : " (异源传感器)")});
      }
      i = j + 1;
    }
  }
}

// 4a. 运动连续性: 帧间差分跳变 (删帧/拼接)。帧差在模糊图上计算, 见 handleFrame。
static void detectDiscontinuity(const std::vector<FrameRec> &fr, std::vector<Event> &ev) {
  if (fr.size() < 40) return;
  std::vector<double> d;
  for (auto &f : fr) d.push_back(f.diff);
  auto rm = rollingMedian(d, 31);
  double gm = medianOf(d);
  double mad = 1.4826 * madOf(d, gm);
  if (mad < 1e-6) mad = 1e-6;
  for (size_t i = 2; i < d.size() - 1; i++) {
    double b = std::max(rm[i], 0.1);
    double z = (d[i] - gm) / mad;
    // 同时要求: 相对局部基线突增 + 全局离群 (排除周期性画面内容造成的规律性尖峰)
    if (d[i] > 3.5 * b && d[i] > 0.7 && d[i] > 1.8 * d[i - 1] && z > 10) {
      ev.push_back({fr[i].time - 0.2, fr[i].time + 0.2, 0.6, "discontinuity",
                    "画面运动不连续: 帧差 " + std::to_string(d[i]).substr(0, 4) + " vs 局部基线 " +
                        std::to_string(b).substr(0, 4) + " (删帧跳变或拼接切口)"});
    }
  }
}

// 4b. 重复/冻结帧 (插帧凑时长): 帧差几乎为 0 且持续 —— 正常录像有传感器噪声, 不会如此
static void detectFrozen(const std::vector<FrameRec> &fr, std::vector<Event> &ev) {
  if (fr.size() < 20) return;
  std::vector<char> flag(fr.size());
  for (size_t i = 0; i < fr.size(); i++) flag[i] = (i > 0 && fr[i].diff < 0.02) ? 1 : 0;
  runsToEvents(flag, fr, 10, 2, 0.75, "frozen",
               "连续多帧内容完全相同 (帧差≈0): 重复帧/冻结帧插入", ev);
}

// 5. 局部区域替换: 每个分块与自身的时域基线比较 —— 被外来内容覆盖时,
//    该位置的噪声水平会偏离其历史值。排除绝对水平过低的纯平坦内容(运动物体),
//    要求异常簇紧凑且持续。
static void detectRegion(const std::vector<FrameRec> &fr, std::vector<Event> &ev) {
  if (fr.size() < 100 || fr[0].cell.size() != GC) return;
  int n = (int)fr.size();
  // 全局噪声水平 (用于排除"纯平坦运动物体": 那种块噪声≈0, 而粘贴块带有源视频的噪声)
  std::vector<double> allNoise;
  for (auto &f : fr) allNoise.push_back(f.noise);
  double gm = medianOf(allNoise);
  // 每格的时域基线: 全片中位数 (固定机位下, 长时段篡改也不会把基线带偏)
  std::vector<std::vector<double>> base(GC);
  for (int c = 0; c < GC; c++) {
    std::vector<double> s(n);
    for (int t = 0; t < n; t++) s[t] = fr[t].cell[c];
    double m = medianOf(s);
    base[c].assign(n, m);
  }
  std::vector<char> cand(n, 0);
  std::vector<std::array<int, 4>> rect(n);
  for (int t = 0; t < n; t++) {
    int flagged = 0, minx = GW, miny = GH, maxx = -1, maxy = -1;
    for (int c = 0; c < GC; c++) {
      double val = fr[t].cell[c], b = base[c][t];
      if (b < 0.05) continue;  // 该位置本来就平坦, 无从判断
      bool low = val < 0.6 * b && b - val > 0.05 && val > 0.25 * gm;
      bool high = val > 1.9 * b && val - b > 0.05;
      if (low || high) {
        flagged++;
        int cx = c % GW, cy = c / GW;
        minx = std::min(minx, cx); maxx = std::max(maxx, cx);
        miny = std::min(miny, cy); maxy = std::max(maxy, cy);
      }
    }
    if (flagged >= 2 && flagged <= GC / 3) {
      // 紧凑性: 命中格占外接框 60% 以上
      int box = (maxx - minx + 1) * (maxy - miny + 1);
      if (flagged * 5 >= box * 3) {
        cand[t] = 1;
        rect[t] = {minx, miny, maxx, maxy};
      }
    }
  }
  // 候选帧连续段 (允许小空洞), 累计 ≥12 帧 -> 事件
  int i = 0;
  while (i < n) {
    if (!cand[i]) { i++; continue; }
    int j = i;
    while (j + 1 < n && (cand[j + 1] || (j + 6 < n && cand[j + 6]))) j++;
    int last = i;
    for (int k = i; k <= j; k++) if (cand[k]) last = k;
    int cnt = 0, minx = GW, miny = GH, maxx = -1, maxy = -1;
    for (int k = i; k <= last; k++)
      if (cand[k]) {
        cnt++;
        minx = std::min(minx, rect[k][0]); miny = std::min(miny, rect[k][1]);
        maxx = std::max(maxx, rect[k][2]); maxy = std::max(maxy, rect[k][3]);
      }
    if (cnt >= 12) {
      char buf[256];
      snprintf(buf, sizeof(buf),
               "画面局部区域噪声与其历史水平持续不一致 (%d 帧): 区域约 (%d%%,%d%%)-(%d%%,%d%%) "
               "(局部覆盖/替换痕迹)",
               cnt, minx * 100 / GW, miny * 100 / GH, (maxx + 1) * 100 / GW, (maxy + 1) * 100 / GH);
      ev.push_back({fr[i].time, fr[last].time, 0.9, "region", buf});
    }
    i = j + 1;
  }
}

// ----------------------------- 证据融合 ------------------------------------
struct Group {
  double t0, t1, score;
  std::vector<const Event *> reasons;
};

static std::vector<Group> fuse(std::vector<Event> &ev) {
  std::sort(ev.begin(), ev.end(), [](const Event &a, const Event &b) { return a.t0 < b.t0; });
  std::vector<Group> gs;
  for (auto &e : ev) {
    if (gs.empty() || e.t0 > gs.back().t1 + 2.5) {
      gs.push_back({e.t0, e.t1, 0, {}});
      gs.back().reasons.push_back(&e);
    } else {
      auto &g = gs.back();
      g.t1 = std::max(g.t1, e.t1);
      g.reasons.push_back(&e);
    }
  }
  for (auto &g : gs) {
    double prod = 1.0;
    for (auto r : g.reasons) prod *= (1.0 - r->score);
    g.score = 1.0 - prod;  // noisy-or
  }
  std::sort(gs.begin(), gs.end(), [](const Group &a, const Group &b) { return a.t0 < b.t0; });
  return gs;
}

// ----------------------------- 主流程 --------------------------------------
static void usage() {
  fprintf(stderr,
          "用法: vfcheck <录像文件> [--json 报告.json] [--verbose]\n"
          "  单遍扫描, 输出可疑时间段及依据。退出码: 0=未发现, 1=发现可疑, 2=错误\n");
}

int main(int argc, char **argv) {
  if (argc < 2) { usage(); return 2; }
  std::string input, jsonPath;
  bool verbose = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--json" && i + 1 < argc) jsonPath = argv[++i];
    else if (a == "--verbose") verbose = true;
    else if (a[0] != '-') input = a;
  }
  if (input.empty()) { usage(); return 2; }

  AVFormatContext *fmt = nullptr;
  if (avformat_open_input(&fmt, input.c_str(), nullptr, nullptr) < 0) {
    fprintf(stderr, "无法打开文件: %s\n", input.c_str());
    return 2;
  }
  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    fprintf(stderr, "无法读取流信息\n");
    return 2;
  }
  int vsi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vsi < 0) { fprintf(stderr, "没有视频流\n"); return 2; }
  AVStream *vs = fmt->streams[vsi];
  const AVCodec *dec = avcodec_find_decoder(vs->codecpar->codec_id);
  if (!dec) { fprintf(stderr, "不支持的编码\n"); return 2; }
  AVCodecContext *cc = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(cc, vs->codecpar);
  cc->thread_count = 0;  // 自动多线程解码
  if (avcodec_open2(cc, dec, nullptr) < 0) { fprintf(stderr, "解码器打开失败\n"); return 2; }

  double tb = av_q2d(vs->time_base);
  std::vector<PacketRec> packets;
  std::vector<FrameRec> frames;
  frames.reserve(100000);

  SwsContext *sws = nullptr;
  int sw = -1, sh = -1;
  AVPixelFormat spf = AV_PIX_FMT_NONE;
  std::vector<uint8_t> gray(AW * AH), blur(AW * AH), prevBlur;
  bool havePrev = false;

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frm = av_frame_alloc();
  int64_t frameIdx = 0;
  int64_t tStart = av_gettime_relative();

  auto handleFrame = [&](AVFrame *f) {
    if (f->width != sw || f->height != sh || (AVPixelFormat)f->format != spf) {
      if (sws) sws_freeContext(sws);
      sws = sws_getContext(f->width, f->height, (AVPixelFormat)f->format, AW, AH,
                           AV_PIX_FMT_GRAY8, SWS_BILINEAR, nullptr, nullptr, nullptr);
      sw = f->width; sh = f->height; spf = (AVPixelFormat)f->format;
      havePrev = false;
    }
    if (!sws) return;
    uint8_t *dst[4] = {gray.data(), nullptr, nullptr, nullptr};
    int dstride[4] = {AW, 0, 0, 0};
    sws_scale(sws, f->data, f->linesize, 0, f->height, dst, dstride);

    FrameRec rec;
    int64_t ts = f->pts != AV_NOPTS_VALUE ? f->pts : frameIdx;
    rec.time = ts * tb;
    rec.noise = lapSigma(gray.data(), 0, 0, AW, AH, AW);
    boxBlur3(gray.data(), blur.data(), AW, AH, AW);
    if (havePrev) {
      double s = 0;
      for (int i = 0; i < AW * AH; i++) s += std::abs((int)blur[i] - (int)prevBlur[i]);
      rec.diff = s / (AW * AH);
    } else rec.diff = 0;
    rec.cell.resize(GC);
    for (int cy = 0; cy < GH; cy++)
      for (int cx = 0; cx < GW; cx++)
        rec.cell[cy * GW + cx] = (float)lapSigma(gray.data(), cx * AW / GW, cy * AH / GH,
                                                 (cx + 1) * AW / GW, (cy + 1) * AH / GH, AW);
    prevBlur = blur;
    havePrev = true;
    frames.push_back(std::move(rec));
    frameIdx++;
  };

  while (av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == vsi) {
      PacketRec pr;
      pr.dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : (pkt->pts != AV_NOPTS_VALUE ? pkt->pts : 0);
      pr.time = pr.dts * tb;
      pr.size = pkt->size;
      pr.key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
      packets.push_back(pr);
      if (avcodec_send_packet(cc, pkt) >= 0)
        while (avcodec_receive_frame(cc, frm) >= 0) { handleFrame(frm); av_frame_unref(frm); }
    }
    av_packet_unref(pkt);
  }
  avcodec_send_packet(cc, nullptr);
  while (avcodec_receive_frame(cc, frm) >= 0) { handleFrame(frm); av_frame_unref(frm); }

  double elapsed = (av_gettime_relative() - tStart) / 1e6;
  double duration = frames.empty() ? 0 : frames.back().time;

  // ---- 运行全部检测器 ----
  std::vector<Event> events;
  detectTimestamps(packets, events);
  detectGOP(packets, events);
  detectSizeRegime(packets, events);
  detectNoise(frames, events);
  detectDiscontinuity(frames, events);
  detectFrozen(frames, events);
  detectRegion(frames, events);
  auto groups = fuse(events);

  // ---- 输出 ----
  printf("文件: %s\n", input.c_str());
  printf("时长: %.1f 秒, 解码帧数: %zu, 数据包: %zu\n", duration, frames.size(), packets.size());
  printf("分析耗时: %.1f 秒 (%.0f 帧/秒, 实时速度的 %.1f 倍)\n", elapsed,
         frames.size() / std::max(1e-9, elapsed),
         duration / std::max(1e-9, elapsed));
  if (verbose) {
    std::vector<double> nz, df;
    for (auto &f : frames) { nz.push_back(f.noise); df.push_back(f.diff); }
    if (!nz.empty())
      fprintf(stderr, "[verbose] 噪声σ 中值 %.3f, 帧差 中值 %.3f / 最大 %.3f\n",
              medianOf(nz), medianOf(df), *std::max_element(df.begin(), df.end()));
  }
  printf("------------------------------------------------------------\n");
  if (groups.empty()) {
    printf("未发现可疑篡改痕迹。\n");
  } else {
    printf("发现 %zu 个可疑时间段:\n\n", groups.size());
    for (auto &g : groups) {
      const char *conf = g.score >= 0.85 ? "高" : (g.score >= 0.6 ? "中" : "低");
      printf("  [置信度:%s 得分:%.2f]  %.1f s — %.1f s\n", conf, g.score, g.t0, g.t1);
      for (auto r : g.reasons) printf("      • [%s] %s\n", r->type.c_str(), r->detail.c_str());
      printf("\n");
    }
  }

  if (!jsonPath.empty()) {
    FILE *jf = fopen(jsonPath.c_str(), "w");
    if (jf) {
      fprintf(jf, "{\n  \"file\": \"%s\",\n  \"duration\": %.2f,\n  \"events\": [\n",
              input.c_str(), duration);
      for (size_t i = 0; i < groups.size(); i++) {
        auto &g = groups[i];
        fprintf(jf, "    {\"t0\": %.2f, \"t1\": %.2f, \"score\": %.3f, \"reasons\": [",
                g.t0, g.t1, g.score);
        for (size_t k = 0; k < g.reasons.size(); k++) {
          fprintf(jf, "%s{\"type\": \"%s\", \"detail\": \"", k ? ", " : "",
                  g.reasons[k]->type.c_str());
          for (char c : g.reasons[k]->detail) {
            if (c == '"' || c == '\\') fputc('\\', jf);
            if ((unsigned char)c >= 0x20) fputc(c, jf);
          }
          fputc('"', jf);
          fputc('}', jf);
        }
        fprintf(jf, "]}%s\n", i + 1 < groups.size() ? "," : "");
      }
      fprintf(jf, "  ]\n}\n");
      fclose(jf);
      fprintf(stderr, "JSON 报告已写入 %s\n", jsonPath.c_str());
    }
  }

  av_frame_free(&frm);
  av_packet_free(&pkt);
  if (sws) sws_freeContext(sws);
  avcodec_free_context(&cc);
  avformat_close_input(&fmt);
  return groups.empty() ? 0 : 1;
}
