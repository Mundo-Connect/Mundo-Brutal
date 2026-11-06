#include <linux/module.h>
#include <linux/version.h>
#include <linux/limits.h>
#include <linux/jiffies.h>
#include <linux/hashtable.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/math64.h>
#include <net/tcp.h>

#if IS_ENABLED(CONFIG_TLS)
#include <net/tls.h>
#endif

#if IS_ENABLED(CONFIG_IPV6) && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
#include <net/transp_v6.h>
#endif

#define MIN_CONFIG_RATE 62500ULL   /* 500 Kbps negotiated ceiling */
#define MAX_PACING_RATE 1250000000000ULL /* 10 Tbps */

#define INIT_CWND_GAIN 18
#define MIN_CWND_GAIN 5
#define MAX_CWND_GAIN 80
#define MIN_CWND 4

#define TCP_BRUTAL_PARAMS 23301
#define MUNDO_PEER_HASH_BITS 8
#define MUNDO_USER_TIMEOUT_MS 120000U
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
#define MUNDO_USER_TIMEOUT msecs_to_jiffies(MUNDO_USER_TIMEOUT_MS)
#else
#define MUNDO_USER_TIMEOUT MUNDO_USER_TIMEOUT_MS
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
static u32 mundo_stamp(const struct tcp_sock *tp)
{
    return tp->tcp_mstamp;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
static u32 mundo_stamp(const struct tcp_sock *tp)
{
    return tp->tcp_mstamp.stamp_us;
}
#else
static u32 mundo_stamp(const struct tcp_sock *tp)
{
    return jiffies_to_usecs(tcp_time_stamp);
}
#endif

struct mundo_peer_key
{
    struct net *net;
    union
    {
        struct
        {
            __be32 remote;
            __be32 local;
        } v4;
#ifdef _TRANSP_V6_H
        struct
        {
            struct in6_addr remote;
            struct in6_addr local;
        } v6;
#endif
    } addr;
    __be16 port;
    u16 family;
};

struct mundo_peer
{
    struct hlist_node node;
    struct mundo_peer_key key;
    atomic64_t max_rate;
    atomic_t active;
    u32 refs;
};

static DEFINE_HASHTABLE(mundo_peers, MUNDO_PEER_HASH_BITS);
static DEFINE_SPINLOCK(mundo_peers_lock);

struct mundo
{
    struct mundo_peer *peer;
    u64 rate; /* Payload bytes/s; TCP pacing accounts for packet headers. */
    u64 delivery;
    u32 stamp;
    u32 delivered;
    int active; /* Last tuple count used for pacing. */
    u8 cwnd_gain;
    u8 round;
    u8 peer_active;
};

struct mundo_params
{
    u64 rate;      /* Maximum send rate in bytes per second */
    u32 cwnd_gain; /* CWND gain in tenths (10=1.0) */
} __packed;

static void mundo_peer_key_from_sock(struct sock *sk, struct mundo_peer_key *key)
{
    const struct inet_sock *inet = inet_sk(sk);

    memset(key, 0, sizeof(*key));
    key->net = sock_net(sk);
    key->family = sk->sk_family;
    key->port = inet->inet_sport;
    if (sk->sk_family == AF_INET)
    {
        key->addr.v4.remote = inet->inet_daddr;
        key->addr.v4.local = inet->inet_saddr;
    }
#ifdef _TRANSP_V6_H
    else
    {
        key->addr.v6.remote = sk->sk_v6_daddr;
        key->addr.v6.local = sk->sk_v6_rcv_saddr;
    }
#endif
}

static struct mundo_peer *mundo_peer_find(const struct mundo_peer_key *key, u32 hash)
{
    struct mundo_peer *peer;

    hash_for_each_possible(mundo_peers, peer, node, hash)
    {
        if (!memcmp(&peer->key, key, sizeof(*key)))
            return peer;
    }
    return NULL;
}

static struct mundo_peer *mundo_peer_get(struct sock *sk, u64 max_rate)
{
    struct mundo_peer_key key;
    struct mundo_peer *peer;
    struct mundo_peer *created;
    u32 hash;

    mundo_peer_key_from_sock(sk, &key);
    hash = jhash(&key, sizeof(key), 0);

    spin_lock_bh(&mundo_peers_lock);
    peer = mundo_peer_find(&key, hash);
    if (peer)
    {
        peer->refs++;
        atomic64_set(&peer->max_rate, min_t(u64, atomic64_read(&peer->max_rate), max_rate));
        spin_unlock_bh(&mundo_peers_lock);
        return peer;
    }
    spin_unlock_bh(&mundo_peers_lock);

    created = kmalloc(sizeof(*created), GFP_KERNEL);
    if (!created)
        return NULL;
    created->key = key;
    atomic64_set(&created->max_rate, max_rate);
    atomic_set(&created->active, 0);
    created->refs = 1;

    spin_lock_bh(&mundo_peers_lock);
    peer = mundo_peer_find(&key, hash);
    if (peer)
    {
        peer->refs++;
        atomic64_set(&peer->max_rate, min_t(u64, atomic64_read(&peer->max_rate), max_rate));
    }
    else
    {
        peer = created;
        hash_add(mundo_peers, &peer->node, hash);
        created = NULL;
    }
    spin_unlock_bh(&mundo_peers_lock);
    kfree(created);
    return peer;
}

static void mundo_peer_limit(struct mundo_peer *peer, u64 max_rate)
{
    spin_lock_bh(&mundo_peers_lock);
    atomic64_set(&peer->max_rate, min_t(u64, atomic64_read(&peer->max_rate), max_rate));
    spin_unlock_bh(&mundo_peers_lock);
}

static void mundo_peer_put(struct mundo_peer *peer)
{
    spin_lock_bh(&mundo_peers_lock);
    if (--peer->refs)
    {
        spin_unlock_bh(&mundo_peers_lock);
        return;
    }
    hash_del(&peer->node);
    spin_unlock_bh(&mundo_peers_lock);
    kfree(peer);
}

static u64 mundo_max_rate(struct mundo *mundo)
{
    struct mundo_peer *peer = mundo->peer;
    int active;

    if (!peer)
        return MAX_PACING_RATE;
    active = atomic_read(&peer->active);
    mundo->active = active;
    return max_t(u64, div_u64(atomic64_read(&peer->max_rate), max(active, 1)), 1);
}

static void mundo_peer_set_active(struct mundo *mundo, bool active)
{
    if (!mundo->peer || mundo->peer_active == active)
        return;
    mundo->peer_active = active;
    if (active)
        atomic_inc(&mundo->peer->active);
    else
        atomic_dec(&mundo->peer->active);
}

static struct proto tcp_prot_override __ro_after_init;
#ifdef _TRANSP_V6_H
static struct proto tcpv6_prot_override __ro_after_init;
#endif

/* The TCP socket lock serializes parameters, ACKs and CA teardown. */
static struct tcp_congestion_ops tcp_mundo_ops;

static void mundo_reset_sample(struct mundo *mundo, const struct tcp_sock *tp)
{
    mundo->stamp = mundo_stamp(tp);
    mundo->delivered = tp->delivered;
}

static void mundo_apply_rate(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct mundo *mundo = inet_csk_ca(sk);
    u64 pacing = mundo->rate;
    u64 cwnd;
    u32 rtt = max(tp->srtt_us >> 3, 1U);
    u32 mtu = tcp_mss_to_mtu(sk, tp->mss_cache);

    if (!pacing)
    {
        /* ACK-driven startup: grow the existing TCP window, not a fraction
         * of an arbitrarily large configured bandwidth ceiling. */
        u64 bytes = min_t(u64, (u64)tp->snd_cwnd * mtu,
                          (MAX_PACING_RATE / USEC_PER_SEC) * rtt);
        pacing = mul_u64_u32_div(bytes, 2 * USEC_PER_SEC, rtt);
    }
    else
        /* The learned rate is bounded and MSS is negotiated in 16 bits. */
        pacing = div_u64(pacing * mtu, tp->mss_cache);
    pacing = min_t(u64, pacing, mundo_max_rate(mundo));
    pacing = min_t(u64, pacing, READ_ONCE(sk->sk_max_pacing_rate));

    if (mundo->rate)
    {
        /* Keep microsecond precision and avoid rate * RTT overflow on 32/64 bit. */
        cwnd = mul_u64_u32_div(pacing, rtt, USEC_PER_SEC);
        cwnd = div_u64(cwnd * mundo->cwnd_gain, 10 * mtu);
        tp->snd_cwnd = min_t(u64, max_t(u64, cwnd, MIN_CWND), tp->snd_cwnd_clamp);
    }
    WRITE_ONCE(sk->sk_pacing_rate, pacing);
}

static bool mundo_update_model(struct sock *sk, u32 elapsed, u32 interval, bool limited)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct mundo *mundo = inet_csk_ca(sk);
    u32 packets;
    u64 delivery;

    packets = tp->delivered - mundo->delivered;
    if (packets < MIN_CWND)
        return false;
    mundo_reset_sample(mundo, tp);
    if (elapsed > max_t(u64, 2 * USEC_PER_SEC, (u64)interval * 4))
    {
        /* An ACK outage is not a fresh capacity measurement. */
        mundo->delivery = 0;
        mundo->round = mundo->rate ? 7 : 0;
        return true;
    }
    /* Cumulative delivery is nonoverlapping and averages ACK compression. */
    delivery = clamp_t(u64, mul_u64_u32_div((u64)packets * tp->mss_cache,
                                          USEC_PER_SEC, elapsed), tp->mss_cache, MAX_PACING_RATE);
    if (limited && (!mundo->rate || delivery < mundo->rate))
        return false;
    if (mundo->rate)
    {
        u64 pacing = READ_ONCE(sk->sk_pacing_rate);
        u32 mtu = tcp_mss_to_mtu(sk, tp->mss_cache);

        if (pacing < div_u64(mundo->rate * mtu, tp->mss_cache) &&
            delivery >= div_u64((pacing - pacing / 8) * tp->mss_cache, mtu))
        {
            /* Sharing a configured ceiling is not a loss of path capacity. */
            mundo->delivery = delivery;
            mundo->round = 7;
            return true;
        }
    }
    if (!mundo->rate)
    {
        u64 window = mul_u64_u32_div(delivery, tp->srtt_us >> 3, USEC_PER_SEC);
        u32 limit = min_t(u64, max_t(u64, div_u64(2 * window, tp->mss_cache),
                                    TCP_INIT_CWND), tp->snd_cwnd_clamp);

        tp->snd_cwnd = min(tp->snd_cwnd, limit);
        if (delivery > mundo->delivery + mundo->delivery / 16)
            mundo->round = 0;
        else if (++mundo->round == 2)
        {
            mundo->rate = delivery;
            mundo->round = 7;
        }
        else
            return false; /* Keep the startup high-water mark across a low sample. */
    }
    else if (mundo->round == 1)
    {
        if (delivery > mundo->delivery + mundo->delivery / 16)
            mundo->rate += mundo->rate / 4;
        else
        {
            mundo->rate -= mundo->rate / 5; /* undo the upward probe */
            mundo->rate -= mundo->rate / 8;
            mundo->round = 2;
            delivery = mundo->delivery;
        }
    }
    else if (mundo->round == 2)
    {
        if (delivery >= mundo->delivery - mundo->delivery / 32)
            mundo->rate -= mundo->rate / 8;
        else
        {
            mundo->rate += mundo->rate / 7; /* undo the downward probe */
            mundo->round = 3;
            delivery = mundo->delivery;
        }
    }
    else if (delivery < mundo->delivery - mundo->delivery / 4)
    {
        mundo->rate -= mundo->rate / 8;
        mundo->round = 2;
    }
    else if (mundo->round == 7)
    {
        mundo->rate += mundo->rate / 4;
        mundo->round = 1;
    }
    else
        mundo->round++;
    mundo->delivery = delivery;
    if (mundo->rate)
        mundo->rate = clamp_t(u64, mundo->rate, tp->mss_cache, MAX_PACING_RATE);
    return true;
}

