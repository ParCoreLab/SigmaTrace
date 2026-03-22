#!/usr/bin/env python3

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import networkx as nx
import numpy as np
import seaborn as sns
from adjustText import adjust_text

_EDGE_KEYS = ("bytes_sent", "bytes_recv", "send_count", "recv_count")
_NIC_GROUP = "__nic__"


def load_graph(path: Path) -> dict:
    with open(path) as f:
        return json.load(f)


def build_nx_graph(data: dict) -> nx.DiGraph:
    G = nx.DiGraph()

    nodes_by_id = {}
    for n in data["nodes"]:
        nodes_by_id[n["id"]] = n
        G.add_node(
            n["id"],
            hostname=n["hostname"],
            qpn=n.get("qpn", 0),
            endpoint_id=n.get("endpoint_id", n.get("qpn", 0)),
            gpu_id=n.get("gpu_id", -1),
            node_type=n.get("type", "cpu"),
            nic_id=n.get("nic_id", -1),
            layer=n["layer"],
            rank=n.get("rank", -1),
            process_id=n.get("process_id", 0),
            is_nic=False,
        )

    for e in data["edges"]:
        src = e["src"]
        dst = e.get("dst")

        if not dst and "dst_qpn" in e and e["dst_qpn"] != 0:
            dst_qpn = e["dst_qpn"]
            for nid, ndata in nodes_by_id.items():
                if ndata.get("qpn") == dst_qpn:
                    dst = nid
                    break

        if not dst:
            node = G.nodes.get(src)
            if node is not None:
                node["unresolved_bytes_sent"] = node.get("unresolved_bytes_sent", 0) + e.get("bytes_sent", 0)
                node["unresolved_bytes_recv"] = node.get("unresolved_bytes_recv", 0) + e.get("bytes_recv", 0)
            continue

        G.add_edge(
            src, dst,
            bytes_sent=e.get("bytes_sent", 0),
            bytes_recv=e.get("bytes_recv", 0),
            send_count=e.get("send_count", 0),
            recv_count=e.get("recv_count", 0),
        )

    return G


def insert_nic_routing(G: nx.DiGraph) -> nx.DiGraph:
    """Route cross-host edges through NIC intermediary nodes.

    For each cross-host edge (u → v), replace with:
        u → src_nic → dst_nic → v
    Same-host edges are kept as-is.
    """
    routed = G.copy()

    # Collect NIC node templates from endpoint nodes
    nic_attrs = {}  # (hostname, nic_id) -> attrs dict

    for n, d in G.nodes(data=True):
        nic_id = d.get("nic_id", -1)
        if nic_id < 0:
            continue
        key = (d["hostname"], nic_id)
        if key not in nic_attrs:
            nic_attrs[key] = {
                "hostname": d["hostname"],
                "nic_id": nic_id,
                "is_nic": True,
                "layer": d.get("layer", "ibv"),
                "gpu_id": -1,
                "rank": -1,
                "process_id": 0,
            }

    cross_host_edges = []
    for u, v, d in G.edges(data=True):
        if G.nodes[u]["hostname"] != G.nodes[v]["hostname"]:
            cross_host_edges.append((u, v, d))

    for u, v, d in cross_host_edges:
        routed.remove_edge(u, v)

        u_data = G.nodes[u]
        v_data = G.nodes[v]

        src_nic_id = u_data.get("nic_id", -1)
        dst_nic_id = v_data.get("nic_id", -1)

        src_nic_key = (u_data["hostname"], src_nic_id)
        dst_nic_key = (v_data["hostname"], dst_nic_id)

        src_nic_node = f"{u_data['hostname']}_nic{src_nic_id}"
        dst_nic_node = f"{v_data['hostname']}_nic{dst_nic_id}"

        # Create NIC nodes if needed
        for nic_node, nic_key in [(src_nic_node, src_nic_key), (dst_nic_node, dst_nic_key)]:
            if nic_node not in routed:
                attrs = nic_attrs.get(nic_key, {
                    "hostname": nic_key[0], "nic_id": nic_key[1],
                    "is_nic": True, "layer": "ibv", "gpu_id": -1,
                    "rank": -1, "process_id": 0,
                })
                routed.add_node(nic_node, **attrs)

        # Add the three-hop path, aggregating if edges already exist
        for eu, ev in [(u, src_nic_node), (src_nic_node, dst_nic_node), (dst_nic_node, v)]:
            if routed.has_edge(eu, ev):
                existing = routed[eu][ev]
                for k in _EDGE_KEYS:
                    existing[k] = existing.get(k, 0) + d.get(k, 0)
            else:
                routed.add_edge(eu, ev, **{k: d.get(k, 0) for k in _EDGE_KEYS})

    return routed


