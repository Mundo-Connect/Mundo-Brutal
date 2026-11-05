#include "kernel_shim.h"
#include "../mundo.c"

static struct net network;

static void start(struct sock *sk, int family, u16 port)
{
    memset(sk, 0, sizeof(*sk));
    assert(!pthread_mutex_init(&sk->lock, NULL));
    sk->sk_family = family;
    sk->sk_state = TCP_ESTABLISHED;
    sk->sk_prot = family == AF_INET ? &tcp_prot : &tcpv6_prot;
    sk->sk_max_pacing_rate = UINT64_MAX;
    sk->net = &network;
    sk->inet.inet_sport = port;
    sk->tcp.mss_cache = 1200;
    sk->tcp.srtt_us = 200000 * 8;
    sk->tcp.snd_cwnd = TCP_INIT_CWND;
    sk->tcp.snd_cwnd_clamp = UINT32_MAX;
    sk->icsk.icsk_ca_ops = &tcp_mundo_ops;
    mundo_init(sk);
}

static void stop(struct sock *sk)
{
    mundo_release(sk);
    assert(sk->sk_prot == (sk->sk_family == AF_INET ? &tcp_prot : &tcpv6_prot));
    assert(!pthread_mutex_destroy(&sk->lock));
}

static int params(struct sock *sk, u64 rate)
{
    struct mundo_params p = {rate, INIT_CWND_GAIN};
    return mundo_set_params(sk, &p, sizeof(p));
}

static void ack(struct sock *sk, u32 elapsed, u32 packets, bool limited)
{
    struct rate_sample rs = {
        .delivered = packets, .interval_us = elapsed,
        .acked_sacked = packets, .is_app_limited = limited
    };
    sk->tcp.tcp_mstamp += elapsed;
    sk->tcp.delivered += packets;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,10,0)
    mundo_main(sk, 0, 0, &rs);
#else
    mundo_main(sk, &rs);
#endif
}

static void test_tuple_lifecycle(void)
{
    struct sock a,b,c;
    start(&a, AF_INET, 443);
    start(&b, AF_INET, 443);
    start(&c, AF_INET6, 443);
    assert(!params(&a, 12500000));
    assert(!params(&b, 6250000));
    assert(!params(&c, 25000000));
    struct mundo *ma=inet_csk_ca(&a), *mb=inet_csk_ca(&b);
    assert(ma->peer == mb->peer && atomic_load(&allocations)==2);
    assert(ma->peer->refs == 2);
    mundo_cwnd_event(&a, CA_EVENT_TX_START);
    assert(mundo_max_rate(ma) == 6250000);
    mundo_cwnd_event(&b, CA_EVENT_TX_START);
    assert(mundo_max_rate(ma) == 3125000);
    ma->rate=12500000;ma->delivery=12500000;ma->round=3;
    a.tcp.inflight=2000;mundo_apply_rate(&a);
    ack(&a,USEC_PER_SEC,2604,false);
    assert(ma->rate==12500000); /* another live socket must not erase capacity */
    mundo_cwnd_event(&b, CA_EVENT_LOSS);
    assert(mundo_max_rate(ma) == 6250000);
    stop(&b);
    assert(mundo_max_rate(ma) == 6250000); /* lowest cap lasts until last close */
    stop(&a);stop(&c);
    assert(atomic_load(&allocations)==0);

    start(&a, AF_INET, 443);
    atomic_store(&fail_alloc, 1);
    assert(params(&a, 12500000)==-ENOMEM);
    atomic_store(&fail_alloc, 0);
    assert(!params(&a, 12500000));
    stop(&a);
    assert(atomic_load(&allocations)==0);
}