#ifdef _LINUX_SOCKPTR_H
static int mundo_set_params(struct sock *sk, sockptr_t optval, unsigned int optlen)
#else
static int mundo_set_params(struct sock *sk, char __user *optval, unsigned int optlen)
#endif
{
    struct mundo *mundo = inet_csk_ca(sk);
    struct mundo_params params;
    struct mundo_peer *peer;
    int err = 0;

    if (optlen < sizeof(params))
        return -EINVAL;

#ifdef _LINUX_SOCKPTR_H
    if (copy_from_sockptr(&params, optval, sizeof(params)))
        return -EFAULT;
#else
    if (copy_from_user(&params, optval, sizeof(params)))
        return -EFAULT;
#endif

    if (params.rate < MIN_CONFIG_RATE || params.rate > MAX_PACING_RATE)
        return -EINVAL;
    if (params.cwnd_gain < MIN_CWND_GAIN || params.cwnd_gain > MAX_CWND_GAIN)
        return -EINVAL;

    lock_sock(sk);
    if (inet_csk(sk)->icsk_ca_ops != &tcp_mundo_ops)
    {
        err = -ENOPROTOOPT;
        goto out;
    }
    if (sk->sk_state != TCP_ESTABLISHED)
    {
        err = -ENOTCONN;
        goto out;
    }
    if (!mundo->peer)
    {
        peer = mundo_peer_get(sk, params.rate);
        if (!peer)
        {
            err = -ENOMEM;
            goto out;
        }
        mundo->peer = peer;
        mundo_peer_set_active(mundo, tcp_packets_in_flight(tcp_sk(sk)) != 0);
    }
    else
    {
        mundo_peer_limit(mundo->peer, params.rate);
    }
    if (!inet_csk(sk)->icsk_user_timeout)
        inet_csk(sk)->icsk_user_timeout = MUNDO_USER_TIMEOUT;

    mundo->cwnd_gain = params.cwnd_gain;
    mundo_apply_rate(sk);
out:
    release_sock(sk);
    return err;
}

