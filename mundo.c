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

#if IS_ENABLED(CONFIG_IPV6) && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
#include <net/transp_v6.h>
#else
#warning IPv6 support is disabled. Mundo X Brutal will only work with IPv4. \
 Please enable CONFIG_IPV6 and use Linux 5.8 or newer for IPv6 support.
#endif

#define INIT_PACING_RATE 125000ULL /* 1 Mbps */
#define MIN_PACING_RATE 62500ULL   /* 500 Kbps */
#define MAX_PACING_RATE 1250000000000ULL /* 10 Tbps */

#define INIT_CWND_GAIN 18
#define MIN_CWND_GAIN 5
#define MAX_CWND_GAIN 80
#define MIN_CWND 4
#define FALLBACK_MSS 536

#define MIN_PKT_INFO_SAMPLES 50
#define HEADROOM_PERMILLE 5
#define DELIVERY_HEADROOM_PERMILLE 30
#define DELIVERY_LOW_PERMILLE 50
#define DELIVERY_FLAT_PERMILLE 30
#define BAD_CONFIRM_ROUNDS 2
#define APP_LIMITED_PROBE_DIV 16
#define MIN_PROBE_STEP 16384ULL
#define FAST_START_DIV 4
#define DIRECT_START_RATE 1000000ULL /* 8 Mbps */

#define TCP_BRUTAL_PARAMS 23301
#define MUNDO_PEER_HASH_BITS 8
#define MUNDO_USER_TIMEOUT_MS 120000U
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
#define MUNDO_USER_TIMEOUT msecs_to_jiffies(MUNDO_USER_TIMEOUT_MS)
#else
#define MUNDO_USER_TIMEOUT MUNDO_USER_TIMEOUT_MS
#endif

