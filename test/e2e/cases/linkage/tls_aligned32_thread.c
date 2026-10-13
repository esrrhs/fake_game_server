// expect: 0
// gcc_flags: -pthread

// An aligned(32) TLS object must stay 32-aligned in a child thread too.
//
// The descriptor's offset field is where the variable sits inside this
// thread's copy of the block, so the static layout has to round the
// zero-filled half up to the strongest alignment any __thread object asked
// for -- otherwise tx lands at offset 8 and every access to it is misaligned.
// See macho_tlv_block_tbss_base, and tls_init_thread for the rest of the TLV
// path.
package main;
import runtime;

__thread char tc = 1;
__thread int tx __attribute__((aligned(32)));

void *worker(void *arg) {
    (void)arg;
    tx = 1;
    if (((unsigned long)&tx & 31ul) != 0) return (void *)1;
    return (void *)0;
}

int main(void) {
    runtime.pthread_t t;
    void *r = (void *)2;
    if (runtime.pthread_create(&t, 0, worker, 0) != 0) return 3;
    runtime.pthread_join(t, &r);
    if ((long)r != 0) return 4;
    tx = 1;
    if (((unsigned long)&tx & 31ul) != 0) return 5;
    return 0;
}