#ifdef _LINUX_SOCKPTR_H
static int mundo_tcp_setsockopt(struct sock *sk, int level, int optname, sockptr_t optval, unsigned int optlen)
#else
static int mundo_tcp_setsockopt(struct sock *sk, int level, int optname, char __user *optval, unsigned int optlen)
#endif
{
    if (level == IPPROTO_TCP && optname == TCP_BRUTAL_PARAMS)
        return mundo_set_params(sk, optval, optlen);
    return tcp_prot.setsockopt(sk, level, optname, optval, optlen);
}

#if defined(CONFIG_COMPAT) && !defined(_LINUX_SOCKPTR_H)
static int mundo_compat_setsockopt(struct sock *sk, int level, int optname, char __user *optval, unsigned int optlen)
{
    if (level == IPPROTO_TCP && optname == TCP_BRUTAL_PARAMS)
        return mundo_set_params(sk, optval, optlen);
    return tcp_prot.compat_setsockopt(sk, level, optname, optval, optlen);
}
#endif

static void mundo_init(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct mundo *mundo = inet_csk_ca(sk);

    if (sk->sk_prot == &tcp_prot)
        WRITE_ONCE(sk->sk_prot, &tcp_prot_override);
#ifdef _TRANSP_V6_H
    else if (sk->sk_prot == &tcpv6_prot)
        WRITE_ONCE(sk->sk_prot, &tcpv6_prot_override);
#endif

    tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;

    memset(mundo, 0, sizeof(*mundo));
    mundo->cwnd_gain = INIT_CWND_GAIN;
    mundo_reset_sample(mundo, tp);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
#endif

    mundo_apply_rate(sk);
}

