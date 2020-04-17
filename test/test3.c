#include <stdio.h>

struct foo {
    int a;
    int b;
};

union bar {
    long    a;
    double  b;
    char    c[8];
};

enum baz {
    a, b, c
};

int main(void) {
    struct foo foo = {1, 2};
    union bar bar = { 0xFF };
    enum baz baz = b;

    baz = c;
    printf("%d, %lf, %d\n", foo.b, bar.b, baz);
    return 0;
}
