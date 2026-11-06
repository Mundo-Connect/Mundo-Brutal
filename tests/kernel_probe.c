/* Run inside a disposable Linux VM; never load an experimental CC on a host
 * serving real traffic. Build: cc -O2 -static -pthread kernel_probe.c -o probe
 * Usage: probe lifecycle | probe transfer SECONDS FLOWS CAP_MBPS [ipv6]
 */
#define _GNU_SOURCE
#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <linux/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PARAMS 23301
enum {TCP_CLOSED_STATE=7};
struct params {uint64_t rate;uint32_t gain;} __attribute__((packed));
struct flow {int sender,receiver;uint64_t bytes;unsigned retrans;};
static atomic_int done;

static double now(void)
{
    struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return t.tv_sec+t.tv_nsec/1e9;
}

static int listener(int family, struct sockaddr_storage *addr, socklen_t *size)
{
    const char *ns=getenv("MUNDO_TEST_NS");
    int original=-1;
    if(ns)
    {
        int target=open(ns,O_RDONLY);original=open("/proc/self/ns/net",O_RDONLY);
        assert(target>=0 && original>=0 && !setns(target,CLONE_NEWNET));close(target);
    }
    int fd=socket(family,SOCK_STREAM,0),one=1,mss=1200;
    assert(fd>=0);
    assert(!setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one)));
    assert(!setsockopt(fd,IPPROTO_TCP,TCP_MAXSEG,&mss,sizeof(mss)));
    memset(addr,0,sizeof(*addr));
    if(family==AF_INET)
    {
        struct sockaddr_in *v=(void *)addr;
        v->sin_family=family;v->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(ns)assert(inet_pton(family,"192.0.2.2",&v->sin_addr)==1);
        *size=sizeof(*v);
    }
    else
    {
        struct sockaddr_in6 *v=(void *)addr;
        v->sin6_family=family;v->sin6_addr=in6addr_loopback;
        if(ns)assert(inet_pton(family,"fd00:4d54::2",&v->sin6_addr)==1);
        *size=sizeof(*v);
    }
    assert(!bind(fd,(void *)addr,*size));
    assert(!getsockname(fd,(void *)addr,size));
    assert(!listen(fd,128));
    if(original>=0){assert(!setns(original,CLONE_NEWNET));close(original);}
    return fd;
}

static struct flow connect_pair(int fd,struct sockaddr_storage *addr,socklen_t size,uint64_t cap,int rcvbuf)
{
    struct flow f={.receiver=socket(addr->ss_family,SOCK_STREAM,0)};
    int mss=1200;
    assert(f.receiver>=0);
    if(rcvbuf)assert(!setsockopt(f.receiver,SOL_SOCKET,SO_RCVBUF,&rcvbuf,sizeof(rcvbuf)));
    assert(!setsockopt(f.receiver,IPPROTO_TCP,TCP_MAXSEG,&mss,sizeof(mss)));
    assert(!connect(f.receiver,(void *)addr,size));
    f.sender=accept(fd,NULL,NULL);assert(f.sender>=0);
    int no_delay=1; /* Match Go's net.TCPConn default used by the proxy. */
    assert(!setsockopt(f.sender,IPPROTO_TCP,TCP_NODELAY,&no_delay,sizeof(no_delay)));
    const char *algorithm=getenv("MUNDO_CC");
    if(!algorithm)algorithm="brutal";
    assert(!setsockopt(f.sender,IPPROTO_TCP,TCP_CONGESTION,algorithm,strlen(algorithm)));
    if(!strcmp(algorithm,"brutal"))
    {
        struct params p={cap,18};
        assert(!setsockopt(f.sender,IPPROTO_TCP,PARAMS,&p,sizeof(p)));
    }
    return f;
}

static void *send_data(void *arg)
{
    struct flow *f=arg;
    char data[16384];memset(data,0x5a,sizeof(data));
    while(!atomic_load(&done))
    {
        ssize_t n=send(f->sender,data,sizeof(data),MSG_NOSIGNAL);
        if(n<=0)break;
    }
    return NULL;
}

static void *receive_data(void *arg)
{
    struct flow *f=arg;char data[16384];
    while(!atomic_load(&done))
    {
        ssize_t n=read(f->receiver,data,sizeof(data));
        if(n<=0)break;
        assert(data[0]==0x5a && data[n-1]==0x5a);
        f->bytes+=n;
    }
    return NULL;
}

