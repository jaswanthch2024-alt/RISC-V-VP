/*
 * math_test.c - Simple integer math test for RISC-V VP
 * Computes multiply, divide, remainder on real values.
 * Bare-metal: no libc, direct peripheral writes.
 */

/* Choose output destination:
 * UART at 0x10000000 - goes to stdout
 * TRACE at 0x40000000 - goes to xterm window (if DISPLAY set)
 */
#ifndef USE_TRACE
#define USE_TRACE 0
#endif

#if USE_TRACE
#define OUTPUT_BASE ((volatile unsigned int *)0x40000000)
#else
#define OUTPUT_BASE ((volatile unsigned int *)0x10000000)
#endif

static void putchar_out(char c) {
    *OUTPUT_BASE = (unsigned int)c;
}

static void print_str(const char *s) {
    while (*s) putchar_out(*s++);
}

static void print_dec(long val) {
    char buf[24];
    int i = 22;
    int neg = 0;
    buf[23] = '\0';
    if (val < 0) { neg = 1; val = -val; }
    if (val == 0) { print_str("0"); return; }
    while (val && i >= 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    if (neg) buf[i--] = '-';
    print_str(&buf[i + 1]);
}

static void print_hex(unsigned long val) {
    const char hex[] = "0123456789ABCDEF";
    char buf[17];
    int i = 15;
    buf[16] = '\0';
    if (val == 0) { print_str("0"); return; }
    while (val && i >= 0) {
        buf[i--] = hex[val & 0xF];
        val >>= 4;
    }
    print_str(&buf[i + 1]);
}

/* Entry point - linker puts this at reset vector */
void _start(void) __attribute__((naked, section(".text.start")));
void _start(void) {
    /* Minimal stack setup - use end of memory (4MB) */
    asm volatile (
        "li sp, 0x400000\n"  /* stack at 4MB */
        "j main\n"
    );
}

int main(void) {
    long a = 7568;
    long b = 93735;

    long mul = a * b;
    long div_ab = b / a;
    long rem_ab = b % a;
    long div_ba = a / b;
    long rem_ba = a % b;

    print_str("=== Math Test ===\n");
    print_str("a = "); print_dec(a); print_str("\n");
    print_str("b = "); print_dec(b); print_str("\n");

    print_str("a * b = "); print_dec(mul); print_str(" (hex: "); print_hex((unsigned long)mul); print_str(")\n");
    print_str("b / a = "); print_dec(div_ab); print_str("\n");
    print_str("b % a = "); print_dec(rem_ab); print_str("\n");
    print_str("a / b = "); print_dec(div_ba); print_str("\n");
    print_str("a % b = "); print_dec(rem_ba); print_str("\n");

    print_str("=== Done ===\n");

    /* Trap to end simulation */
    asm volatile("ebreak");
    while(1); /* safety: don't return */
    return 0;
}