static void test_parameters_and_switch(void)
{
    struct sock sk;
    struct mundo_params p = {12500000, INIT_CWND_GAIN};
    struct tcp_congestion_ops other;
    struct proto ulp;
    struct ulp_ops tls = {.name="tls"};
    struct tls_context ctx;
    start(&sk, AF_INET, 444);
    assert(mundo_set_params(&sk, NULL, sizeof(p))==-EFAULT);
    assert(mundo_set_params(&sk, &p, sizeof(p)-1)==-EINVAL);
    assert(params(&sk, 0)==-EINVAL);
    assert(params(&sk, MAX_PACING_RATE+1)==-EINVAL);
    sk.sk_state=0;assert(params(&sk, 12500000)==-ENOTCONN);sk.sk_state=TCP_ESTABLISHED;
    sk.icsk.icsk_user_timeout=99;
    assert(!params(&sk, 12500000));
    assert(sk.icsk.icsk_user_timeout==99);
    /* A saved proto callback may run after another thread changes the CA. */
    mundo_release(&sk);
    sk.icsk.icsk_ca_ops=&other;
    memset(sk.icsk.ca.bytes, 0x5a, sizeof(sk.icsk.ca.bytes));
    assert(mundo_set_params(&sk, &p, sizeof(p))==-ENOPROTOOPT);
    assert(!pthread_mutex_destroy(&sk.lock));
    assert(atomic_load(&allocations)==0);

    start(&sk, AF_INET6, 444);
    ctx.sk_proto=sk.sk_prot;
    sk.tls=&ctx;sk.icsk.icsk_ulp_ops=&tls;sk.sk_prot=&ulp;
    mundo_release(&sk);
    assert(sk.sk_prot==&ulp && ctx.sk_proto==&tcpv6_prot);
    assert(!pthread_mutex_destroy(&sk.lock));
}

static void test_feedback_and_precision(void)
{
    struct sock sk;
    start(&sk, AF_INET, 445);
    assert(!params(&sk, MAX_PACING_RATE));
    struct mundo *m=inet_csk_ca(&sk);
    m->rate=12500000;m->delivery=12500000;m->round=7;
    sk.tcp.inflight=3000;
    mundo_apply_rate(&sk);
    ack(&sk, USEC_PER_SEC, 10417, false);
    assert(m->rate==15625000 && m->round==1);
    ack(&sk, USEC_PER_SEC, 10417, false);
    assert(m->rate==10937500 && m->round==2);
    ack(&sk, USEC_PER_SEC, 10417, false);
    assert(m->rate==9570313 && m->round==2);
    ack(&sk, USEC_PER_SEC, 7900, false);
    assert(m->rate==10937500 && m->round==3);
    assert(m->delivery==12500400); /* unsuccessful probes retain the accepted baseline */
    /* No ACK for several seconds must not poison a learned rate. */
    u64 rate=m->rate;
    ack(&sk, 10000000, 100, false);
    assert(m->rate==rate && !m->delivery);
    sk.tcp.inflight=0;
    ack(&sk, USEC_PER_SEC, 1, true);
    assert(m->rate==rate);
    mundo_cwnd_event(&sk, CA_EVENT_TX_START);
    assert(m->stamp==(u32)sk.tcp.tcp_mstamp && m->delivered==sk.tcp.delivered);

    sk.tcp.mss_cache=48;sk.tcp.srtt_us=500*8;
    m->rate=12500000;mundo_apply_rate(&sk);
    assert(sk.tcp.snd_cwnd==234); /* no 536-byte MSS or 1-ms RTT floor */
    sk.sk_max_pacing_rate=1000000;mundo_apply_rate(&sk);
    assert(sk.sk_pacing_rate==1000000 && sk.tcp.snd_cwnd==10);
    sk.sk_max_pacing_rate=UINT32_MAX;sk.tcp.srtt_us=UINT32_MAX;
    m->rate=MAX_PACING_RATE;mundo_apply_rate(&sk);
    u64 bytes=(unsigned __int128)UINT32_MAX*(UINT32_MAX>>3)/USEC_PER_SEC;
    assert(sk.tcp.snd_cwnd==min_t(u64,bytes*INIT_CWND_GAIN/(10*88),UINT32_MAX));
    assert(sk.sk_pacing_rate==UINT32_MAX);

    sk.sk_max_pacing_rate=UINT64_MAX;sk.tcp.mss_cache=65535;
    m->rate=MAX_PACING_RATE;mundo_apply_rate(&sk);
    assert(sk.sk_pacing_rate==MAX_PACING_RATE);

    /* Both native 32-bit counters wrap; differences remain correct. */
    sk.sk_max_pacing_rate=UINT64_MAX;sk.tcp.srtt_us=200000*8;
    sk.tcp.mss_cache=1200;sk.tcp.inflight=1000;
    sk.tcp.tcp_mstamp=UINT32_MAX-500000;sk.tcp.delivered=UINT32_MAX-500;
    mundo_reset_sample(m,&sk.tcp);m->rate=12500000;m->round=3;
    ack(&sk, USEC_PER_SEC, 10417, false);
    assert(m->delivery==12500400);
    m->rate=16000;m->delivery=16000;m->round=3;mundo_apply_rate(&sk);
    ack(&sk,USEC_PER_SEC,13,false);
    assert(m->rate==16000); /* the cap's 500-Kbps ABI floor is not a path-rate floor */
    stop(&sk);
}

