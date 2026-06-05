# Regression: sepc must not be corrupted after MRET transitions priv M -> S.
# Test: set mepc = s_mode_entry, MRET, check we land at s_mode_entry (PASS).
# If sepc is corrupted or MRET jumps to the wrong PC, we never reach PASS.
#
# mstatus layout (RV64):
#   MPP  = bits 12:11.  MPP=01 => return to S-mode.
#   MPIE = bit 7.
# Writing 0x800 to mstatus sets MPP=01 (S-mode), all other fields 0.

.section .tohost,"aw",@progbits
.align 6
.globl tohost;   tohost:   .dword 0
.globl fromhost; fromhost: .dword 0

.section .text.init
.globl _start
_start:
  # Write mstatus: MPP=01 (S-mode), everything else 0.
  li   t0, 0x800
  csrw mstatus, t0

  # Point mepc at s_mode_entry.
  la   t0, s_mode_entry
  csrw mepc, t0

  # MRET: must jump to s_mode_entry and switch to S-mode.
  mret

  # Unreachable if MRET works correctly; write FAIL (test 1) if we land here.
  li   t0, 3          # (1<<1)|1 = FAIL test 1
  la   t5, tohost
  sw   t0, 0(t5)
1:
  j 1b

s_mode_entry:
  # Landed at the correct address -> PASS.
  li   t0, 1
  la   t5, tohost
  sw   t0, 0(t5)
2:
  j 2b
