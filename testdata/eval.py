#!/usr/bin/env python3
"""对照 ground_truth.json 评估 vfcheck 的检出效果。
用法: python3 eval.py   (在 testdata/out 目录下运行, 或指定 vfcheck 路径)"""
import json, subprocess, sys, os

VFCHECK = sys.argv[1] if len(sys.argv) > 1 else \
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "../detector/build/vfcheck")
OUT = os.path.dirname(os.path.abspath(__file__)) + "/out"

gt = json.load(open(os.path.join(OUT, "ground_truth.json")))
tp = fn = fp = 0
for f, truth in sorted(gt.items()):
    rpt = "/tmp/vfcheck_eval.json"
    r = subprocess.run([VFCHECK, os.path.join(OUT, f), "--json", rpt],
                       capture_output=True, text=True)
    events = json.load(open(rpt))["events"]
    lines = []
    for t in truth:
        hit = any(e["t0"] <= t["t1"] + 2 and e["t1"] >= t["t0"] - 2 for e in events)
        tp += hit; fn += (not hit)
        lines.append(f"  真值 {t['type']:10s} {t['t0']:6.0f}-{t['t1']:.0f}s -> {'✓ 检出' if hit else '✗ 漏检'}")
    for e in events:
        ok = any(e["t0"] <= t["t1"] + 2 and e["t1"] >= t["t0"] - 2 for t in truth)
        fp += (not ok)
        if not ok:
            lines.append(f"  误报: {e['t0']:.1f}-{e['t1']:.1f}s score={e['score']:.2f}")
    status = "✓" if all('✓' in l or '误报' not in l for l in lines) and not any('误报' in l for l in lines) else "△"
    print(f"{f:16s} {status}")
    for l in lines: print(l)
print(f"\n总计: 检出 {tp}/{tp+fn}, 误报 {fp}")
