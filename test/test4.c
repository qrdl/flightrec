#include <stdio.h>
#include <inttypes.h>

/* unsigned globals */
uint8_t ug8 = 1;
uint16_t ug16 = 2;
uint32_t ug32 = 3;
uint64_t ug64 = 4;

/* signed globals */
int8_t sg8 = -1;
int16_t sg16 = -2;
int32_t sg32 = -3;
int64_t sg64 = -4;

int main(void) {
    /* unsigned locals */
    uint8_t ul8 = ug8 + 1;
    uint16_t ul16 = ug16 + 2;
    uint32_t ul32 = ug32 + 3;
    uint64_t ul64 = ug64 + 4;

    /* signed locals */
    int8_t sl8 = sg8 - 1;
    int16_t sl16 = sg16 - 2;
    int32_t sl32 = sg32 - 3;
    int64_t sl64 = sg64 - 4;

    /* make printable character */
    ul8 = 'a';
    sl8 = 'b';
    ug8 = 'c';
    sg8 = 'd';

    return 0;
}