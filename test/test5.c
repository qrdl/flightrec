#include <stdio.h>

/* unsigned globals */
double pi = 3.14159265;
float e = 2.71828182;

int main(void) {
    double foo = pi * -2;
    float bar = e / -2;

    foo /= 1000000000;
    e *= -0.000001;

    return 0;
}