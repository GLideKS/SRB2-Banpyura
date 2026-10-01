/* Separate translation unit prevents constant folding of the runtime probe. */
long long ps2_probe_sdiv(long long a, long long b) { return a/b; }
unsigned long long ps2_probe_udiv(unsigned long long a, unsigned long long b) { return a/b; }
