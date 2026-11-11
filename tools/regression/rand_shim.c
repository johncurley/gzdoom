/* macOS regression-only interposer. Never linked into or shipped with GZDoom.
 * Outside an explicit diagnostic scope every rand() calls the system RNG.
 * Only the calling thread's scoped stream is substituted; other threads retain
 * their normal RNG. The stream is a test input, not an emulation of libc rand.
 */
#include <stdint.h>
#include <stdlib.h>

static _Thread_local int active;
static _Thread_local uint64_t stream, draws;

int gzdoom_test_rand(void)
{
    if (!active) return rand(); /* dyld does not interpose this image's calls. */
    stream = stream * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    ++draws;
    return (int)((stream >> 33) & RAND_MAX);
}

/* begin, snapshot, restore, end; snapshot/restore operate on two uint64_t's. */
int gzdoom_test_rng(unsigned op, uint64_t *state, uint64_t *count)
{
    if (!state || !count) return 0;
    if (op == 0)
    {
        if (active) return 0;
        active = 1;
        stream = UINT64_C(19831111);
        draws = 0;
    }
    else if (!active || op > 3) return 0;
    if (op == 2) { stream = *state; draws = *count; }
    else { *state = stream; *count = draws; }
    if (op == 3) active = 0;
    return 1;
}

__attribute__((used)) static const struct { const void *replacement, *original; }
interpose_rand __attribute__((section("__DATA,__interpose"))) =
    { (const void *)&gzdoom_test_rand, (const void *)&rand };
