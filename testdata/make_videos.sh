#!/bin/bash
# 合成测试录像: 自绘监控场景(静态背景+匀速运动物体+传感器噪声) + 各种篡改版本
set -euo pipefail
cd "$(dirname "$0")/out"

V="640x360"; R=25; G=50
# 模拟监控摄像头常见参数: 固定 GOP, 无 B 帧
ENC="-c:v libx264 -preset veryfast -crf 21 -g $G -bf 0 -sc_threshold 0 -pix_fmt yuv420p -an"

# 摄像头1: 深灰背景 + 静态参照物 + 两个匀速运动物体, 时域噪声 sigma~10
SCENE1="color=c=0x303030:s=${V}:r=${R}:d=180[bg];\
color=c=0xE0E0E0:s=80x120:r=${R}:d=180[b1];\
color=c=0x909090:s=60x60:r=${R}:d=180[b2];\
[bg]drawbox=x=60:y=40:w=120:h=80:c=0x606060:t=fill,drawbox=x=420:y=220:w=150:h=90:c=0x505050:t=fill[bg2];\
[bg2][b1]overlay=x='mod(120*t,760)-100':y=120[tmp];\
[tmp][b2]overlay=x='580-mod(60*t,700)':y=270,format=yuv420p,noise=alls=20:allf=t+u"

# 摄像头2: 不同背景/布局/运动速度/亮度, 噪声 sigma~4
SCENE2="color=c=0x3a3a3a:s=${V}:r=${R}:d=180[bg];\
color=c=0xD0D0D0:s=70x100:r=${R}:d=180[b1];\
[bg]drawbox=x=200:y=60:w=140:h=100:c=0x555555:t=fill,drawbox=x=480:y=40:w=100:h=120:c=0x484848:t=fill[bg2];\
[bg2][b1]overlay=x='620-mod(90*t,740)':y=180,format=yuv420p,eq=brightness=0.03,noise=alls=8:allf=t+u"

echo "=== [1/9] orig.mp4 原始监控录像 (180s) ==="
[ -f orig.mp4 ] || ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "$SCENE1" -t 180 $ENC orig.mp4

echo "=== [2/9] cam2.mp4 另一台摄像头 ==="
[ -f cam2.mp4 ] || ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "$SCENE2" -t 180 $ENC cam2.mp4

echo "=== [3/9] del_copy.mp4 删掉 60s-62s, 流拷贝拼接 (不重编码) ==="
if [ ! -f del_copy.mp4 ]; then
  ffmpeg -hide_banner -loglevel error -y -ss 0 -to 60 -i orig.mp4 -c copy p1.mp4
  ffmpeg -hide_banner -loglevel error -y -ss 62 -i orig.mp4 -c copy p2.mp4
  printf "file 'p1.mp4'\nfile 'p2.mp4'\n" > cc.txt
  ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i cc.txt -c copy del_copy.mp4
  rm -f p1.mp4 p2.mp4 cc.txt
fi

echo "=== [4/9] del_reenc.mp4 删掉 60s-63s, 整段重编码 ==="
[ -f del_reenc.mp4 ] || ffmpeg -hide_banner -loglevel error -y -i orig.mp4 \
  -vf "select='not(between(t,60,63))',setpts=N/FRAME_RATE/TB" \
  $ENC del_reenc.mp4

echo "=== [5/9] insert.mp4 在 60s 处插入 cam2 的 3 秒 ==="
if [ ! -f insert.mp4 ]; then
  ffmpeg -hide_banner -loglevel error -y -ss 0 -to 60 -i orig.mp4 -c copy q1.mp4
  ffmpeg -hide_banner -loglevel error -y -ss 10 -t 3 -i cam2.mp4 $ENC q2.mp4
  ffmpeg -hide_banner -loglevel error -y -ss 60 -i orig.mp4 -c copy q3.mp4
  printf "file 'q1.mp4'\nfile 'q2.mp4'\nfile 'q3.mp4'\n" > cc.txt
  ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i cc.txt -c copy insert.mp4
  rm -f q1.mp4 q2.mp4 q3.mp4 cc.txt
