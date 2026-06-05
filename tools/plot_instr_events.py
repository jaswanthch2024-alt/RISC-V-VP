#!/usr/bin/env python3
"""
Instruction-Indexed Pipeline Event Visualizer
==============================================
Instead of showing events per cycle (like the heatmap), this shows:
  - At which COMMITTED INSTRUCTION NUMBER did each stall / flush occur?
  - What PC was in the Execute stage when it happened?

This makes it easy to spot:
  - Clusters of back-to-back stalls (RAW hazards)
  - Isolated flushes (branch mispredictions / control hazards)
  - Quiet regions where the pipeline runs cleanly

Usage:
    python plot_instr_events.py [pipeline_trace.csv]
                               [--max-instr N]    # cap instruction count (default: all)
                               [--out FILE]       # output PNG (default: instr_events.png)
                               [--no-density]     # skip density subplot

Outputs a 3-panel figure:
  Panel 1  – Event raster: one thin row per event type, x = instruction index
  Panel 2  – PC address at stall/flush events (scatter coloured by type)
  Panel 3  – Stall/flush density histogram (events per 500-instr bucket)
"""

import argparse
import csv
import sys
import os

try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import matplotlib.ticker as mticker
    import numpy as np
except ImportError:
    print("ERROR: matplotlib and numpy are required.")
    print("  pip install matplotlib numpy")
    sys.exit(1)

# ── colour palette ────────────────────────────────────────────────────────────
CLR_STALL  = '#f39c12'   # orange
CLR_FLUSH  = '#e74c3c'   # red
CLR_ACTIVE = '#2ecc71'   # green (used in density)

# ── CSV column names ──────────────────────────────────────────────────────────
COL_CYCLE        = 'cycle'
COL_PC_COMMIT    = 'pc_commit'
COL_PC_EX        = 'pc_ex'
COL_PC_ISSUE     = 'pc_issue'
COL_COMMIT_VALID = 'commit_valid'
COL_STALL_ISSUE  = 'stall_issue'
COL_FLUSH        = 'flush'


def parse_csv(path, max_instr):
    """
    Walk the CSV row by row.  Every time an instruction commits (ex_valid=1, no
    flush that cycle) we increment a committed-instruction counter.  Stall and
    flush events are recorded against the *current* committed-instruction index
    so you can see exactly where in the instruction stream they cluster.

    Returns
    -------
    stall_events : list of (instr_idx, pc_ex_hex)
    flush_events : list of (instr_idx, pc_ex_hex)
    total_committed : int
    total_cycles    : int
    """
    stall_events = []
    flush_events = []
    instr_idx    = 0          # committed instruction counter
    total_cycles = 0

    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            total_cycles += 1
            flush        = int(row[COL_FLUSH])
            stall_iss    = int(row[COL_STALL_ISSUE])
            commit_valid = int(row[COL_COMMIT_VALID])
            pc_ex        = row[COL_PC_EX]

            # ── record stall event ────────────────────────────────────────────
            if stall_iss:
                stall_events.append((instr_idx, pc_ex))

            # ── record flush event ────────────────────────────────────────────
            if flush:
                flush_events.append((instr_idx, pc_ex))

            # ── count committed instruction ───────────────────────────────────
            # An instruction commits when commit_valid=1 (ROB head retires).
            if commit_valid:
                instr_idx += 1

            if max_instr and instr_idx >= max_instr:
                break

    return stall_events, flush_events, instr_idx, total_cycles


def print_summary(stall_events, flush_events, total_committed, total_cycles):
    n_stall  = len(stall_events)
    n_flush  = len(flush_events)
    n_active = total_committed
    total_ev = total_cycles

    print(f"\n{'='*55}")
    print(f"  Pipeline Event Summary")
    print(f"{'='*55}")
    print(f"  Cycles processed    : {total_cycles:>10,}")
    print(f"  Instructions committed: {total_committed:>8,}")
    print(f"  Stall cycles        : {n_stall:>10,}  ({100*n_stall/max(total_cycles,1):.1f}%)")
    print(f"  Flush cycles        : {n_flush:>10,}  ({100*n_flush/max(total_cycles,1):.1f}%)")
    print(f"  Active (commit) cyc : {n_active:>10,}  ({100*n_active/max(total_cycles,1):.1f}%)")
    if total_committed > 0:
        cpi = total_cycles / total_committed
        print(f"  CPI (cycles/instr)  : {cpi:>10.3f}")
    print(f"{'='*55}\n")

    # ── first few stall / flush locations ─────────────────────────────────────
    if stall_events:
        print("  First 10 stall events (instr#, pc_ex):")
        for idx, pc in stall_events[:10]:
            print(f"    instr #{idx:>6,}  pc={pc}")
    if flush_events:
        print("\n  First 10 flush (branch misp.) events (instr#, pc_ex):")
        for idx, pc in flush_events[:10]:
            print(f"    instr #{idx:>6,}  pc={pc}")
    print()


def _hex_to_int(h):
    try:
        return int(h, 16)
    except (ValueError, TypeError):
        return 0


