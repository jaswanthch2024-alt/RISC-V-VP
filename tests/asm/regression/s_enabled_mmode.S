# Regression: s_enabled must be false in M-mode.
# Bug: s_enabled was incorrectly true in M-mode, causing STIP to fire there.
#
# Test: while running in M-mode, assert STIP pending and SIE=1 in mstatus.
# A correct VP continues past the NOPs and writes PASS.
# A buggy VP (s_enabled=true in M-mode) takes an S-mode timer interrupt and
# diverts to a trap handler, never reaching PASS.
#
# No trap vector is set up, so any unexpected trap will loop at address 0
# (default mtvec=0) and the VP will timeout -> reported as FAIL/TIMEOUT.

.section .tohost,"aw",@progbits
.align 6
.globl tohost;   tohost:   .dword 0
.globl fromhost; fromhost: .dword 0

.section .text.init
.globl _start
_start:
  # Enable STIE in mie (bit 5 = Supervisor Timer Interrupt Enable).
  li   t0, (1<<5)
  csrw mie, t0

  # Set mstatus.SIE = 1 (bit 1) so S-mode interrupts are nominally enabled.
  li   t0, (1<<1)
  csrs mstatus, t0

  # Set STIP (bit 5) in mip to simulate a pending S-mode timer interrupt.
  li   t0, (1<<5)
  csrw mip, t0

  # A correct VP: we are in M-mode, so s_enabled is false -> no interrupt fires.
  # A buggy VP:   s_enabled is true in M-mode -> interrupt fires here.
  nop
  nop
  nop

  # Clear the pending interrupt so it doesn't affect anything after this test.
  csrw mip, zero

  # Reached here without being interrupted -> PASS.
  li   t0, 1
  la   t5, tohost
  sw   t0, 0(t5)
1:
  j 1b
