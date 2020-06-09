#include <stddef.h>

typedef char my_char;
typedef char *my_string;
typedef int my_int;
typedef struct {
    my_int      foo;
    my_string   bar;
} my_struct;
typedef my_struct *my_pstruct;
typedef my_int  my_int_array[5];
typedef int (*my_func)(void);
typedef my_int my_int2;
typedef my_int *my_pint2;
typedef my_int **my_ppint2;
typedef struct opaque opaque;
typedef struct opaque *popaque;

int main(void) {
    my_char foo = 'A';
    my_string bar = "fubar";
    my_int baz = 42;
    my_struct zzz = { 65536, "256 squared" };
    my_pstruct p_zzz = &zzz;
    my_int_array array = {1, 2, 3, 4, 5};
    my_func func = main;
    my_string *pstring = &bar;
    my_int2 bazz = 35;
    my_pint2 bazzz = &bazz;
    my_ppint2 bazzzz = &bazzz;
    opaque *op = (opaque *)0xFFFF;
    popaque pop = op;

    return 0;
}