def plot_events(stall_events, flush_events, total_committed, out_file, no_density):
    """Three-panel figure."""

    n_panels = 2 if no_density else 3
    fig, axes = plt.subplots(n_panels, 1,
                             figsize=(14, 4 * n_panels),
                             constrained_layout=True)
    if n_panels == 1:
        axes = [axes]

    s_idx = np.array([e[0] for e in stall_events], dtype=np.float32)
    f_idx = np.array([e[0] for e in flush_events], dtype=np.float32)
    s_pc  = np.array([_hex_to_int(e[1]) for e in stall_events], dtype=np.uint64)
    f_pc  = np.array([_hex_to_int(e[1]) for e in flush_events], dtype=np.uint64)

    x_max = total_committed or 1

    # ── Panel 1 : Event Raster ────────────────────────────────────────────────
    ax1 = axes[0]
    # Draw a thin horizontal tick at each event
    if len(s_idx):
        ax1.vlines(s_idx, 0.55, 0.95, colors=CLR_STALL,  linewidth=0.4, alpha=0.7, label='Stall')
    if len(f_idx):
        ax1.vlines(f_idx, 0.05, 0.45, colors=CLR_FLUSH,  linewidth=0.6, alpha=0.9, label='Flush / Branch Misp.')
    ax1.set_xlim(0, x_max)
    ax1.set_ylim(0, 1)
    ax1.set_yticks([0.25, 0.75])
    ax1.set_yticklabels(['Flush', 'Stall'], fontsize=9)
    ax1.set_xlabel('Committed Instruction #', fontsize=9)
    ax1.set_title('Pipeline Events vs Instruction Index', fontsize=10, fontweight='bold')
    ax1.legend(loc='upper right', fontsize=8)
    ax1.grid(axis='x', linestyle=':', alpha=0.4)
    ax1.xaxis.set_major_formatter(mticker.FuncFormatter(lambda x, _: f'{int(x):,}'))

    # ── Panel 2 : PC scatter ──────────────────────────────────────────────────
    ax2 = axes[1]
    if len(s_idx):
        ax2.scatter(s_idx, s_pc, s=1.5, c=CLR_STALL, alpha=0.5, label='Stall', rasterized=True)
    if len(f_idx):
        ax2.scatter(f_idx, f_pc, s=4,   c=CLR_FLUSH, alpha=0.8, label='Flush', rasterized=True, zorder=3)
    ax2.set_xlim(0, x_max)
    ax2.set_xlabel('Committed Instruction #', fontsize=9)
    ax2.set_ylabel('PC (Execute stage) at event', fontsize=9)
    ax2.set_title('PC Address at Stall / Flush Events', fontsize=10, fontweight='bold')
    ax2.legend(loc='upper right', fontsize=8)
    ax2.grid(linestyle=':', alpha=0.4)
    ax2.xaxis.set_major_formatter(mticker.FuncFormatter(lambda x, _: f'{int(x):,}'))
    ax2.yaxis.set_major_formatter(mticker.FuncFormatter(lambda x, _: f'0x{int(x):04x}'))

    # ── Panel 3 : Density histogram ───────────────────────────────────────────
    if not no_density:
        ax3 = axes[2]
        bucket = max(1, x_max // 100)   # ~100 buckets across the window
        bins   = np.arange(0, x_max + bucket, bucket)

        if len(s_idx):
            ax3.hist(s_idx, bins=bins, color=CLR_STALL, alpha=0.7, label='Stalls')
        if len(f_idx):
            ax3.hist(f_idx, bins=bins, color=CLR_FLUSH, alpha=0.8, label='Flushes')

        ax3.set_xlim(0, x_max)
        ax3.set_xlabel('Committed Instruction #', fontsize=9)
        ax3.set_ylabel(f'Events per ~{bucket}-instr bucket', fontsize=9)
        ax3.set_title('Stall / Flush Density over Instruction Stream', fontsize=10, fontweight='bold')
        ax3.legend(loc='upper right', fontsize=8)
        ax3.grid(axis='y', linestyle=':', alpha=0.4)
        ax3.xaxis.set_major_formatter(mticker.FuncFormatter(lambda x, _: f'{int(x):,}'))

    plt.savefig(out_file, dpi=150)
    print(f"Saved visualization to {out_file}")


def main():
    parser = argparse.ArgumentParser(
        description='Instruction-indexed stall/flush visualizer for 6-stage pipeline trace')
    parser.add_argument('csv', nargs='?', default='pipeline_trace.csv',
                        help='Pipeline trace CSV (default: pipeline_trace.csv)')
    parser.add_argument('--max-instr', type=int, default=0,
                        help='Stop after N committed instructions (0 = all)')
    parser.add_argument('--out', default='instr_events.png',
                        help='Output PNG (default: instr_events.png)')
    parser.add_argument('--no-density', action='store_true',
                        help='Skip the density histogram panel')
    args = parser.parse_args()

    if not os.path.exists(args.csv):
        print(f"ERROR: {args.csv} not found. Run the simulator first.")
        sys.exit(1)

    print(f"Parsing {args.csv} ...")
    stall_ev, flush_ev, committed, cycles = parse_csv(args.csv, args.max_instr)
    print(f"  → {cycles:,} cycles parsed, {committed:,} instructions committed")

    print_summary(stall_ev, flush_ev, committed, cycles)
    plot_events(stall_ev, flush_ev, committed, args.out, args.no_density)


if __name__ == '__main__':
    main()