def filter_by_layer(G: nx.DiGraph, layer: str) -> nx.DiGraph:
    if layer == "all":
        return G
    nodes = [n for n, d in G.nodes(data=True) if d["layer"] == layer]
    return G.subgraph(nodes).copy()


def prune_isolated(G: nx.DiGraph) -> nx.DiGraph:
    isolated = [n for n in G.nodes() if G.degree(n) == 0]
    if isolated:
        G = G.copy()
        G.remove_nodes_from(isolated)
    return G


def aggregate(G: nx.DiGraph, level: str) -> nx.DiGraph:
    if level == "qp":
        return G

    def group_key(n, d):
        # NIC nodes always stay at NIC granularity
        if d.get("is_nic"):
            return n if n.startswith(d["hostname"]) else f"{d['hostname']}_nic{d.get('nic_id', -1)}"

        hostname = d["hostname"]
        if level == "node":
            return hostname
        elif level == "gpu":
            gpu_id = d.get("gpu_id", -1)
            return f"{hostname}_gpu{gpu_id}" if gpu_id >= 0 else f"{hostname}_cpu"
        elif level == "nic":
            nic_id = d.get("nic_id", -1)
            return f"{hostname}_nic{nic_id}" if nic_id >= 0 else f"{hostname}_nic?"
        elif level == "rank":
            rank = d.get("rank", -1)
            return f"{hostname}_rank{rank}" if rank >= 0 else f"{hostname}_pid{d.get('process_id', '?')}"
        return hostname

    node_map = {}
    group_attrs = {}
    for n, d in G.nodes(data=True):
        key = group_key(n, d)
        node_map[n] = key
        if key not in group_attrs:
            group_attrs[key] = {
                "hostname": d["hostname"],
                "level": level,
                "is_nic": d.get("is_nic", False),
                "nic_id": d.get("nic_id", -1),
            }

    agg = nx.DiGraph()
    for key, attrs in group_attrs.items():
        agg.add_node(key, **attrs)

    edge_data = defaultdict(lambda: {k: 0 for k in _EDGE_KEYS})
    for u, v, d in G.edges(data=True):
        sk, dk = node_map[u], node_map[v]
        if sk == dk:
            continue
        edge = edge_data[(sk, dk)]
        for k in _EDGE_KEYS:
            edge[k] += d.get(k, 0)

    for (u, v), d in edge_data.items():
        agg.add_edge(u, v, **d)

    return agg


def get_metric(edge_data: dict, metric: str) -> float:
    if metric == "total":
        return edge_data.get("bytes_sent", 0) + edge_data.get("bytes_recv", 0)
    return edge_data.get(metric, 0)


def format_bytes(b: float) -> str:
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if abs(b) < 1024:
            return f"{b:.1f}{unit}"
        b /= 1024
    return f"{b:.1f}PB"


def short_label(node_id: str) -> str:
    """Strip hostname prefix for compact labels: 'host_qp3' -> 'qp3'."""
    return node_id.split("_", 1)[-1] if "_" in node_id else node_id


def _host_palette(G: nx.DiGraph):
    """Assign a distinct colour per hostname."""
    hostnames = sorted({d.get("hostname", "") for _, d in G.nodes(data=True)})
    cmap = plt.cm.Set2 if len(hostnames) <= 8 else plt.cm.tab20
    return {h: cmap(i / max(len(hostnames), 1)) for i, h in enumerate(hostnames)}


def _edge_color_by_source(G: nx.DiGraph, u, host_colors: dict, default=(0.5, 0.5, 0.5, 0.7)):
    """Determine edge color from source node's hostname."""
    hostname = G.nodes[u].get("hostname", "")
    return host_colors.get(hostname, default)


def _group_nodes_by_host(G: nx.DiGraph):
    """Return {hostname: [node_ids...]} sorted with NIC nodes last within each host."""
    groups = defaultdict(list)
    for n, d in G.nodes(data=True):
        groups[d.get("hostname", "")].append(n)
    for h in groups:
        # Sort: non-NIC nodes first (alphabetically), then NIC nodes
        groups[h].sort(key=lambda n: (G.nodes[n].get("is_nic", False), _natural_sort_key(n)))
    return dict(sorted(groups.items()))


