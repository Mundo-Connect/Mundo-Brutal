/* Userspace test scaffolding for the actual mundo.c; never built into mundo.ko. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <netinet/in.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef uint32_t __be32;
typedef uint16_t __be16;
#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#ifndef LINUX_VERSION_CODE
#define LINUX_VERSION_CODE KERNEL_VERSION(6,18,0)
#endif
#define CONFIG_IPV6 1
#define CONFIG_TLS 1
#define IS_ENABLED(x) (x)
#define _TRANSP_V6_H
#define _LINUX_SOCKPTR_H
#define __ro_after_init
#define __packed __attribute__((packed))
#define __init
#define __exit
#define __user
#define THIS_MODULE NULL
#define MODULE_AUTHOR(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_VERSION(x)
#define module_init(x)
#define module_exit(x)
#define TCP_CONG_NON_RESTRICTED 1
#define TCP_INFINITE_SSTHRESH 0x7fffffff
#define TCP_INIT_CWND 10
#define TCP_ESTABLISHED 1
#define ICSK_CA_PRIV_SIZE 104
#define USEC_PER_SEC 1000000U
#define USEC_PER_MSEC 1000U
#define MSEC_PER_SEC 1000U
#define SK_PACING_NONE 0
#define SK_PACING_NEEDED 1
#define U32_MAX UINT32_MAX
#define GFP_KERNEL 0
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define WRITE_ONCE(x,y) __atomic_store_n(&(x),(y),__ATOMIC_RELAXED)
#define cmpxchg(p,old,newval) __sync_val_compare_and_swap(p,old,newval)
#define min_t(t,a,b) ({t _a=(a),_b=(b);_a<_b?_a:_b;})
#define max_t(t,a,b) ({t _a=(a),_b=(b);_a>_b?_a:_b;})
#define min(a,b) min_t(__typeof__(a),a,b)
#define max(a,b) max_t(__typeof__(a),a,b)
#define clamp_t(t,x,lo,hi) min_t(t,max_t(t,x,lo),hi)
#define BUILD_BUG_ON(x) _Static_assert(!(x), #x)
#define WARN_ON_ONCE(x) assert(!(x))
#define BUG() abort()
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#include <stddef.h>
typedef _Atomic u64 atomic64_t;
typedef _Atomic int atomic_t;
#define atomic64_set(p,x) atomic_store(p,x)
#define atomic64_read(p) atomic_load(p)
#define atomic_set(p,x) atomic_store(p,x)
#define atomic_read(p) atomic_load(p)
#define atomic_inc(p) atomic_fetch_add(p,1)
#define atomic_dec(p) atomic_fetch_sub(p,1)
#define DEFINE_SPINLOCK(n) pthread_mutex_t n = PTHREAD_MUTEX_INITIALIZER
#define spin_lock_bh(p) assert(!pthread_mutex_lock(p))
#define spin_unlock_bh(p) assert(!pthread_mutex_unlock(p))
static u64 div_u64(u64 a,u32 b) {return a/b;}
static u64 mul_u64_u32_div(u64 a,u32 b,u32 c) {return (unsigned __int128)a*b/c;}

struct hlist_node {struct hlist_node *next,**pprev;};
struct hlist_head {struct hlist_node *first;};
#define DEFINE_HASHTABLE(n,b) struct hlist_head n[1<<(b)]
#define HASH_SIZE(n) (sizeof(n)/sizeof((n)[0]))
#define hash_for_each_possible(h,p,m,k) \
    for(struct hlist_node *_n=(h)[(k)&(HASH_SIZE(h)-1)].first; \
        _n && ((p)=container_of(_n,__typeof__(*(p)),m),1); _n=_n->next)
#define hash_add(h,n,k) do { \
    struct hlist_node **_p=&(h)[(k)&(HASH_SIZE(h)-1)].first; \
    (n)->next=*_p;(n)->pprev=_p;if(*_p)(*_p)->pprev=&(n)->next;*_p=(n); \
} while(0)
static void hash_del(struct hlist_node *n) { *n->pprev=n->next;if(n->next)n->next->pprev=n->pprev; }
static u32 jhash(const void *v,size_t n,u32 seed) {
    const unsigned char *p=v;u32 h=seed;while(n--)h=(h^*p++)*16777619U;return h;
}
static atomic_int allocations, fail_alloc;
static void *kmalloc(size_t n,int flags) {
    (void)flags;if(atomic_load(&fail_alloc))return NULL;
    void *p=malloc(n);if(p)atomic_fetch_add(&allocations,1);return p;
}
static void kfree(void *p) {if(p){atomic_fetch_sub(&allocations,1);free(p);}}

struct net {int id;};
struct sock;
typedef const void *sockptr_t;
struct proto {int (*setsockopt)(struct sock *,int,int,sockptr_t,unsigned int);};
struct tcp_sock {
    u64 tcp_mstamp;
    u32 delivered,mss_cache,srtt_us,snd_cwnd,snd_cwnd_clamp,snd_ssthresh,inflight;
};
struct inet_sock {__be32 inet_daddr,inet_saddr;__be16 inet_sport;};
struct rate_sample {int delivered;long interval_us;u32 acked_sacked;bool is_app_limited;};
enum tcp_ca_event {CA_EVENT_LOSS,CA_EVENT_TX_START};
struct tcp_congestion_ops {
    unsigned int flags;char name[16];void *owner;
    void (*init)(struct sock *),(*release)(struct sock *);
    void (*cwnd_event)(struct sock *,enum tcp_ca_event);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,10,0)
    void (*cong_control)(struct sock *,u32,int,const struct rate_sample *);
#else
    void (*cong_control)(struct sock *,const struct rate_sample *);
#endif
    u32 (*undo_cwnd)(struct sock *),(*ssthresh)(struct sock *);
};
struct ulp_ops {char name[16];};
struct inet_connection_sock {
    struct tcp_congestion_ops *icsk_ca_ops;
    struct ulp_ops *icsk_ulp_ops;
    u32 icsk_user_timeout;
    union {max_align_t align;char bytes[ICSK_CA_PRIV_SIZE];} ca;
};
struct tls_context {struct proto *sk_proto;};
struct sock {
    struct proto *sk_prot;
    int sk_family,sk_state,sk_pacing_status;
    u64 sk_pacing_rate,sk_max_pacing_rate;
    struct in6_addr sk_v6_daddr,sk_v6_rcv_saddr;
    pthread_mutex_t lock;
    struct tcp_sock tcp;
    struct inet_sock inet;
    struct inet_connection_sock icsk;
    struct tls_context *tls;
    struct net *net;
};
static struct proto tcp_prot,tcpv6_prot;
static struct tcp_sock *tcp_sk(struct sock *sk) {return &sk->tcp;}
static struct inet_sock *inet_sk(struct sock *sk) {return &sk->inet;}
static struct net *sock_net(struct sock *sk) {return sk->net;}
static struct inet_connection_sock *inet_csk(struct sock *sk) {return &sk->icsk;}
static void *inet_csk_ca(struct sock *sk) {return sk->icsk.ca.bytes;}
static u32 tcp_packets_in_flight(const struct tcp_sock *tp) {return tp->inflight;}
static u32 tcp_mss_to_mtu(struct sock *sk,u32 mss) {(void)sk;return mss+40;}
static void lock_sock(struct sock *sk) {assert(!pthread_mutex_lock(&sk->lock));}
static void release_sock(struct sock *sk) {assert(!pthread_mutex_unlock(&sk->lock));}
static struct tls_context *tls_get_ctx(struct sock *sk) {return sk->tls;}
static int copy_from_sockptr(void *out,const void *in,size_t n) {
    if(!in)return -1;
    memcpy(out,in,n);return 0;
}
static int tcp_register_congestion_control(struct tcp_congestion_ops *ops) {(void)ops;return 0;}
static void tcp_unregister_congestion_control(struct tcp_congestion_ops *ops) {(void)ops;}
