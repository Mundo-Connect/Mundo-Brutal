/* Production ACK callback benchmark. Pass -DMUNDO_SOURCE='"/path/to/mundo.c"'
 * to compare another revision using the same scaffolding. */
#include "kernel_shim.h"
#include <time.h>
#ifndef MUNDO_SOURCE
#define MUNDO_SOURCE "../mundo.c"
#endif
#include MUNDO_SOURCE

int main(void)
{
    struct sock sk={0};
    struct timespec start,end;
    const unsigned count=10000000;
    struct mundo_params p={125000000,18};
    struct rate_sample rs={.delivered=100,.interval_us=100,.acked_sacked=1};
    assert(!mundo_register());
    assert(!pthread_mutex_init(&sk.lock,NULL));
    sk.sk_family=AF_INET;sk.sk_state=TCP_ESTABLISHED;sk.sk_prot=&tcp_prot;
    sk.sk_max_pacing_rate=UINT64_MAX;sk.icsk.icsk_ca_ops=&tcp_mundo_ops;
    sk.tcp.mss_cache=1200;sk.tcp.srtt_us=10000*8;
    sk.tcp.snd_cwnd=10;sk.tcp.snd_cwnd_clamp=UINT32_MAX;sk.tcp.inflight=100;
    mundo_init(&sk);assert(!mundo_set_params(&sk,&p,sizeof(p)));
    /* Start in the learned steady path; model correctness is tested separately. */
    ((struct mundo *)inet_csk_ca(&sk))->rate=12500000;
    ((struct mundo *)inet_csk_ca(&sk))->round=7;
    mundo_apply_rate(&sk);
    assert(!clock_gettime(CLOCK_MONOTONIC,&start));
    for(unsigned i=0;i<count;i++)
    {
        /* Exercise the per-ACK path before its next model deadline. */
        sk.tcp.tcp_mstamp+=(i & 31)==0;sk.tcp.delivered++;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,10,0)
        mundo_main(&sk,0,0,&rs);
#else
        mundo_main(&sk,&rs);
#endif
    }
    assert(!clock_gettime(CLOCK_MONOTONIC,&end));
    double ns=(end.tv_sec-start.tv_sec)*1e9+end.tv_nsec-start.tv_nsec;
    printf("%.2f ns/ACK, %zu-byte CA state, checksum=%llu\n",ns/count,
           sizeof(struct mundo),(unsigned long long)sk.sk_pacing_rate);
    mundo_release(&sk);assert(!atomic_load(&allocations));
    assert(!pthread_mutex_destroy(&sk.lock));mundo_unregister();
}