static void mundo_restore_proto(struct proto **protp)
{
    struct proto *prot = READ_ONCE(*protp);

    if (prot == &tcp_prot_override)
        WRITE_ONCE(*protp, &tcp_prot);
#ifdef _TRANSP_V6_H
    else if (prot == &tcpv6_prot_override)
        WRITE_ONCE(*protp, &tcpv6_prot);
#endif
}

static void mundo_release(struct sock *sk)
{
    struct mundo *mundo = inet_csk_ca(sk);

    if (mundo->peer)
    {
        mundo_peer_set_active(mundo, false);
        mundo_peer_put(mundo->peer);
    }
    mundo_restore_proto(&sk->sk_prot);
#if IS_ENABLED(CONFIG_TLS)
    if (inet_csk(sk)->icsk_ulp_ops &&
        !strcmp(inet_csk(sk)->icsk_ulp_ops->name, "tls"))
    {
        struct tls_context *ctx = tls_get_ctx(sk);

        /* kTLS retains the base protocol for sockopts and close. */
        if (ctx)
            mundo_restore_proto(&ctx->sk_proto);
    }
#endif
}

static void mundo_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
    struct mundo *mundo = inet_csk_ca(sk);

    if (event == CA_EVENT_LOSS)
    {
        mundo_peer_set_active(mundo, false);
    }
    if (event == CA_EVENT_TX_START)
    {
        mundo_reset_sample(mundo, tcp_sk(sk));
        mundo_peer_set_active(mundo, true);
        mundo_apply_rate(sk);
    }
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
static void mundo_main(struct sock *sk, u32 ack, int flag, const struct rate_sample *rs)
#else
static void mundo_main(struct sock *sk, const struct rate_sample *rs)
#endif
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct mundo *mundo = inet_csk_ca(sk);

    u32 flight = tcp_packets_in_flight(tp);
    bool refresh = !mundo->rate;

    mundo_peer_set_active(mundo, flight != 0);
    if (refresh && !rs->is_app_limited)
        tp->snd_cwnd = min_t(u64, (u64)tp->snd_cwnd + rs->acked_sacked, tp->snd_cwnd_clamp);
    {
        u32 elapsed = mundo_stamp(tp) - mundo->stamp;
        u32 interval = max_t(u32, tp->srtt_us >> 2, mundo->rate ? USEC_PER_SEC : 100000U);

        if (elapsed >= interval)
            refresh |= mundo_update_model(sk, elapsed, interval, rs->is_app_limited);
    }
    if (refresh || (mundo->peer && mundo->active != atomic_read(&mundo->peer->active)))
        mundo_apply_rate(sk);
}

