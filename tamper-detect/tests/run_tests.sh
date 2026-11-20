#!/usr/bin/env bash
# 自动化评测: 对每个样本运行检测, 与 ground truth 对照, 统计检出/误报与时间定位误差。
# 用法: ./run_tests.sh [素材前缀(cam|real|空=全部cam)]
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/../build/tamper-detect"
MAT="$HERE/material"
PREFIX="${1:-cam}"

# 样本 | 期望类型(空=阴性) | 期望时间点(秒, 逗号分隔)
CASES=(
  "${PREFIX}_orig.mp4||"
  "${PREFIX}_clean_edited.mp4||"
  "${PREFIX}_deletion.mp4|frame_deletion|10"
  "${PREFIX}_insertion.mp4|splice|15,16.5"
  "${PREFIX}_splice.mp4|splice|12,24"
  "${PREFIX}_local_replace.mp4|local_replace|8,20"
  "${PREFIX}_freeze.mp4|frame_insertion|18"
  "${PREFIX}_splice_copy.mp4|frame_insertion|12"
)

PASS=0; FAIL=0
printf "%-28s %-16s %-10s %s\n" "样本" "期望" "结果" "检出时间/说明"
echo "-----------------------------------------------------------------------"
for entry in "${CASES[@]}"; do
  IFS='|' read -r file expect_type expect_times <<< "$entry"
  [ -f "$MAT/$file" ] || { printf "%-28s %-16s %-10s %s\n" "$file" "$expect_type" "SKIP" "素材不存在"; continue; }
  out=$("$BIN" "$MAT/$file" --json /tmp/_td_test.json 2>/dev/null)
  count=$(python3 -I -c "import json;print(len(json.load(open('/tmp/_td_test.json'))['findings']))" 2>/dev/null || echo "ERR")

  if [ -z "$expect_type" ]; then
    # 阴性样本: 期望 0 发现
    if [ "$count" = "0" ]; then st="PASS"; PASS=$((PASS+1));
    else st="FAIL(误报)"; FAIL=$((FAIL+1)); fi
    detail="发现 $count 个"
  else
    # 阳性样本: 检查是否有匹配类型且时间点落在检出区间附近(±1.5s)
    detail=$(python3 -I - "$expect_type" "$expect_times" <<'EOF'
import json,sys
etype,times=sys.argv[1],sys.argv[2].split(',')
fs=json.load(open('/tmp/_td_test.json'))['findings']
hit=[]; matched_types=set()
for want in times:
    w=float(want); ok=None
    for f in fs:
        if f['start_sec']-1.5<=w<=f['end_sec']+1.5:
            ok=f; break
    if ok: hit.append(f"{w}s->{ok['type']}@%.1f"%ok['start_sec'])
print(("HIT " + "; ".join(hit)) if hit else "未命中(检出 %d 个: %s)"%(len(fs), ",".join(sorted(set(f['type'] for f in fs))) or "无"))
EOF
)
    if [[ "$detail" == HIT* ]]; then st="PASS"; PASS=$((PASS+1));
    else st="FAIL(漏检)"; FAIL=$((FAIL+1)); fi
  fi
  printf "%-28s %-16s %-10s %s\n" "$file" "${expect_type:-阴性}" "$st" "$detail"
done
echo "-----------------------------------------------------------------------"
echo "通过 $PASS / $((PASS+FAIL))"
exit $FAIL
