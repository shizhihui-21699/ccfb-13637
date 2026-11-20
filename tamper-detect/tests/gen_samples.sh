#!/usr/bin/env bash
# 合成测试录像: 1 个干净原始片 + 5 类篡改片 + 2 个阴性对照 + 1 个长片(速度测试)。
# 全部由 ffmpeg lavfi 源/滤镜生成, 不依赖外部素材。
# 输出目录 tests/material 与脚本分离; 同时生成 ground_truth.json。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
MAT="$HERE/material"
mkdir -p "$MAT"

W=640; H=360; FPS=25
X264="-c:v libx264 -preset medium -pix_fmt yuv420p"

echo "== [1/9] 原始片 A: testsrc2 + 传感器噪声, crf23 GOP25 (30s)"
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc2=size=${W}x${H}:rate=${FPS}:duration=30" \
  -vf "noise=alls=12:allf=t" \
  $X264 -crf 23 -g 25 -an "$MAT/orig.mp4"

echo "== [2/9] 异源片 B: testsrc + 更强噪声, crf28 GOP50 (30s, 不同编码指纹)"
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc=size=${W}x${H}:rate=${FPS}:duration=30" \
  -vf "noise=alls=22:allf=t" \
  $X264 -crf 28 -g 50 -an "$MAT/source_b.mp4"

echo "== [3/9] 删帧: 删除 t=10~12s 后整体重编码 (28s)"
ffmpeg -hide_banner -loglevel error -y \
  -i "$MAT/orig.mp4" \
  -filter_complex "[0:v]trim=start=0:end=10,setpts=PTS-STARTPTS[a]; \
                   [0:v]trim=start=12:end=30,setpts=PTS-STARTPTS[b]; \
                   [a][b]concat=n=2:v=1[v]" -map "[v]" \
  $X264 -crf 23 -g 25 -an "$MAT/tamper_deletion.mp4"

echo "== [4/9] 插帧: t=15s 处插入异源 1.5s 片段后重编码 (31.5s)"
ffmpeg -hide_banner -loglevel error -y \
  -i "$MAT/orig.mp4" -i "$MAT/source_b.mp4" \
  -filter_complex "[0:v]trim=start=0:end=15,setpts=PTS-STARTPTS[a]; \
                   [1:v]trim=start=0:end=1.5,setpts=PTS-STARTPTS[b]; \
                   [0:v]trim=start=15:end=30,setpts=PTS-STARTPTS[c]; \
                   [a][b][c]concat=n=3:v=1[v]" -map "[v]" \
  $X264 -crf 23 -g 25 -an "$MAT/tamper_insertion.mp4"

echo "== [5/9] 拼接: A(0-12) + B(0-12) + A(12-30), 整体二次编码"
ffmpeg -hide_banner -loglevel error -y \
  -i "$MAT/orig.mp4" -i "$MAT/source_b.mp4" \
  -filter_complex "[0:v]trim=start=0:end=12,setpts=PTS-STARTPTS[a]; \
                   [1:v]trim=start=0:end=12,setpts=PTS-STARTPTS[b]; \
                   [0:v]trim=start=12:end=30,setpts=PTS-STARTPTS[c]; \
                   [a][b][c]concat=n=3:v=1[v]" -map "[v]" \
  $X264 -crf 23 -g 25 -an "$MAT/tamper_splice.mp4"

echo "== [6/9] 拼接(流拷贝版): 分段编码后 concat copy, GOP 在接缝处重置"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/orig.mp4" -t 12 -c copy "$MAT/_p1.mp4"
ffmpeg -hide_banner -loglevel error -y -i "$MAT/source_b.mp4" -t 12 -c copy "$MAT/_p2.mp4"
ffmpeg -hide_banner -loglevel error -y -ss 12 -i "$MAT/orig.mp4" -c copy "$MAT/_p3.mp4"
printf "file '%s'\nfile '%s'\nfile '%s'\n" "$MAT/_p1.mp4" "$MAT/_p2.mp4" "$MAT/_p3.mp4" \
  > "$MAT/_concat.txt"
ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i "$MAT/_concat.txt" \
  -c copy "$MAT/tamper_splice_copy.mp4"
