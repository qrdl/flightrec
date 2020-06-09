enum foo {
    zero,
    one,
    two,
    three
};

int main(void) {
    enum foo bar = two;
    bar++;

    int baz = three;

    return 0;
}