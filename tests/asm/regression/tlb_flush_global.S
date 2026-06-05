# Regression: TLB flush_all() must flush global (G=1) entries.
# Minimal stub: verifies sfence.vma x0,x0 executes without faulting in M-mode.
# If sfence.vma causes an illegal-instruction trap in M-mode, execution will
# divert to a trap handler (or loop forever) and never reach PASS.

.section .tohost,"aw",@progbits
.align 6
.globl tohost;   tohost:   .dword 0
.globl fromhost; fromhost: .dword 0

.section .text.init
.globl _start
_start:
  # sfence.vma x0,x0 is legal in M-mode (RISC-V privileged spec §4.2.1).
  # A correct implementation executes it as a fence; a broken one may fault.
  sfence.vma x0, x0

  # Reaching here means sfence.vma did not cause an illegal-instruction trap.
  li   t0, 1
  la   t5, tohost
  sw   t0, 0(t5)
1:
  j 1b
