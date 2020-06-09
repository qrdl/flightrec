#include <stdlib.h>

double foo[5] = {0, 0.1, 0.2, 0.3, 0.4};

int main(void) {
    int bar[] = { 1, 2, 3 };
    char baz[] = "This is string";

    foo[0] = foo[2] = foo[4] = 3.14159;
    bar[0] = 5;
    baz[4] = '\0';

    int *dynamic = calloc(sizeof(*dynamic), 3);
    dynamic[0] = dynamic[2] = 42;

    return 0;
}