def _grouped_layout(G: nx.DiGraph, host_groups: dict, padding: float = 0.3):
    """Position nodes in a grid of host boxes with generous spacing."""
    pos = {}
    hosts = list(host_groups.keys())
    n_hosts = len(hosts)

    h_cols = max(1, int(np.ceil(np.sqrt(n_hosts))))

    max_per_host = max(len(v) for v in host_groups.values()) if host_groups else 1

    inner_cols = max(1, int(np.ceil(np.sqrt(max_per_host))))
    inner_rows = max(1, int(np.ceil(max_per_host / inner_cols)))

    node_spacing = 0.8
    box_w = inner_cols * node_spacing + 0.4
    box_h = inner_rows * node_spacing + 0.4

    gap_x = box_w * 0.8
    gap_y = box_h * 0.6

    boxes = {}

    for idx, host in enumerate(hosts):
        col = idx % h_cols
        row = idx // h_cols
        cx = col * (box_w + gap_x)
        cy = -row * (box_h + gap_y)
        boxes[host] = (cx, cy, box_w, box_h)

        nodes = host_groups[host]
        for i, node in enumerate(nodes):
            ic = i % inner_cols
            ir = i // inner_cols
            x = cx + (ic - (inner_cols - 1) / 2) * node_spacing
            y = cy - (ir - (inner_rows - 1) / 2) * node_spacing
            pos[node] = (x, y)

    return pos, boxes


def _draw_host_boxes(ax, boxes: dict, host_colors: dict, padding: float = 0.15):
    """Draw rounded rectangles around each host group."""
    for host, (cx, cy, w, h) in boxes.items():
        color = host_colors.get(host, (0.8, 0.8, 0.8))
        rect = mpatches.FancyBboxPatch(
            (cx - w / 2 - padding, cy - h / 2 - padding),
            w + 2 * padding, h + 2 * padding,
            boxstyle="round,pad=0.05",
            facecolor=(*color[:3], 0.12),
            edgecolor=(*color[:3], 0.6),
            linewidth=1.5,
            zorder=0,
        )
        ax.add_patch(rect)
        ax.text(cx, cy + h / 2 + padding + 0.06, host,
                ha="center", va="bottom", fontsize=9, fontweight="bold",
                color=(*color[:3], 0.9))


