#include <gnu/libc-version.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    double x = argc > 1 ? atof(argv[1]) : 2.0;
    printf("hello from C, glibc %s, sqrt(%g) = %g\n", gnu_get_libc_version(), x, sqrt(x));
    return 0;
}