static void transfer(int seconds,int count,uint64_t cap,int family)
{
    struct flow f[16];pthread_t sends[16],receives[16];
    struct sockaddr_storage addr;socklen_t size;
    assert(count>0 && count<=16);
    int fd=listener(family,&addr,&size);
    for(int i=0;i<count;i++)f[i]=connect_pair(fd,&addr,size,cap,0);
    close(fd);
    double start=now();
    for(int i=0;i<count;i++)
    {
        assert(!pthread_create(&receives[i],NULL,receive_data,&f[i]));
        assert(!pthread_create(&sends[i],NULL,send_data,&f[i]));
    }
    if(getenv("TRACE"))
    {
        uint64_t previous=0;
        for(int second=1;second<=seconds;second++)
        {
            sleep(1);
            uint64_t acked=0;struct tcp_info info;
            for(int i=0;i<count;i++)
            {
                socklen_t n=sizeof(info);memset(&info,0,sizeof(info));
                assert(!getsockopt(f[i].sender,IPPROTO_TCP,TCP_INFO,&info,&n));
                acked+=info.tcpi_bytes_acked;
            }
            printf("t=%d ack=%.2f Mbps pace=%.2f Mbps rtt=%.1f ms cwnd=%u unacked=%u rwnd=%u notsent=%u\n",
                   second,(acked-previous)*8/1e6,info.tcpi_pacing_rate*8/1e6,
                   info.tcpi_rtt/1000.0,info.tcpi_snd_cwnd,info.tcpi_unacked,
                   info.tcpi_snd_wnd,info.tcpi_notsent_bytes);
            fflush(stdout);previous=acked;
        }
    }
    else sleep(seconds);
    atomic_store(&done,1);
    for(int i=0;i<count;i++)
    {
        shutdown(f[i].sender,SHUT_RDWR);
        shutdown(f[i].receiver,SHUT_RDWR);
    }
    uint64_t bytes=0;unsigned retrans=0;
    for(int i=0;i<count;i++)
    {
        assert(!pthread_join(sends[i],NULL));
        assert(!pthread_join(receives[i],NULL));
        struct tcp_info info;socklen_t n=sizeof(info);
        assert(!getsockopt(f[i].sender,IPPROTO_TCP,TCP_INFO,&info,&n));
        retrans+=info.tcpi_total_retrans;bytes+=f[i].bytes;
        struct linger abort_test={1,0};
        assert(!setsockopt(f[i].sender,SOL_SOCKET,SO_LINGER,&abort_test,sizeof(abort_test)));
        assert(!setsockopt(f[i].receiver,SOL_SOCKET,SO_LINGER,&abort_test,sizeof(abort_test)));
        close(f[i].sender);close(f[i].receiver);
    }
    struct rusage usage;assert(!getrusage(RUSAGE_SELF,&usage));
    double cpu=usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6+
               usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6;
    printf("transfer ipv%d flows=%d cap=%.0f Mbps delivered=%.3f Mbps bytes=%llu retrans=%u cpu=%.3f s rss=%ld KiB elapsed=%.3f s\n",
           family==AF_INET?4:6,count,cap*8/1e6,bytes*8/(now()-start)/1e6,
           (unsigned long long)bytes,retrans,cpu,usage.ru_maxrss,now()-start);
}

static void *switcher(void *arg)
{
    int fd=*(int *)arg;
    for(int i=0;i<2000;i++)
    {
        assert(!setsockopt(fd,IPPROTO_TCP,TCP_CONGESTION,"cubic",5));
        assert(!setsockopt(fd,IPPROTO_TCP,TCP_CONGESTION,"brutal",6));
    }
    return NULL;
}

static void lifecycle(void)
{
    for(int family=AF_INET;family<=AF_INET6;family+=AF_INET6-AF_INET)
    {
        struct sockaddr_storage addr;socklen_t size;
        int fd=listener(family,&addr,&size);
        for(int i=0;i<1000;i++)
        {
            struct flow f=connect_pair(fd,&addr,size,12500000,0);
            if(i&1)assert(!setsockopt(f.sender,IPPROTO_TCP,TCP_ULP,"tls",3));
            assert(!setsockopt(f.sender,IPPROTO_TCP,TCP_CONGESTION,"cubic",5));
            int one=1;
            assert(!setsockopt(f.sender,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one)));
            assert(write(f.sender,"ok",2)==2);
            char b[2];assert(recv(f.receiver,b,2,MSG_WAITALL)==2 && !memcmp(b,"ok",2));
            close(f.sender);close(f.receiver);
        }
        struct flow f=connect_pair(fd,&addr,size,12500000,0);
        pthread_t thread;assert(!pthread_create(&thread,NULL,switcher,&f.sender));
        struct params p={12500000,18};
        for(int i=0;i<20000;i++)
        {
            int err=setsockopt(f.sender,IPPROTO_TCP,PARAMS,&p,sizeof(p));
            assert(!err || errno==ENOPROTOOPT);
        }
        assert(!pthread_join(thread,NULL));
        close(f.sender);close(f.receiver);close(fd);
    }
    puts("lifecycle: IPv4/IPv6, kTLS, concurrent CA switches passed");
}

