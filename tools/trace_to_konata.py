#!/usr/bin/env python3
"""
trace_to_konata.py
==================
Converts RISC-V TLM simulator pipeline_trace.csv to Konata (Onikiri) format.

Usage:
    python trace_to_konata.py pipeline_trace.csv -o konata.log [--max-cycles N]

Pipeline stages (indices 0-4):
  PCG(0) -> FET(1) -> DEC(2) -> ISS(3) -> EXE(4) -> COM (off-pipeline, via queues)

Stall model (from Issue_stage in CPU_P64_6_Cycle.cpp):
  stall_issue=1: PCG/FET/DEC/ISS all hold their instruction.
                 EXE always drains normally. Bubble enters EXE next cycle.

Flush model (from EX_stage + PCGen_stage):
  flush=1: PCG/FET/DEC/ISS instructions are wrong-path -> killed (R fault=1).
           EXE instruction (the branch that caused the flush) completes normally -> COM.

Previous approach (PC comparison) was WRONG:
  Loops bring the same PC back to the same stage every iteration, which was
  incorrectly detected as a stall. Using stall_issue from the CSV is exact.
"""

import csv
import argparse

NAMES = ['PCG', 'FET', 'DEC', 'ISS', 'EXE']
PCG, FET, DEC, ISS, EXE = 0, 1, 2, 3, 4


def convert(csv_path, out_path, max_cycles=0):
    with open(csv_path) as f_in, open(out_path, 'w') as f_out:
        reader = csv.DictReader(f_in)
        f_out.write("Kanata\t0004\n")

        nid      = 0          # next instruction ID to assign
        slot     = [None] * 5 # instruction ID currently in each stage [PCG..EXE]
        pcom     = []         # instructions that left EXE, waiting for commit_valid
        pret     = []         # committed this cycle: emit E COM + R at start of next cycle
        last_cyc = 0

        for row in reader:
            cyc = int(row['cycle'])
            if max_cycles > 0 and cyc > max_cycles:
                break

            stall = int(row['stall_issue'])
            flush = int(row['flush'])

            # ── Step 1: Advance Konata clock, then drain retirements ──────────
            # Retirements are emitted AFTER the C advance so COM has 1-cycle width.
            if cyc > last_cyc:
                f_out.write(f"C\t{cyc - last_cyc}\n")
                last_cyc = cyc
                for iid in pret:
                    f_out.write(f"E\t{iid}\t0\tCOM\n")
                    f_out.write(f"R\t{iid}\t0\t0\n")
                pret.clear()

            new = [None] * 5

            # ── Step 2: EXE always drains every cycle ─────────────────────────
            # This is true even during stall (EXE is not stalled, only PCG..ISS are)
            # and during flush (the branch instruction in EXE completes normally).
            if slot[EXE] is not None:
                f_out.write(f"E\t{slot[EXE]}\t0\tEXE\n")
                f_out.write(f"S\t{slot[EXE]}\t0\tCOM\n")
                pcom.append(slot[EXE])

            # ── Step 3: Flush — kill wrong-path PCG..ISS instructions ─────────
            if flush:
                for i in [PCG, FET, DEC, ISS]:
                    if slot[i] is not None:
                        f_out.write(f"E\t{slot[i]}\t0\t{NAMES[i]}\n")
                        f_out.write(f"R\t{slot[i]}\t0\t1\n")
                # new[] stays all-None: pipeline empties, will refill from redirect target
                # Note: pcom entries are OLDER than the branch (already left EXE before the
                # branch) — let them commit normally. pret same.

            # ── Step 4: Stall — PCG/FET/DEC/ISS hold, EXE gets bubble ────────
            elif stall:
                # Hold all upstream stages unchanged (no E/S events emitted)
                for i in [PCG, FET, DEC, ISS]:
                    new[i] = slot[i]
                # new[EXE] = None: issue_ex_next.valid=false injects a bubble into EXE

            # ── Step 5: Normal advance — each stage moves forward ─────────────
            else:
                # ISS -> EXE
                if slot[ISS] is not None:
                    new[EXE] = slot[ISS]
                    f_out.write(f"E\t{slot[ISS]}\t0\tISS\n")
                    f_out.write(f"S\t{slot[ISS]}\t0\tEXE\n")
                # DEC -> ISS
                if slot[DEC] is not None:
                    new[ISS] = slot[DEC]
                    f_out.write(f"E\t{slot[DEC]}\t0\tDEC\n")
                    f_out.write(f"S\t{slot[DEC]}\t0\tISS\n")
                # FET -> DEC
                if slot[FET] is not None:
                    new[DEC] = slot[FET]
                    f_out.write(f"E\t{slot[FET]}\t0\tFET\n")
                    f_out.write(f"S\t{slot[FET]}\t0\tDEC\n")
                # PCG -> FET
                if slot[PCG] is not None:
                    new[FET] = slot[PCG]
                    f_out.write(f"E\t{slot[PCG]}\t0\tPCG\n")
                    f_out.write(f"S\t{slot[PCG]}\t0\tFET\n")
                # New instruction enters PCG output this cycle
                if int(row['pcgen_valid']):
                    new[PCG] = nid
                    f_out.write(f"I\t{nid}\t0\t0\n")
                    f_out.write(f"L\t{nid}\t0\t{row['pc_pcgen']} c={cyc}\n")
                    f_out.write(f"S\t{nid}\t0\tPCG\n")
                    nid += 1

            # ── Step 6: Commit — move head of pcom to pret ────────────────────
            # E COM + R are deferred to the next cycle (Step 1) to give COM 1-cycle width
            if int(row['commit_valid']) and not flush:
                if pcom:
                    pret.append(pcom.pop(0))

            slot = new

    print(f"Done: {nid} instructions tracked -> {out_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Convert pipeline_trace.csv to Konata format")
    parser.add_argument("csv", help="Input pipeline_trace.csv")
    parser.add_argument("-o", "--out", default="pipeline.konata", help="Output .log file")
    parser.add_argument("--max-cycles", type=int, default=0, help="Stop after N cycles (0=no limit)")
    args = parser.parse_args()
    convert(args.csv, args.out, args.max_cycles)