#ifndef ICSK_CA_PRIV_SIZE
#error "ICSK_CA_PRIV_SIZE not defined"
#else
#define MUNDO_FIXED_PRIV_SIZE 32
#define RAW_PKT_INFO_SLOTS ((ICSK_CA_PRIV_SIZE - MUNDO_FIXED_PRIV_SIZE) / sizeof(u32))
#define PKT_INFO_SLOTS (RAW_PKT_INFO_SLOTS < 3 ? 3 : (RAW_PKT_INFO_SLOTS > 4 ? 4 : RAW_PKT_INFO_SLOTS))
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
static u64 tcp_sock_get_sec(const struct tcp_sock *tp)
{
    return div_u64(tp->tcp_mstamp, USEC_PER_SEC);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
static u64 tcp_sock_get_sec(const struct tcp_sock *tp)
{
    return div_u64(tp->tcp_mstamp.stamp_us, USEC_PER_SEC);
}
#else
static u64 tcp_sock_get_sec(const struct tcp_sock *tp)
{
    return div_u64(jiffies_to_usecs(tcp_time_stamp), USEC_PER_SEC);
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
        struct
        {
            struct in6_addr remote;
            struct in6_addr local;
        } v6;
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
    u64 rate;
    u32 last_sec;
    u32 peak_delivery;
    u8 cwnd_gain;
    u8 bad_rounds;
    u8 slot;
    u8 active_mask;
    u8 peer_active;
    u32 slots[PKT_INFO_SLOTS];
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
    bool release = false;

    spin_lock_bh(&mundo_peers_lock);
    if (!--peer->refs)
    {
        hash_del(&peer->node);
        release = true;
    }
    spin_unlock_bh(&mundo_peers_lock);
    if (release)
        kfree(peer);
}

static u64 mundo_max_rate(const struct mundo *mundo)
{
    struct mundo_peer *peer = mundo->peer;
    int active;

    if (!peer)
        return INIT_PACING_RATE;
    active = atomic_read(&peer->active);
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

static inline u32 mundo_tcp_snd_cwnd(const struct tcp_sock *tp)
{
    return tp->snd_cwnd;
}

static inline void mundo_tcp_snd_cwnd_set(struct tcp_sock *tp, u32 val)
{
    WARN_ON_ONCE((int)val <= 0);
    tp->snd_cwnd = val;
}

static void mundo_apply_rate(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct mundo *mundo = inet_csk_ca(sk);
    u64 rate = mundo->rate;
    u64 max_rate = mundo_max_rate(mundo);
    u64 pacing;
    u64 max_pacing;
    u64 cwnd;
    u32 mss = tp->mss_cache;
    u32 rtt_ms = (tp->srtt_us >> 3) / USEC_PER_MSEC;

    if (mss < FALLBACK_MSS)
        mss = FALLBACK_MSS;
    if (!rtt_ms)
        rtt_ms = 1;

    pacing = rate + div_u64(rate * HEADROOM_PERMILLE, 1000);
    pacing = min_t(u64, pacing, max_rate);

    cwnd = div_u64(pacing, MSEC_PER_SEC);
    cwnd *= rtt_ms;
    cwnd = div_u64(cwnd, mss);
    cwnd *= mundo->cwnd_gain;
    cwnd = div_u64(cwnd, 10);
    cwnd = max_t(u64, cwnd, MIN_CWND);
    cwnd = min_t(u64, cwnd, tp->snd_cwnd_clamp);

    mundo_tcp_snd_cwnd_set(tp, (u32)cwnd);

    max_pacing = READ_ONCE(sk->sk_max_pacing_rate);
    if (max_pacing)
        pacing = min_t(u64, pacing, max_pacing);
    WRITE_ONCE(sk->sk_pacing_rate, pacing);
}

static void mundo_raise_rate(struct mundo *mundo, u64 max_rate)
{
    u64 step;

    if (mundo->rate >= max_rate)
        return;

    step = mundo->rate >> 1;
    step = max_t(u64, step, MIN_PROBE_STEP);
    mundo->rate += min_t(u64, step, max_rate - mundo->rate);
}

static u32 mundo_rate_unit(u64 rate)
{
    rate = div_u64(rate, 1024);
    return (u32)min_t(u64, rate, U32_MAX);
}

static u64 mundo_unit_rate(u32 unit)
{
    return (u64)unit * 1024;
}

static u64 mundo_delivery_ceiling(u32 delivery)
{
    u64 rate = mundo_unit_rate(delivery);

    return rate + div_u64(rate * DELIVERY_HEADROOM_PERMILLE, 1000);
}

static void mundo_reduce_rate(struct mundo *mundo, u64 ceiling, u64 max_rate)
{
    mundo->rate = min_t(u64, max_t(u64, ceiling, MIN_PACING_RATE), max_rate);
}

static void mundo_update_model(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct mundo *mundo = inet_csk_ca(sk);
    u64 active_acked = 0;
    u64 delivery_rate;
    u64 delivery_ceiling;
    u64 max_rate = mundo_max_rate(mundo);
    u64 target = min_t(u64, mundo->rate, max_rate);
    u32 mss = tp->mss_cache;
    u32 delivery;
    u32 peak_growth;
    u8 active_slots = 0;
    bool plateau;

    if (mss < FALLBACK_MSS)
        mss = FALLBACK_MSS;

    for (int i = 0; i < PKT_INFO_SLOTS; i++)
    {
        if (mundo->active_mask & (1U << i))
        {
            active_slots++;
            active_acked += mundo->slots[i];
        }
    }

    if (!active_slots || active_acked < MIN_PKT_INFO_SAMPLES)
    {
        mundo_apply_rate(sk);
        return;
    }

    delivery_rate = div_u64(active_acked * mss, active_slots);
    delivery_ceiling = delivery_rate + div_u64(delivery_rate * DELIVERY_HEADROOM_PERMILLE, 1000);
    delivery = mundo_rate_unit(delivery_rate);
    if (delivery > mundo->peak_delivery)
        mundo->peak_delivery = delivery;

    peak_growth = max_t(u32, div_u64((u64)mundo->peak_delivery * DELIVERY_FLAT_PERMILLE, 1000), 16);
    plateau = delivery_ceiling > MIN_PACING_RATE &&
              target > delivery_ceiling &&
              (target - delivery_ceiling) >
                  div_u64(target * DELIVERY_LOW_PERMILLE, 1000) &&
              mundo->peak_delivery &&
              delivery <= mundo->peak_delivery + peak_growth;

    if (plateau)
    {
        if (mundo->bad_rounds < BAD_CONFIRM_ROUNDS)
            mundo->bad_rounds++;
        if (mundo->bad_rounds >= BAD_CONFIRM_ROUNDS)
        {
            mundo->peak_delivery = delivery;
            mundo_reduce_rate(mundo, mundo_delivery_ceiling(delivery), max_rate);
            mundo->bad_rounds = 0;
        }
    }
    else
    {
        mundo->bad_rounds = 0;
        mundo_raise_rate(mundo, max_rate);
    }

    mundo_apply_rate(sk);
}

static void mundo_advance_window(struct mundo *mundo, u32 sec)
{
    u32 elapsed;

    if (mundo->last_sec == (u32)~0U)
    {
        mundo->last_sec = sec;
        return;
    }

    elapsed = sec - mundo->last_sec;
    if (!elapsed)
        return;

    if (elapsed >= PKT_INFO_SLOTS)
    {
        memset(mundo->slots, 0, sizeof(mundo->slots));
        mundo->slot = 0;
        mundo->active_mask = 0;
    }
    else
    {
        while (elapsed--)
        {
            mundo->slot++;
            if (mundo->slot >= PKT_INFO_SLOTS)
                mundo->slot = 0;
            mundo->slots[mundo->slot] = 0;
            mundo->active_mask &= ~(1U << mundo->slot);
        }
    }

    mundo->last_sec = sec;
}

static void mundo_add_sample(u32 *dst, int value)
{
    u32 add;

    if (value <= 0)
        return;
    add = (u32)value;
    if (*dst > U32_MAX - add)
        *dst = U32_MAX;
    else
        *dst += add;
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
    u64 max_rate;

    if (optlen < sizeof(params))
        return -EINVAL;

#ifdef _LINUX_SOCKPTR_H
    if (copy_from_sockptr(&params, optval, sizeof(params)))
        return -EFAULT;
#else
    if (copy_from_user(&params, optval, sizeof(params)))
        return -EFAULT;
#endif

    if (params.rate < MIN_PACING_RATE || params.rate > MAX_PACING_RATE)
        return -EINVAL;
    if (params.cwnd_gain < MIN_CWND_GAIN || params.cwnd_gain > MAX_CWND_GAIN)
        return -EINVAL;

    if (!mundo->peer)
    {
        if (sk->sk_state != TCP_ESTABLISHED)
            return -ENOTCONN;
        peer = mundo_peer_get(sk, params.rate);
        if (!peer)
            return -ENOMEM;
        mundo->peer = peer;
        mundo_peer_set_active(mundo, tcp_packets_in_flight(tcp_sk(sk)) != 0);
    }
    else
    {
        mundo_peer_limit(mundo->peer, params.rate);
    }
    if (!inet_csk(sk)->icsk_user_timeout)
        inet_csk(sk)->icsk_user_timeout = MUNDO_USER_TIMEOUT;

    max_rate = mundo_max_rate(mundo);
    mundo->cwnd_gain = params.cwnd_gain;
    if (mundo->rate > max_rate)
        mundo->rate = max_rate;
    else if (mundo->rate <= INIT_PACING_RATE && max_rate > DIRECT_START_RATE)
        mundo->rate = max_t(u64, INIT_PACING_RATE, div_u64(max_rate, FAST_START_DIV));
    else if (mundo->rate < MIN_PACING_RATE)
        mundo->rate = min_t(u64, INIT_PACING_RATE, max_rate);

    mundo_apply_rate(sk);
    return 0;
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

#ifdef _TRANSP_V6_H
#ifdef _LINUX_SOCKPTR_H
static int mundo_tcpv6_setsockopt(struct sock *sk, int level, int optname, sockptr_t optval, unsigned int optlen)
#else
static int mundo_tcpv6_setsockopt(struct sock *sk, int level, int optname, char __user *optval, unsigned int optlen)
#endif
{
    if (level == IPPROTO_TCP && optname == TCP_BRUTAL_PARAMS)
        return mundo_set_params(sk, optval, optlen);
    return tcpv6_prot.setsockopt(sk, level, optname, optval, optlen);
}
#endif

static void mundo_init(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct mundo *mundo = inet_csk_ca(sk);

    if (sk->sk_family == AF_INET)
        sk->sk_prot = &tcp_prot_override;
#ifdef _TRANSP_V6_H
    else if (sk->sk_family == AF_INET6)
        sk->sk_prot = &tcpv6_prot_override;
#endif
    else
        BUG();

    tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;

    memset(mundo, 0, sizeof(*mundo));
    mundo->rate = INIT_PACING_RATE;
    mundo->last_sec = U32_MAX;
    mundo->cwnd_gain = INIT_CWND_GAIN;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
#endif

    mundo_apply_rate(sk);
}

static void mundo_release(struct sock *sk)
{
    struct mundo *mundo = inet_csk_ca(sk);

    if (mundo->peer)
    {
        mundo_peer_set_active(mundo, false);
        mundo_peer_put(mundo->peer);
    }
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
    u32 sec = (u32)tcp_sock_get_sec(tp);
    u32 mss = tp->mss_cache;

    mundo_peer_set_active(mundo, tcp_packets_in_flight(tp) != 0);
    if (rs->delivered < 0 || rs->interval_us <= 0)
        return;

    if (mundo->last_sec != (u32)~0U && sec != mundo->last_sec &&
        sec - mundo->last_sec < PKT_INFO_SLOTS)
        mundo_update_model(sk);
    mundo_advance_window(mundo, sec);

    mundo_add_sample(&mundo->slots[mundo->slot], rs->acked_sacked);
    if (!rs->is_app_limited)
    {
        mundo->active_mask |= 1U << mundo->slot;
    }
    else if (mundo->peak_delivery)
    {
        if (mss < FALLBACK_MSS)
            mss = FALLBACK_MSS;
        if (mundo_rate_unit((u64)mundo->slots[mundo->slot] * mss) >=
            mundo->peak_delivery / APP_LIMITED_PROBE_DIV)
            mundo->active_mask |= 1U << mundo->slot;
    }
}

static u32 mundo_undo_cwnd(struct sock *sk)
{
    return mundo_tcp_snd_cwnd(tcp_sk(sk));
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
    BUILD_BUG_ON(PKT_INFO_SLOTS < 3);

    tcp_prot_override = tcp_prot;
    tcp_prot_override.setsockopt = mundo_tcp_setsockopt;

#ifdef _TRANSP_V6_H
    tcpv6_prot_override = tcpv6_prot;
    tcpv6_prot_override.setsockopt = mundo_tcpv6_setsockopt;
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
