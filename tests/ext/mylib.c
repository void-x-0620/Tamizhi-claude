#include "mylib.h"
int my_add(int a, int b) { return a + b; }
double my_scale(double x, double k) { return x * k; }
const char *my_greeting(void) { return "hello from C"; }
static int counter;
int my_counter_next(void) { return ++counter; }

/* used by raw.tzb: no header, so the signature must match Tamizhi's types */
long long twice(long long n) { return 2 * n; }