static void test_shared_ceiling_preserves_capacity(void)
{
    struct sock sk;start(&sk,AF_INET,449);
    assert(!params(&sk,1000000));
    struct mundo *m=inet_csk_ca(&sk);
    m->rate=10000000;m->round=3;sk.tcp.inflight=1000;
    mundo_apply_rate(&sk);
    for(unsigned i=0;i<16;i++)ack(&sk,USEC_PER_SEC,800,false);
    assert(m->rate==10000000 && m->round==7);
    /* A cap is not permission to ignore a genuinely narrower path. */
    ack(&sk,USEC_PER_SEC,80,false);
    assert(m->rate==8750000 && m->round==2);
    stop(&sk);
}

static void test_startup_jitter(void)
{
    struct sock sk;start(&sk,AF_INET,447);
    assert(!params(&sk,MAX_PACING_RATE));
    struct mundo *m=inet_csk_ca(&sk);sk.tcp.inflight=1000;
    ack(&sk,USEC_PER_SEC,1000,false);
    assert(!m->rate && m->delivery==1200000);
    ack(&sk,USEC_PER_SEC,800,false);
    assert(!m->rate && m->delivery==1200000);
    ack(&sk,USEC_PER_SEC,1000,false);
    assert(m->rate==1200000);
    stop(&sk);

    start(&sk,AF_INET,447);
    assert(!params(&sk,MAX_PACING_RATE));
    for(int i=0;i<100;i++)ack(&sk,USEC_PER_SEC,1,true);
    assert(sk.tcp.snd_cwnd==TCP_INIT_CWND);
    stop(&sk);
}

static void test_delivery_model(void)
{
    for (unsigned loss=0;loss<=20;loss+=5)
    {
        struct sock sk;start(&sk,AF_INET,448);
        assert(!params(&sk,MAX_PACING_RATE));
        struct mundo *m=inet_csk_ca(&sk);
        m->rate=1000000;m->round=7;sk.tcp.inflight=1000;
        mundo_apply_rate(&sk);
        u64 high=0,low=0,wire=0;
        for(unsigned round=0;round<160;round++)
        {
            u64 capacity=round>=32 && round<96?2000000:1000000;
            u64 offered=sk.sk_pacing_rate;
            u64 payload=mul_u64_u32_div(offered,1200,1240);
            u64 delivery=min_t(u64,capacity,payload*(100-loss)/100);
            if(round>=80 && round<96)high+=delivery;
            if(round>=128){low+=delivery;wire+=offered;}
            ack(&sk,USEC_PER_SEC,delivery/1200,false);
        }
        assert(high/16>=1600000 && low/32>=900000);
        assert(wire/32<=((1000000ULL*1240/1200)*100/(100-loss))*5/4);
        stop(&sk);
    }
}

static void *peer_worker(void *arg)
{
    (void)arg;
    for(int i=0;i<2000;i++)
    {
        struct sock sk;
        start(&sk, AF_INET, 446);
        assert(!params(&sk, 12500000));
        mundo_cwnd_event(&sk, CA_EVENT_TX_START);
        stop(&sk);
    }
    return NULL;
}

int main(void)
{
    assert(!mundo_register());
    test_tuple_lifecycle();test_parameters_and_switch();test_feedback_and_precision();test_startup_jitter();test_delivery_model();test_shared_ceiling_preserves_capacity();
    pthread_t workers[8];
    for(int i=0;i<8;i++)assert(!pthread_create(&workers[i],NULL,peer_worker,NULL));
    for(int i=0;i<8;i++)assert(!pthread_join(workers[i],NULL));
    assert(!atomic_load(&allocations));
    for(size_t i=0;i<HASH_SIZE(mundo_peers);i++)assert(!mundo_peers[i].first);
    mundo_unregister();
    printf("actual C tests passed; CA state=%zu bytes, peer=%zu bytes; 16000 concurrent lifecycles\n",
           sizeof(struct mundo), sizeof(struct mundo_peer));
}
