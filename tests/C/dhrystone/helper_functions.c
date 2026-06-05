#define TRACE (*(volatile unsigned char *)0x40000000)

// Bare-metal string functions
static char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

static int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (*(unsigned char *)s1) - (*(unsigned char *)s2);
}

static void *memcpy(void *dst, const void *src, unsigned long n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

int _read(int file, char* ptr, int len) {
    return 0;
}

int _close(int fd){
    return 0;
}

int _fstat_r(int fd) {
    return 0;
}

int _lseek_r(void *ptr, void *fp, long offset, int whence){
    return 0;
}

int _isatty_r(void *ptr, int fd) {
    return 0;
}

int _getpid(void) {
    return 0;
}

void _kill(int pid) {
}

int _write(int file, const char *ptr, int len) {
    int x;
    for (x = 0; x < len; x++) {
        TRACE = *ptr++;
    }
    return len;
}

