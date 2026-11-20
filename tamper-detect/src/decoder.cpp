#include "decoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/motion_vector.h>
#include <libavutil/video_enc_params.h>
#include <libswscale/swscale.h>
}

namespace td {

// ---------- 小工具 ----------
static float median_vec(std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// 对一块区域做高频残差噪声估计: 残差 = 像素 - 四邻域均值。
// 不做硬边缘屏蔽(会让小网格块样本过少、估计量化成常数); MAD 中位数统计
// 本身对边缘/运动的少量大残差不敏感, 取残差绝对值中位数 * 1.4826。
static float noise_level(const uint8_t* img, int w, int h, int stride,
                         int x0, int y0, int x1, int y1) {
    std::vector<float> res;
    res.reserve((x1 - x0) * (y1 - y0));
    for (int y = y0 + 1; y < y1 - 1; ++y) {
        const uint8_t* r = img + y * stride;
        for (int x = x0 + 1; x < x1 - 1; ++x) {
            float neigh = (r[x - 1] + r[x + 1] + r[-stride + x] + r[stride + x]) * 0.25f;
            res.push_back(static_cast<float>(r[x]) - neigh);
        }
    }
    if (res.size() < 12) return 0.0f;
    for (auto& v : res) v = std::fabs(v);
    return median_vec(res) * 1.4826f;
}

static void motion_stats(const AVFrame* frame, FrameInfo& fi) {
    AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
    if (!sd) return;
    const auto* mvs = reinterpret_cast<const AVMotionVector*>(sd->data);
    int n = sd->size / sizeof(AVMotionVector);
    double sum_mag = 0.0;
    int frac = 0;
    for (int i = 0; i < n; ++i) {
        double mx = static_cast<double>(mvs[i].motion_x) / mvs[i].motion_scale;
        double my = static_cast<double>(mvs[i].motion_y) / mvs[i].motion_scale;
        sum_mag += std::sqrt(mx * mx + my * my);
        // 亚像素: motion_x 不能被 scale 整除
        if (mvs[i].motion_x % mvs[i].motion_scale != 0 ||
            mvs[i].motion_y % mvs[i].motion_scale != 0)
            ++frac;
    }
    if (n > 0) {
        fi.mv_count = n;
        fi.mv_mean_mag = static_cast<float>(sum_mag / n);
        fi.mv_frac = static_cast<float>(frac) / n;
    }
}

static float frame_qp(const AVFrame* frame) {
    AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_VIDEO_ENC_PARAMS);
    if (!sd) return -1.0f;
    const auto* p = reinterpret_cast<const AVVideoEncParams*>(sd->data);
    if (p->type != AV_VIDEO_ENC_PARAMS_NONE)
        return static_cast<float>(p->qp);
    return -1.0f;
}

// 块内高频能量(锐度): 水平+垂直拉普拉斯响应绝对值均值。
// 画面被模糊贴片覆盖后, 该指标会显著、持续地低于该块自身的时间基线。
static float hf_energy(const uint8_t* img, int stride,
                       int x0, int y0, int x1, int y1) {
    double sum = 0.0;
    int cnt = 0;
    for (int y = y0 + 1; y < y1 - 1; ++y) {
        const uint8_t* r = img + y * stride;
        for (int x = x0 + 1; x < x1 - 1; ++x) {
            int lap = (int)r[-stride + x] + r[stride + x] + r[x - 1] + r[x + 1]
                      - 4 * (int)r[x];
            sum += std::fabs((float)lap);
            ++cnt;
        }
    }
    return cnt ? (float)(sum / cnt) : 0.0f;
}

// ---------- 主流程 ----------
VideoData decode_and_extract(const Config& cfg) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, cfg.input.c_str(), nullptr, nullptr) < 0)
        throw std::runtime_error("无法打开输入: " + cfg.input);
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        throw std::runtime_error("无法读取流信息");
    }
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vi < 0) {
        avformat_close_input(&fmt);
        throw std::runtime_error("找不到视频流");
    }
    AVStream* st = fmt->streams[vi];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        avformat_close_input(&fmt);
        throw std::runtime_error("找不到解码器");
    }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->flags2 |= AV_CODEC_FLAG2_EXPORT_MVS;   // 请求输出运动向量 side data
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        throw std::runtime_error("解码器打开失败");
    }

    VideoData vd;
    vd.meta.codec = avcodec_get_name(st->codecpar->codec_id);
    vd.meta.width = st->codecpar->width;
    vd.meta.height = st->codecpar->height;
    vd.meta.time_base = av_q2d(st->time_base);
    vd.meta.avg_fps = st->avg_frame_rate.den ? av_q2d(st->avg_frame_rate) : 0.0;
    vd.meta.duration = (st->duration != AV_NOPTS_VALUE)
                       ? st->duration * vd.meta.time_base
                       : fmt->duration / (double)AV_TIME_BASE;

    // 分析分辨率: 固定宽度, 高度按 16 对齐
    int aw = cfg.analysis_width;
    int ah = (int)std::round((double)aw * vd.meta.height / vd.meta.width);
    ah = ((ah + 15) / 16) * 16;
    vd.meta.analysis_w = aw;
    vd.meta.analysis_h = ah;

    SwsContext* sws = sws_getContext(vd.meta.width, vd.meta.height, ctx->pix_fmt,
                                     aw, ah, AV_PIX_FMT_GRAY8,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        throw std::runtime_error("sws 初始化失败");
    }

    const int stride = aw;
    std::vector<uint8_t> gray_prev(aw * ah, 0), gray_cur(aw * ah, 0);
    bool have_prev = false;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* gray = av_frame_alloc();
    gray->format = AV_PIX_FMT_GRAY8;
    gray->width = aw;
    gray->height = ah;
    av_frame_get_buffer(gray, 16);

    // packet 大小按 dts 记录, 帧重排后用 frame 的 pts/dts 关联
    std::map<int64_t, int64_t> pkt_size_by_dts;
    std::map<int64_t, bool>    pkt_key_by_dts;

    auto process_frame = [&](AVFrame* fr) {
        // 1) 灰度转换
        sws_scale(sws, fr->data, fr->linesize, 0, vd.meta.height,
                  gray->data, gray->linesize);
        std::memcpy(gray_cur.data(), gray->data[0], (size_t)aw * ah);

        FrameInfo fi;
        fi.idx = static_cast<int64_t>(vd.frames.size());
        fi.pts = (fr->pts != AV_NOPTS_VALUE) ? fr->pts * vd.meta.time_base
                                             : fi.idx / std::max(vd.meta.avg_fps, 1e-6);
        fi.pict_type = av_get_picture_type_char(fr->pict_type);
        fi.qp = frame_qp(fr);
        if (fi.qp >= 0) vd.meta.has_qp = true;
        motion_stats(fr, fi);
        if (fi.mv_count > 0) vd.meta.has_mv = true;

        auto it = pkt_size_by_dts.find(fr->pkt_dts != AV_NOPTS_VALUE ? fr->pkt_dts : fr->pts);
        if (it != pkt_size_by_dts.end()) {
            fi.pkt_size = it->second;
            fi.key_flag = pkt_key_by_dts[it->first];
        }

        // 2) 亮度 + 帧间差异
        double sum_luma = 0, ssd = 0, sad = 0;
        for (int i = 0; i < aw * ah; ++i) {
            float c = gray_cur[i];
            sum_luma += c;
            if (have_prev) {
                float d = (c - gray_prev[i]) / 255.0f;
                ssd += d * d;
                sad += std::fabs(d);
            }
        }
        fi.mean_luma = static_cast<float>(sum_luma / (aw * ah));
        if (have_prev) {
            fi.ssd = static_cast<float>(ssd / (aw * ah));
            fi.sad = static_cast<float>(sad / (aw * ah));
            fi.duplicate = fi.sad < cfg.dup_sad_thresh / 255.0f;
        }

        // 3) 噪声: 全帧 + 网格块; 同时记录各块高频能量
        fi.noise_sigma = noise_level(gray_cur.data(), aw, ah, stride, 0, 0, aw, ah);
        for (int gy = 0; gy < GRID_H; ++gy) {
            for (int gx = 0; gx < GRID_W; ++gx) {
                int x0 = gx * aw / GRID_W, x1 = (gx + 1) * aw / GRID_W;
                int y0 = gy * ah / GRID_H, y1 = (gy + 1) * ah / GRID_H;
                int c = gy * GRID_W + gx;
                fi.cell_sigma[c] =
                    noise_level(gray_cur.data(), aw, ah, stride, x0, y0, x1, y1);
                fi.cell_hf[c] = hf_energy(gray_cur.data(), stride, x0, y0, x1, y1);
            }
        }

        vd.frames.push_back(fi);
        gray_prev.swap(gray_cur);
        have_prev = true;
    };

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vi) {
            int64_t key = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
            pkt_size_by_dts[key] = pkt->size;
            pkt_key_by_dts[key] = !!(pkt->flags & AV_PKT_FLAG_KEY);
            if (avcodec_send_packet(ctx, pkt) == 0) {
                while (avcodec_receive_frame(ctx, frame) == 0)
                    process_frame(frame);
            }
        }
        av_packet_unref(pkt);
    }
    // flush
    avcodec_send_packet(ctx, nullptr);
    while (avcodec_receive_frame(ctx, frame) == 0)
        process_frame(frame);

    av_frame_free(&gray);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    sws_freeContext(sws);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);

    if (vd.frames.size() < 4)
        throw std::runtime_error("有效帧数过少, 无法分析");

    // 按 PTS 重排为显示顺序(解码输出通常已有序, 这里兜底)
    std::sort(vd.frames.begin(), vd.frames.end(),
              [](const FrameInfo& a, const FrameInfo& b) { return a.pts < b.pts; });
    for (size_t i = 0; i < vd.frames.size(); ++i) vd.frames[i].idx = (int64_t)i;

    // 帧间隔
    for (size_t i = 1; i < vd.frames.size(); ++i)
        vd.frames[i - 1].duration = vd.frames[i].pts - vd.frames[i - 1].pts;
    vd.frames.back().duration = vd.frames[vd.frames.size() - 2].duration;
    return vd;
}

} // namespace td