def draw_graph(G: nx.DiGraph, metric: str, title: str, output: Path | None,
               grouped: bool = False):
    n_nodes = G.number_of_nodes()
    if n_nodes == 0:
        print("No nodes to display.")
        return

    host_colors = _host_palette(G)

    # Split nodes into regular and NIC for different shapes
    regular_nodes = [n for n in G.nodes() if not G.nodes[n].get("is_nic", False)]
    nic_nodes = [n for n in G.nodes() if G.nodes[n].get("is_nic", False)]

    if grouped:
        host_groups = _group_nodes_by_host(G)
        pos, boxes = _grouped_layout(G, host_groups)
    else:
        pos = None
        boxes = None

    if pos is None:
        scale = max(1.0, np.sqrt(n_nodes) * 0.4)
        if n_nodes <= 64:
            pos = nx.kamada_kawai_layout(G, scale=scale)
        else:
            pos = nx.spring_layout(G, seed=42, k=1.2 / max(np.sqrt(n_nodes), 1),
                                   iterations=80, scale=scale)

    node_size = max(200, 600 - n_nodes * 8)
    font_size = max(5, 8 - n_nodes // 20)
    if grouped:
        xs = [p[0] for p in pos.values()]
        ys = [p[1] for p in pos.values()]
        span_x = max(xs) - min(xs) if xs else 1
        span_y = max(ys) - min(ys) if ys else 1
        fig_w = max(8, span_x * 2.5 + 4)
        fig_h = max(6, span_y * 2.5 + 4)
    else:
        fig_side = max(6, min(16, 4 + n_nodes * 0.3))
        fig_w, fig_h = fig_side, fig_side * 0.75
    fig, ax = plt.subplots(figsize=(fig_w, fig_h))

    if boxes:
        _draw_host_boxes(ax, boxes, host_colors)

    # Edge widths proportional to metric
    edge_weights = [get_metric(d, metric) for _, _, d in G.edges(data=True)]
    max_w = max(edge_weights) if edge_weights else 1
    edge_widths = [0.5 + 3.0 * (w / max_w) for w in edge_weights] if max_w > 0 else [1] * len(edge_weights)

    # Edge colors based on source node's host
    edge_colors = [_edge_color_by_source(G, u, host_colors) for u, v in G.edges()]

    # Draw regular nodes (circles)
    if regular_nodes:
        reg_colors = [host_colors[G.nodes[n].get("hostname", "")] for n in regular_nodes]
        nx.draw_networkx_nodes(G, pos, ax=ax, nodelist=regular_nodes, node_color=reg_colors,
                               node_size=node_size, edgecolors="black", linewidths=0.4,
                               node_shape="o")

    # Draw NIC nodes (squares, slightly smaller, darker edge)
    if nic_nodes:
        nic_node_colors = [host_colors[G.nodes[n].get("hostname", "")] for n in nic_nodes]
        nx.draw_networkx_nodes(G, pos, ax=ax, nodelist=nic_nodes, node_color=nic_node_colors,
                               node_size=int(node_size * 0.8), edgecolors="black", linewidths=0.8,
                               node_shape="s")

    labels = {n: short_label(n) for n in G.nodes()}
    nx.draw_networkx_labels(G, pos, labels=labels, ax=ax, font_size=font_size, font_weight="bold")

    nx.draw_networkx_edges(G, pos, ax=ax, width=edge_widths, alpha=0.6,
                           edge_color=edge_colors, arrows=True, arrowsize=12,
                           arrowstyle="-|>", connectionstyle="arc3,rad=0.08",
                           min_source_margin=8, min_target_margin=8)

    if n_nodes <= 32:
        label_fs = max(4, font_size - 1)
        texts = []
        for u, v, d in G.edges(data=True):
            val = get_metric(d, metric)
            if val <= 0:
                continue
            # Place initial position at edge midpoint, offset along the arc
            x0, y0 = pos[u]
            x1, y1 = pos[v]
            mx, my = (x0 + x1) / 2, (y0 + y1) / 2
            # Offset perpendicular to edge to avoid sitting on the arrow
            dx, dy = x1 - x0, y1 - y0
            length = max((dx**2 + dy**2) ** 0.5, 1e-6)
            nx_p, ny_p = -dy / length, dx / length
            offset = 0.04 * max(
                max(p[0] for p in pos.values()) - min(p[0] for p in pos.values()),
                max(p[1] for p in pos.values()) - min(p[1] for p in pos.values()),
                1,
            )
            t = ax.text(mx + nx_p * offset, my + ny_p * offset, format_bytes(val),
                        fontsize=label_fs, ha="center", va="center", alpha=0.85,
                        bbox=dict(boxstyle="round,pad=0.15", fc="white", ec="none", alpha=0.7))
            texts.append(t)
        if texts:
            adjust_text(texts, ax=ax, arrowprops=dict(arrowstyle="-", color="gray", alpha=0.3, lw=0.5),
                        expand=(1.2, 1.4), force_text=(0.8, 1.0), force_static=(0.5, 0.5),
                        ensure_inside_axes=True)

    # Legend
    if not boxes:
        for hostname, color in host_colors.items():
            ax.scatter([], [], c=[color], label=hostname, s=60, edgecolors="black", linewidths=0.3)
    if nic_nodes:
        ax.scatter([], [], c=["gray"], label="NIC", s=50, edgecolors="black",
                   linewidths=0.6, marker="s")
    if not boxes or nic_nodes:
        ax.legend(loc="upper left", fontsize=max(6, font_size), framealpha=0.8,
                  borderpad=0.3, handletextpad=0.3, labelspacing=0.2)

    ax.set_title(title, fontsize=11, pad=4)
    ax.axis("off")
    ax.margins(0.08)
    fig.tight_layout(pad=0.5)

    if output:
        fig.savefig(output, dpi=150, bbox_inches="tight")
        print(f"Saved graph to: {output}")
    else:
        plt.show()
    plt.close(fig)


def _natural_sort_key(s: str):
    """Sort key that handles embedded numbers naturally: rank2 < rank10."""
    import re
    return [int(p) if p.isdigit() else p.lower() for p in re.split(r'(\d+)', s)]


def draw_matrix(G: nx.DiGraph, metric: str, title: str, output: Path | None):
    nodes = sorted(G.nodes(), key=_natural_sort_key)

    n = len(nodes)
    if n == 0:
        print("No nodes for matrix.")
        return

    idx = {node: i for i, node in enumerate(nodes)}
    matrix = np.zeros((n, n))
    for u, v, d in G.edges(data=True):
        matrix[idx[u]][idx[v]] = get_metric(d, metric)

    cell = max(0.35, min(0.6, 8.0 / n))
    fig_w = max(5, n * cell + 2)
    fig_h = max(4, n * cell + 1.5)
    fig, ax = plt.subplots(figsize=(fig_w, fig_h))

    labels = [short_label(nd) for nd in nodes]

    # Per-cell annotations with dynamic units (e.g. "1.2GB", "340KB")
    if n <= 24:
        annot_strs = np.empty_like(matrix, dtype=object)
        for i in range(n):
            for j in range(n):
                v = matrix[i, j]
                annot_strs[i, j] = "" if v == 0 else format_bytes(v)
        annot = annot_strs
    else:
        annot = False

    annot_fontsize = max(5, 8 - n // 4)
    sns.heatmap(
        matrix, ax=ax, xticklabels=labels, yticklabels=labels,
        cmap="YlOrRd", linewidths=0.3, annot=annot,
        fmt="", square=True, annot_kws={"fontsize": annot_fontsize},
        cbar_kws={"label": f"{metric} (bytes)", "shrink": 0.8},
    )

    ax.set_title(title, fontsize=11, pad=6)
    ax.set_xlabel("Destination", fontsize=9)
    ax.set_ylabel("Source", fontsize=9)
    ax.tick_params(labelsize=max(5, 9 - n // 10))
    fig.tight_layout(pad=0.5)

    if output:
        matrix_path = output.with_suffix(".matrix.png") if output.suffix else Path(str(output) + ".matrix.png")
        fig.savefig(matrix_path, dpi=150, bbox_inches="tight")
        print(f"Saved matrix to: {matrix_path}")
    else:
        plt.show()
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Visualize Sigma Profiler communication graphs",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("input_file", type=Path, help="Path to commgraph.json")
    parser.add_argument("--level", choices=["qp", "gpu", "nic", "node", "rank"], default="qp",
                        help="Aggregation level (default: qp)")
    parser.add_argument("--layer", choices=["all", "ibv", "ucx", "libfabric"], default="all",
                        help="Filter by communication layer (default: all)")
    parser.add_argument("--metric", choices=["bytes_sent", "bytes_recv", "total"], default="total",
                        help="Edge weight metric (default: total)")
    parser.add_argument("--output", "-o", type=Path, default=None,
                        help="Output image path (shows interactively if omitted)")
    parser.add_argument("--matrix-only", action="store_true", help="Only show communication matrix")
    parser.add_argument("--graph-only", action="store_true", help="Only show network graph")
    parser.add_argument("--include-isolated", action="store_true",
                        help="Include isolated nodes (no edges) in visualization")
    parser.add_argument("--no-group", action="store_true",
                        help="Disable grouped host-box layout in the graph")
    parser.add_argument("--show-nics", action="store_true",
                        help="Insert NIC nodes as intermediaries for cross-host edges")

    args = parser.parse_args()

    if not args.input_file.exists():
        print(f"Error: {args.input_file} not found", file=sys.stderr)
        sys.exit(1)

    data = load_graph(args.input_file)
    G_base = build_nx_graph(data)
    G_base = filter_by_layer(G_base, args.layer)

    title = f"Communication Graph ({args.level} level, {args.layer} layer, {args.metric})"

    # Graph view: optionally insert NIC routing
    if not args.matrix_only:
        G_graph = G_base
        if args.show_nics:
            G_graph = insert_nic_routing(G_graph)
        G_graph = aggregate(G_graph, args.level)
        if not args.include_isolated:
            G_graph = prune_isolated(G_graph)
        if G_graph.number_of_nodes() == 0:
            print("No data after filtering. Check --layer and --level options.", file=sys.stderr)
            sys.exit(1)
        use_grouped = args.level in ("rank", "gpu", "nic", "qp") and not args.no_group
        draw_graph(G_graph, args.metric, title, args.output, grouped=use_grouped)

    # Matrix view: always without NIC routing
    if not args.graph_only:
        G_matrix = aggregate(G_base, args.level)
        if not args.include_isolated:
            G_matrix = prune_isolated(G_matrix)
        if G_matrix.number_of_nodes() == 0:
            print("No data after filtering. Check --layer and --level options.", file=sys.stderr)
            sys.exit(1)
        draw_matrix(G_matrix, args.metric, title, args.output)


if __name__ == "__main__":
    main()