static u32 mundo_undo_cwnd(struct sock *sk)
{
    return tcp_sk(sk)->snd_cwnd;
}

static u32 mundo_ssthresh(struct sock *sk)
{
    return tcp_sk(sk)->snd_ssthresh;
}

static struct tcp_congestion_ops tcp_mundo_ops = {
    .flags = TCP_CONG_NON_RESTRICTED,
    .name = "brutal",
    .owner = THIS_MODULE,
    .init = mundo_init,
    .release = mundo_release,
    .cwnd_event = mundo_cwnd_event,
    .cong_control = mundo_main,
    .undo_cwnd = mundo_undo_cwnd,
    .ssthresh = mundo_ssthresh,
};

static int __init mundo_register(void)
{
    BUILD_BUG_ON(sizeof(struct mundo) > ICSK_CA_PRIV_SIZE);
    BUILD_BUG_ON(sizeof(struct mundo_params) != 12);

    tcp_prot_override = tcp_prot;
    tcp_prot_override.setsockopt = mundo_tcp_setsockopt;
#if defined(CONFIG_COMPAT) && !defined(_LINUX_SOCKPTR_H)
    tcp_prot_override.compat_setsockopt = mundo_compat_setsockopt;
#endif

#ifdef _TRANSP_V6_H
    tcpv6_prot_override = tcpv6_prot;
    tcpv6_prot_override.setsockopt = mundo_tcp_setsockopt;
#if defined(CONFIG_COMPAT) && !defined(_LINUX_SOCKPTR_H)
    tcpv6_prot_override.compat_setsockopt = mundo_compat_setsockopt;
#endif
#endif

    return tcp_register_congestion_control(&tcp_mundo_ops);
}

static void __exit mundo_unregister(void)
{
    tcp_unregister_congestion_control(&tcp_mundo_ops);
}

module_init(mundo_register);
module_exit(mundo_unregister);

MODULE_AUTHOR("Mundo Connect Project");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Mundo X Brutal congestion control for Mundo Connect Project");
MODULE_VERSION("0.1.0");