static void suspended(void)
{
    struct sockaddr_storage addr;socklen_t size;
    int fd=listener(AF_INET,&addr,&size),timeout;
    struct flow paused=connect_pair(fd,&addr,size,12500000,4096);
    struct flow live=connect_pair(fd,&addr,size,12500000,0);
    close(fd);
    size=sizeof(timeout);
    assert(!getsockopt(paused.sender,IPPROTO_TCP,TCP_USER_TIMEOUT,&timeout,&size));
    assert(timeout==120000);
    pthread_t ps,pr,ls,lr;
    assert(!pthread_create(&ps,NULL,send_data,&paused));
    assert(!pthread_create(&ls,NULL,send_data,&live));
    assert(!pthread_create(&lr,NULL,receive_data,&live));
    sleep(5);
    struct tcp_info info;size=sizeof(info);
    assert(!getsockopt(paused.sender,IPPROTO_TCP,TCP_INFO,&info,&size));
    printf("suspended TCP window=%u unacked=%u lost=%u retrans=%u\n",
           info.tcpi_snd_wnd,info.tcpi_unacked,info.tcpi_lost,info.tcpi_total_retrans);
    /* Sender SWS avoidance may leave a sub-MSS window instead of exact zero. */
    assert(info.tcpi_snd_wnd<info.tcpi_snd_mss && info.tcpi_notsent_bytes>0);
    FILE *f=fopen("/sys/module/mundo/parameters/audit_active","r");
    if(f)
    {
        unsigned active;assert(fscanf(f,"%u",&active)==1);fclose(f);
        assert(active<=1);printf("suspended tuple active=%u\n",active);
    }
    assert(!pthread_create(&pr,NULL,receive_data,&paused));
    sleep(4);atomic_store(&done,1);
    shutdown(paused.sender,SHUT_RDWR);shutdown(live.sender,SHUT_RDWR);
    shutdown(paused.receiver,SHUT_RDWR);shutdown(live.receiver,SHUT_RDWR);
    assert(!pthread_join(ps,NULL));assert(!pthread_join(pr,NULL));
    assert(!pthread_join(ls,NULL));assert(!pthread_join(lr,NULL));
    assert(paused.bytes>65536 && live.bytes>1000000);
    close(paused.sender);close(paused.receiver);close(live.sender);close(live.receiver);

    atomic_store(&done,0);size=sizeof(addr);fd=listener(AF_INET,&addr,&size);
    paused=connect_pair(fd,&addr,size,12500000,4096);close(fd);
    timeout=1500;
    assert(!setsockopt(paused.sender,IPPROTO_TCP,TCP_USER_TIMEOUT,&timeout,sizeof(timeout)));
    assert(!pthread_create(&ps,NULL,send_data,&paused));
    for(int i=0;i<10;i++)
    {
        sleep(1);size=sizeof(info);
        assert(!getsockopt(paused.sender,IPPROTO_TCP,TCP_INFO,&info,&size));
        if(info.tcpi_state==TCP_CLOSED_STATE)break;
    }
    assert(info.tcpi_state==TCP_CLOSED_STATE);
    atomic_store(&done,1);shutdown(paused.sender,SHUT_RDWR);
    assert(!pthread_join(ps,NULL));close(paused.sender);close(paused.receiver);
    puts("suspend/resume, shared bandwidth and native user timeout passed");
}

int main(int argc,char **argv)
{
    signal(SIGPIPE,SIG_IGN);alarm(180);
    if(argc==2 && !strcmp(argv[1],"lifecycle"))lifecycle();
    else if(argc==2 && !strcmp(argv[1],"suspended"))suspended();
    else if(argc>=5)transfer(atoi(argv[2]),atoi(argv[3]),strtoull(argv[4],NULL,10)*125000,
                            argc>5?AF_INET6:AF_INET);
    else {fprintf(stderr,"probe lifecycle | probe transfer SECONDS FLOWS CAP_MBPS [ipv6]\n");return 2;}
    return 0;
}
