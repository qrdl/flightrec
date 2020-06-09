int baz(void) {
    return 42;
}

int foo(double bar) {
    if (bar > 0.2)
        return (int)(bar * bar);
    else
        return baz() * baz();
}

