#include <string.h>
#include <stdio.h>

#define TRACE (*(volatile unsigned char *)0x40000000)

static void print(const char *msg) {
 int i =0;
 while (msg[i] != '\0') {
 TRACE = (unsigned char)msg[i];
 i++;
 }
}

int func1(int a, int* b) {
 return a - (*b);
}

int main(void) {
 printf("Using printf!\n");
 print("Using custom print!\n");
 asm volatile ("fence");
 asm volatile ("ecall");
 return0;
}
