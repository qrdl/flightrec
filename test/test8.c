#include <stdlib.h>
#include <stdint.h>

struct item {
    int         foo;
    double      bar;
    struct item *next;
};

union my_union {
    int32_t         a;
    unsigned char   b[4];
};

struct item first = { 7, 2.71828 };
union my_union mu = { .b = "abc" };

int main(void) {
    struct item second = {5, 3.14159, &first};

    second.foo = 2;
    first.bar -= 1;

    struct item *third = malloc(sizeof(*third));
    third->bar = 1;
    third->foo = -1.5;
    third->next = &second;

    struct item *two_items = malloc(sizeof(*two_items) * 2);
    two_items->next = third;
    two_items[1].next = &first;
    two_items->bar = two_items[1].bar = 7.5;
    two_items->foo = two_items[1].foo = -5;

    struct item **ppstruct = &third;

    mu.a += 0x10000;  // abc -> abd

    return 0;
}