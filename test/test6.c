#include <string.h>
#include <stdlib.h>

int *gint;
double *gdbl;

int foo = 5;
double bar = 3.14159;

int main(void) {
    gint = &foo;
    gdbl = &bar;

    foo = 42;
    bar = 2.71828;

    gint = malloc(sizeof(*gint));
    *gint = 5;
    free(gint);
    int baz = 8;
    gint = &baz;

    char *str = "qwerty";
    str = calloc(6,1);
    strcpy(str, "asdfg");
    str[5] = 'A';

    int (* func)(void) = main;

    return 0;
}
