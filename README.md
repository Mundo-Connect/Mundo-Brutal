# Mundo X Brutal

Mundo X Brutal is a Linux TCP congestion-control module derived from the TCP
Brutal ABI but with adaptive bandwidth probing.

Mundo X Brutal is part of the Mundo Connect Project, the world-engine project
for connecting the world.

- Website: <https://668993.xyz>
- Telegram: <https://t.me/mconnectofficial>
- License: MIT License

It registers a congestion-control algorithm named `brutal`. Applications enable
the Mundo X Brutal congestion control with `TCP_CONGESTION`, then pass the
existing TCP Brutal-compatible sockopt `TCP_BRUTAL_PARAMS = 23301`.

The syscall/sockopt code is compatible with TCP Brutal:

```c
#define TCP_BRUTAL_PARAMS 23301
```

Unlike TCP Brutal, `rate` is interpreted as the negotiated maximum send rate,
not the fixed send rate. The application should negotiate the minimum of the
server limit and the client limit, then pass that value to Mundo X Brutal.

On established server-side sockets, the negotiated rate is shared by the
three-tuple `(client IP, server IP, server port)`. The client source port is
deliberately excluded, so parallel TCP connections from the same endpoint share
one aggregate limit instead of multiplying it. Each connection keeps its own
RTT and delivery model; only the configured maximum is shared and split
between connections that currently have data in flight. Idle connections keep
their tuple reference but consume no bandwidth share. The lowest limit seen
during a tuple's lifetime remains in effect until its last connection closes.

Installing `TCP_BRUTAL_PARAMS` also supplies a two-minute TCP user timeout when
the application has not configured one. A connection with persistently
unacknowledged data is therefore closed by TCP's existing retransmission timer
instead of retaining its tuple and bandwidth share for the global
`tcp_retries2` lifetime. Idle connections with no outstanding data are
unaffected.

This address-level identity intentionally treats clients behind the same public
NAT address as one endpoint. Applications that need account-level isolation
must continue to enforce that policy above TCP.

Mundo X Brutal is intended to run on the sending side, typically the server
side. The client can remain a normal TCP receiver with its ordinary
congestion-control configuration.

```c
struct mundo_params {
    uint64_t rate;      /* maximum send rate, bytes per second */
    uint32_t cwnd_gain; /* tenths, 15 means 1.5x */
} __attribute__((packed));
```

## Algorithm

Mundo X Brutal keeps a small per-connection sliding window of delivered ACKs:

- only non app-limited samples participate in peak-bandwidth probing;
- app-limited samples that are still large enough to stress the link may also
  participate, so medium downloads can detect a lower client-side bottleneck;
- peak bandwidth is inferred when a higher send target no longer produces
  meaningful ACK-throughput growth;
- idle or low-demand periods do not reset the learned target rate, so later
  bursts can start near the previous estimate instead of climbing from 100 Kbps.

Every second it checks delivered ACK throughput and changes the internal target
rate:

- available delivery headroom: probe upward with large early steps;
- one short spike: hold rate;
- sustained ACK-throughput plateau: reduce rate;
- pacing rate: internal target plus small filtered-loss headroom, capped by the
  negotiated maximum.

This is intentionally not BBR. Mundo X Brutal does not estimate BtlBw/RTprop or
run a BBR state machine. It is closer to an adaptive Brutal-style pacer:
aggressive, bounded by the negotiated maximum, and filtered so short network
jitter is not treated as a real client bandwidth drop.

The ACK path uses a cached pointer and never looks up the tuple table. TCP's
existing transmit-start event tracks active connections without timers or
scans. One small entry is allocated per live tuple; the table is touched only
when socket parameters are installed or a connection is released, and the
entry is freed immediately after its final connection.

## Build

```sh
make
sudo insmod mundo.ko
```

The module requires headers for the kernel that is currently running, not the
newest kernel available from the distribution. Linux 4.9+ is the intended
kernel API compatibility floor across old and new distribution kernels. IPv6
support requires Linux 5.8+ with `CONFIG_IPV6`, matching the exported
`tcpv6_prot` requirement used by TCP Brutal.

## One-Key Install

```sh
sh install.sh
```

The installer attempts to install `clang`, `make`, `gcc`, `dkms`, and matching
kernel headers/dev packages for Debian, Ubuntu, Alpine, RHEL/CentOS/Rocky,
Fedora, Arch, openSUSE, and Void Linux families. It first checks whether the
build tools and `/lib/modules/$(uname -r)/build` already exist. It prefers DKMS
so the module can rebuild after kernel updates, writes
`/etc/modules-load.d/mundo.conf`, runs `depmod`, and loads the module.

Useful modes:

```sh
sh install.sh install
sh install.sh repair
sh install.sh reinstall
sh install.sh uninstall
```

`repair` and `reinstall` both rebuild the module for the running kernel, restore
boot autoload, and reload `mundo`.

## Example

```sh
python3 example/server.py --server-max-mbps 500 -p 1234
python3 example/client.py example.com 100 -p 1234
```

The example negotiates `min(server_max_mbps, client_limit_mbps)` and passes that
as `TCP_BRUTAL_PARAMS.rate`.
