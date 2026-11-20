#!/usr/bin/env bash
# 固定机位监控场景测试集: 静止噪声背景 + 移动目标, 符合局部检测的适用前提。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; MAT="$HERE/material"; mkdir -p "$MAT"
FPS=25; X264="-c:v libx264 -preset medium -pix_fmt yuv420p"

echo "== 原始片 A: 静止背景 + 移动目标"
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "color=c=0x2a3540:size=640x360:rate=25" -t 30 \
  -f lavfi -i "gradients=size=640x360:rate=25:speed=0.03" -t 30 \
  -filter_complex "[1:v]format=gray,boxblur=8[bg]; \
    color=c=0x556677:size=120x90:rate=25:d=30,drawbox=x=0:y=0:w=120:h=90:color=0x8899aa:t=fill[b2]; \
    [bg][b2]overlay=x='80+250*sin(2*PI*t/12)':y='135+60*cos(2*PI*t/9)'[mv]; \
    [0:v][mv]blend=all_mode=overlay:all_opacity=0.92,noise=alls=9:allf=t,format=yuv420p[v]" \
  -map "[v]" $X264 -crf 23 -g 25 -an "$MAT/cam_orig.mp4"

echo "== 异源片 B: 不同图案/噪声/编码参数"
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "gradients=size=640x360:rate=25:speed=0.08:c0=red:c1=blue" -t 30 \
  -vf "noise=alls=22:allf=t,format=yuv420p" $X264 -crf 28 -g 50 -an "$MAT/cam_source_b.mp4"

echo "== 删帧(重编码, 删 10~12s)"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/cam_orig.mp4" \
  -filter_complex "[0:v]trim=start=0:end=10,setpts=PTS-STARTPTS[a]; \
                   [0:v]trim=start=12:end=30,setpts=PTS-STARTPTS[b]; \
                   [a][b]concat=n=2:v=1[v]" -map "[v]" $X264 -crf 23 -g 25 -an "$MAT/cam_deletion.mp4"

echo "== 插帧(15s 处插入异源 1.5s)"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/cam_orig.mp4" -i "$MAT/cam_source_b.mp4" \
  -filter_complex "[0:v]trim=start=0:end=15,setpts=PTS-STARTPTS[a]; \
                   [1:v]trim=start=2:end=3.5,setpts=PTS-STARTPTS[b]; \
                   [0:v]trim=start=15:end=30,setpts=PTS-STARTPTS[c]; \
                   [a][b][c]concat=n=3:v=1[v]" -map "[v]" $X264 -crf 23 -g 25 -an "$MAT/cam_insertion.mp4"

echo "== 拼接(A 12s + B 12s + A 18s)"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/cam_orig.mp4" -i "$MAT/cam_source_b.mp4" \
  -filter_complex "[0:v]trim=start=0:end=12,setpts=PTS-STARTPTS[a]; \
                   [1:v]trim=start=2:end=14,setpts=PTS-STARTPTS[b]; \
                   [0:v]trim=start=12:end=30,setpts=PTS-STARTPTS[c]; \
                   [a][b][c]concat=n=3:v=1[v]" -map "[v]" $X264 -crf 23 -g 25 -an "$MAT/cam_splice.mp4"

echo "== 局部替换(8~20s 模糊+异噪声覆盖 x120 y80)"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/cam_orig.mp4" \
  -filter_complex "[0:v]crop=160:120:120:80,gblur=sigma=12,noise=alls=30:allf=t[p]; \
                   [0:v][p]overlay=120:80:enable='between(t,8,20)'[v]" \
  -map "[v]" $X264 -crf 23 -g 25 -an "$MAT/cam_local_replace.mp4"

echo "== 冻结插帧(帧 449 重复 12 次)"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/cam_orig.mp4" \
  -vf "loop=loop=12:size=1:start=449" $X264 -crf 23 -g 25 -an "$MAT/cam_freeze.mp4"

echo "== 阴性对照: 同源硬切剪辑"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/cam_orig.mp4" \
  -filter_complex "[0:v]trim=start=0:end=8,setpts=PTS-STARTPTS[a]; \
                   [0:v]trim=start=14:end=22,setpts=PTS-STARTPTS[b]; \
                   [0:v]trim=start=22:end=30,setpts=PTS-STARTPTS[c]; \
                   [a][b][c]concat=n=3:v=1[v]" -map "[v]" $X264 -crf 23 -g 25 -an "$MAT/cam_clean_edited.mp4"

echo "== 流拷贝拼接(不重编码, 保留原始 PTS/GOP, 接缝在 12s)"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/cam_orig.mp4" -t 12 -c copy "$MAT/_c1.mp4"
ffmpeg -hide_banner -loglevel error -y -ss 14 -i "$MAT/cam_orig.mp4" -c copy "$MAT/_c2.mp4"
printf "file '%s'\nfile '%s'\n" "$MAT/_c1.mp4" "$MAT/_c2.mp4" > "$MAT/_cc.txt"
ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i "$MAT/_cc.txt" -c copy "$MAT/cam_splice_copy.mp4"
rm -f "$MAT/_c1.mp4" "$MAT/_c2.mp4" "$MAT/_cc.txt"
echo "完成"
