#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double y = 5;

char qwe[] = "0123456789abcdefghijklmopqrstuvwxyz";

struct bar {
    int     a;
    double   b;
};

extern int foo(double bar);

int (*z)(double) = foo;

int b(void) {
    return EXIT_SUCCESS;
}

int a(void) {
    return b();
}

int main(void) {
    int i;
    int *u = &i;
    int garr[5];
    struct bar bar[5];
    struct bar *cur = malloc(5 * sizeof(*cur));
    struct bar *cur1 = cur + 1;
    char *zzz = calloc(256, 1);
    strcpy(zzz, "fubar");

    for (i = 0; i < 5; i++) {
        y = i + 0.01;
        qwe[i] = ' ';
//        cur = bar + i;
        cur[i].a = i;
        cur[i].b = y;
        garr[i] = z(y);
        zzz[i]++;
        printf("%d\n", garr[i]);
        printf("Here %d\n", i);
    }
    free(zzz);
    i = 20;

    return a();
}