fi

echo "=== [6/9] splice.mp4 60s 之后整段换成 cam2 的画面 ==="
if [ ! -f splice.mp4 ]; then
  ffmpeg -hide_banner -loglevel error -y -ss 0 -to 60 -i orig.mp4 -c copy s1.mp4
  ffmpeg -hide_banner -loglevel error -y -ss 60 -i cam2.mp4 $ENC s2.mp4
  printf "file 's1.mp4'\nfile 's2.mp4'\n" > cc.txt
  ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i cc.txt -c copy splice.mp4
  rm -f s1.mp4 s2.mp4 cc.txt
fi

echo "=== [7/9] region.mp4 60s-75s 画面局部被 cam2 的一块移动区域覆盖 ==="
[ -f region.mp4 ] || ffmpeg -hide_banner -loglevel error -y -i orig.mp4 -i cam2.mp4 \
  -filter_complex "[1:v]crop=200:120:220:100,setsar=1[p];[0:v][p]overlay=x='120+80*sin(2*PI*t/5)':y=160:enable='between(t,60,75)'[v]" \
  -map "[v]" $ENC region.mp4

echo "=== [8/9] dup.mp4 60s-62s 画面被冻结 (重复同一帧) ==="
if [ ! -f dup.mp4 ]; then
  ffmpeg -hide_banner -loglevel error -y -i orig.mp4 \
    -vf "select='lt(n,1500)',setpts=N/FRAME_RATE/TB" $ENC d1.mp4
  ffmpeg -hide_banner -loglevel error -y -i orig.mp4 \
    -vf "select='eq(n,1500)',loop=loop=49:size=1,setpts=N/FRAME_RATE/TB" $ENC d2.mp4
  ffmpeg -hide_banner -loglevel error -y -i orig.mp4 \
    -vf "select='gte(n,1550)',setpts=N/FRAME_RATE/TB" $ENC d3.mp4
  printf "file 'd1.mp4'\nfile 'd2.mp4'\nfile 'd3.mp4'\n" > cc.txt
  ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i cc.txt -c copy dup.mp4
  rm -f d1.mp4 d2.mp4 d3.mp4 cc.txt
fi

echo "=== [9/9] double.mp4 对照组: 仅整体重编码, 无篡改 ==="
[ -f double.mp4 ] || ffmpeg -hide_banner -loglevel error -y -i orig.mp4 $ENC double.mp4

echo "=== 1 小时性能测试文件 (orig 循环 20 次, 流拷贝) ==="
if [ ! -f long_1h.mp4 ]; then
  ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$SCENE1" -t 120 \
    -c:v libx264 -preset veryfast -crf 26 -g $G -bf 0 -sc_threshold 0 -pix_fmt yuv420p -an seg.mp4
  for i in $(seq 30); do echo "file 'seg.mp4'"; done > loop.txt
  ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 -i loop.txt -c copy long_1h.mp4
  rm -f loop.txt seg.mp4
fi

cat > ground_truth.json <<'EOF'
{
  "orig.mp4":      [],
  "double.mp4":    [],
  "del_copy.mp4":  [{"type": "deletion",  "t0": 60, "t1": 62}],
  "del_reenc.mp4": [{"type": "deletion",  "t0": 60, "t1": 63}],
  "insert.mp4":    [{"type": "insertion", "t0": 60, "t1": 63}],
  "splice.mp4":    [{"type": "splice",    "t0": 60, "t1": 180}],
  "region.mp4":    [{"type": "region",    "t0": 60, "t1": 75}],
  "dup.mp4":       [{"type": "duplicate", "t0": 60, "t1": 62}]
}
EOF

echo "=== 完成 ==="
ls -lh *.mp4
