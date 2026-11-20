#!/usr/bin/env bash
# 真实感测试集: 基于平滑连续的 mandelbrot 源(无周期性硬切, 贴近固定机位监控),
# 生成与 tests/gen_samples.sh 同构的 5 类篡改 + 阴性对照。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MAT="$HERE/material"
mkdir -p "$MAT"
FPS=25
X264="-c:v libx264 -preset medium -pix_fmt yuv420p"

echo "== 真实感原始片 A: mandelbrot 平滑缩放, crf23 GOP25, 30s"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "mandelbrot=size=640x360:rate=${FPS}" -t 30 \
  -vf "noise=alls=10:allf=t,format=yuv420p" $X264 -crf 23 -g 25 -an "$MAT/real_orig.mp4"

echo "== 异源片 B: life 图案 + 强噪声, crf28 GOP50"
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "life=size=640x360:rate=${FPS}:mold=8:ratio=0.15" -t 30 \
  -vf "noise=alls=20:allf=t,format=yuv420p" $X264 -crf 28 -g 50 -an "$MAT/real_source_b.mp4"

echo "== 删帧(重编码)"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/real_orig.mp4" \
  -filter_complex "[0:v]trim=start=0:end=10,setpts=PTS-STARTPTS[a]; \
                   [0:v]trim=start=12:end=30,setpts=PTS-STARTPTS[b]; \
                   [a][b]concat=n=2:v=1[v]" -map "[v]" $X264 -crf 23 -g 25 -an "$MAT/real_deletion.mp4"

echo "== 插帧(异源 1.5s, 重编码)"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/real_orig.mp4" -i "$MAT/real_source_b.mp4" \
  -filter_complex "[0:v]trim=start=0:end=15,setpts=PTS-STARTPTS[a]; \
                   [1:v]trim=start=2:end=3.5,setpts=PTS-STARTPTS[b]; \
                   [0:v]trim=start=15:end=30,setpts=PTS-STARTPTS[c]; \
                   [a][b][c]concat=n=3:v=1[v]" -map "[v]" $X264 -crf 23 -g 25 -an "$MAT/real_insertion.mp4"

echo "== 拼接(A+B+A, 重编码)"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/real_orig.mp4" -i "$MAT/real_source_b.mp4" \
  -filter_complex "[0:v]trim=start=0:end=12,setpts=PTS-STARTPTS[a]; \
                   [1:v]trim=start=2:end=14,setpts=PTS-STARTPTS[b]; \
                   [0:v]trim=start=12:end=30,setpts=PTS-STARTPTS[c]; \
                   [a][b][c]concat=n=3:v=1[v]" -map "[v]" $X264 -crf 23 -g 25 -an "$MAT/real_splice.mp4"

echo "== 局部替换(模糊+异噪声覆盖 8~20s)"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/real_orig.mp4" \
  -filter_complex "[0:v]crop=160:120:120:80,gblur=sigma=10,noise=alls=30:allf=t[p]; \
                   [0:v][p]overlay=120:80:enable='between(t,8,20)'[v]" \
  -map "[v]" $X264 -crf 23 -g 25 -an "$MAT/real_local_replace.mp4"

echo "== 冻结插帧(帧 449 重复 12 次)"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/real_orig.mp4" \
  -vf "loop=loop=12:size=1:start=449" $X264 -crf 23 -g 25 -an "$MAT/real_freeze.mp4"

echo "== 阴性对照: 同源硬切剪辑"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/real_orig.mp4" \
  -filter_complex "[0:v]trim=start=0:end=8,setpts=PTS-STARTPTS[a]; \
                   [0:v]trim=start=14:end=22,setpts=PTS-STARTPTS[b]; \
                   [0:v]trim=start=22:end=30,setpts=PTS-STARTPTS[c]; \
                   [a][b][c]concat=n=3:v=1[v]" -map "[v]" $X264 -crf 23 -g 25 -an "$MAT/real_clean_edited.mp4"
echo "完成"