rm -f "$MAT/_p1.mp4" "$MAT/_p2.mp4" "$MAT/_p3.mp4" "$MAT/_concat.txt"

echo "== [7/9] 画面局部替换: t=8~20s 一块区域模糊+异噪声覆盖后重编码"
# 区域 x=120..280 y=80..200 (640x360 下归一化 x[0.1875,0.4375] y[0.2222,0.5556])
ffmpeg -hide_banner -loglevel error -y \
  -i "$MAT/orig.mp4" \
  -filter_complex "[0:v]crop=160:120:120:80,gblur=sigma=10,noise=alls=30:allf=t[p]; \
                   [0:v][p]overlay=120:80:enable='between(t,8,20)'[v]" \
  -map "[v]" $X264 -crf 23 -g 25 -an "$MAT/tamper_local_replace.mp4"

echo "== [8/9] 冻结插帧: t=18s 处 1 帧被冻结 12 次 (loop 滤镜)"
# start=449 即 17.96s; loop=12 表示该帧额外重复 12 次, 总长增加 12 帧
ffmpeg -hide_banner -loglevel error -y -i "$MAT/orig.mp4" \
  -vf "loop=loop=12:size=1:start=449" \
  $X264 -crf 23 -g 25 -an "$MAT/tamper_freeze.mp4"

echo "== [9/9] 阴性对照: 同源硬切剪辑(噪声指纹一致, 不应报警)"
ffmpeg -hide_banner -loglevel error -y \
  -i "$MAT/orig.mp4" \
  -filter_complex "[0:v]trim=start=0:end=8,setpts=PTS-STARTPTS[a]; \
                   [0:v]trim=start=14:end=22,setpts=PTS-STARTPTS[b]; \
                   [0:v]trim=start=22:end=30,setpts=PTS-STARTPTS[c]; \
                   [a][b][c]concat=n=3:v=1[v]" -map "[v]" \
  $X264 -crf 23 -g 25 -an "$MAT/clean_edited.mp4"

echo "== 长片: 1 小时 320x240, 用于处理速度测试"
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc2=size=320x240:rate=${FPS}:duration=3600" \
  -vf "noise=alls=8:allf=t,format=yuv420p" \
  -c:v libx264 -preset ultrafast -crf 32 -tune stillimage -g 25 -an "$MAT/long_1h.mp4"

# 真值标注(时间单位秒)
cat > "$MAT/ground_truth.json" <<EOF
{
  "orig.mp4":                [],
  "clean_edited.mp4":        [],
  "tamper_deletion.mp4":     [{"type": "frame_deletion",  "start": 9.84, "end": 10.16, "note": "删除 t=10~12s, 接缝在 10s"}],
  "tamper_insertion.mp4":    [{"type": "frame_insertion", "start": 14.84, "end": 16.66, "note": "15s 处插入异源 1.5s"}],
  "tamper_splice.mp4":       [{"type": "splice",          "start": 11.84, "end": 12.16, "note": "A->B 接缝 12s; B->A 接缝 24s"},
                              {"type": "splice",          "start": 23.84, "end": 24.16, "note": ""}],
  "tamper_splice_copy.mp4":  [{"type": "splice",          "start": 11.84, "end": 12.16, "note": "copy 拼接, 双接缝"},
                              {"type": "splice",          "start": 23.84, "end": 24.16, "note": ""}],
  "tamper_local_replace.mp4":[{"type": "local_replace",   "start": 8.0,  "end": 20.0,  "note": "覆盖区 x[0.1875,0.4375] y[0.2222,0.5556]"}],
  "tamper_freeze.mp4":       [{"type": "frame_insertion", "start": 17.96, "end": 18.56, "note": "449 帧冻结重复 12 次, 输出帧 449~461"}]
}
EOF
echo "完成。素材位于 $MAT"; ls -lh "$MAT"
