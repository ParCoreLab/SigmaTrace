#!/usr/bin/env python3
"""
Verify communication graph byte counts against expected values from main_nvshmem.cu.

Usage:
    python verify_commgraph.py <commgraph.json> [--expected-per-pe BYTES]

Default expected bytes per PE: 65,536,004
    = 32,768 bytes/launch * 2000 launches + 4 bytes (simple_shift)
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description="Verify commgraph byte counts")
    parser.add_argument("input_file", type=Path, help="Path to commgraph.json")
    parser.add_argument(
        "--expected-per-pe",
        type=int,
        default=65_536_004,
        help="Expected total bytes_sent per PE (default: 65536004)",
    )
    args = parser.parse_args()

    with open(args.input_file) as f:
        data = json.load(f)

    nodes_by_id = {n["id"]: n for n in data["nodes"]}

    # Aggregate bytes_sent by source gpu_id
    gpu_traffic = defaultdict(lambda: {"bytes_sent": 0, "send_count": 0, "qps": []})

    for e in data["edges"]:
        src = e["src"]
        src_node = nodes_by_id.get(src)
        if not src_node:
            print(f"WARNING: edge src {src} not in nodes", file=sys.stderr)
            continue

        gpu_id = src_node.get("gpu_id", -1)
        gpu_traffic[gpu_id]["bytes_sent"] += e.get("bytes_sent", 0)
        gpu_traffic[gpu_id]["send_count"] += e.get("send_count", 0)
        gpu_traffic[gpu_id]["qps"].append(
            {
                "qpn": src_node.get("qpn"),
                "bytes_sent": e.get("bytes_sent", 0),
                "send_count": e.get("send_count", 0),
            }
        )

    # Check edge connectivity
    edge_issues = 0
    for e in data["edges"]:
        dst = e.get("dst")
        if not dst or dst not in nodes_by_id:
            dst_qpn = e.get("dst_qpn", "?")
            print(f"DISCONNECTED EDGE: {e['src']} -> dst_qpn={dst_qpn} (not in nodes)")
            edge_issues += 1

    if edge_issues:
        print(f"\n{edge_issues} disconnected edge(s) found!\n")

    # Report per-PE
    expected = args.expected_per_pe
    all_ok = True

    print(f"{'PE (gpu_id)':<15} {'bytes_sent':>15} {'expected':>15} {'diff':>10} {'%err':>10} {'send_count':>12}")
    print("-" * 80)

    for gpu_id in sorted(gpu_traffic.keys()):
        t = gpu_traffic[gpu_id]
        diff = t["bytes_sent"] - expected
        pct = (diff / expected * 100) if expected else 0
        ok = "OK" if diff == 0 else "MISMATCH"
        if diff != 0:
            all_ok = False
        print(
            f"GPU {gpu_id:<10} {t['bytes_sent']:>15,} {expected:>15,} {diff:>+10,} {pct:>+9.4f}% {t['send_count']:>12,}  {ok}"
        )
        for qp in t["qps"]:
            print(f"  QPN {qp['qpn']:<8} {qp['bytes_sent']:>15,} {'':>15} {'':>10} {'':>10} {qp['send_count']:>12,}")

    print("-" * 80)
    total_sent = sum(t["bytes_sent"] for t in gpu_traffic.values())
    total_expected = expected * len(gpu_traffic)
    total_diff = total_sent - total_expected
    print(f"{'TOTAL':<15} {total_sent:>15,} {total_expected:>15,} {total_diff:>+10,}")
    print()

    if edge_issues == 0:
        print(f"Edge connectivity: ALL OK ({len(data['edges'])} edges, all resolved)")
    else:
        print(f"Edge connectivity: {edge_issues} DISCONNECTED")

    if all_ok and edge_issues == 0:
        print("Result: PASS")
        return 0
    else:
        print("Result: FAIL")
        return 1


if __name__ == "__main__":
    sys.exit(main())
