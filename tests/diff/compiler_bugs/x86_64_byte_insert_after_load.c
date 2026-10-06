#include <stdio.h>
#include <stdlib.h>
#include <string.h>
unsigned char *g;
struct C { unsigned eax; };
__attribute__((noinline)) void f(struct C *c, unsigned p) {
    unsigned v;
    memcpy(&v, g + p, 4);
    c->eax = v;
    c->eax = (c->eax & 0xFFFFFF00u) | g[c->eax];
}
int main(void) {
    g = calloc(1, 1 << 16);
    unsigned v = 0x1234;
    memcpy(g + 0x100, &v, 4);
    g[0x1234] = 5;
    struct C c = {0};
    f(&c, 0x100);
    printf("%08x (expect 00001205)\n", c.eax);
    return 0;
}
