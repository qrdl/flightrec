#include <signal.h>

int main(void) {
    for (int i = 0; i < 16; ) {
        i++;
    }
    raise(SIGSEGV);
    return 0;
}
