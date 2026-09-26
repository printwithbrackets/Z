/* C side of the export test: calls the Z functions the Z side publishes. */
#include <stdio.h>

/* Calls back into Z. The Z side declares these with `export`, which keeps the
 * symbol as written and makes it visible outside the Z translation unit. */
extern long z_triple(long v);
extern long z_sum_to(long n);
void c_call_back(void) {
    printf("c: z_triple(7) = %ld\n", z_triple(7));
    printf("c: z_sum_to(10) = %ld\n", z_sum_to(10));
}
