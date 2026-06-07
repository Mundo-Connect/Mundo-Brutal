# Mundo X Brutal

Mundo X Brutal is a Linux TCP congestion-control module derived from the TCP
Brutal ABI but with adaptive bandwidth probing.

Mundo X Brutal is part of the Mundo Connect Project, the world-engine project
for connecting the world.

- Website: <https://668993.xyz>
- Telegram: <https://t.me/mconnectofficial>
- License: MIT License

It registers a congestion-control algorithm named `mundo`. Applications enable
the Mundo X Brutal congestion control with `TCP_CONGESTION`, then pass the
existing TCP Brutal-compatible sockopt `TCP_BRUTAL_PARAMS = 23301`.

The syscall/sockopt code is compatible with TCP Brutal:

```c
#define TCP_BRUTAL_PARAMS 23301
```

Unlike TCP Brutal, `rate` is interpreted as the negotiated maximum send rate,
not the fixed send rate. The application should negotiate the minimum of the
server limit and the client limit, then pass that value to Mundo X Brutal.

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

Mundo X Brutal keeps a small per-connection sliding window of ACK/loss samples.
It treats packet loss and peak bandwidth as separate signals:

- all samples, including low-demand periods, update the filtered loss baseline;
- only non app-limited samples participate in peak-bandwidth probing;
- app-limited samples that are still large enough to stress the link may also
  participate, so medium downloads can detect a lower client-side bottleneck;
- peak bandwidth is inferred when a higher send target no longer produces
  meaningful ACK-throughput growth;
- idle or low-demand periods do not reset the learned target rate, so later
  bursts can start near the previous estimate instead of climbing from 100 Kbps.

Every second it filters loss with an EWMA, cross-checks delivered ACK throughput,
and changes the internal target rate:

- low filtered loss: probe upward with large early steps, then taper near the
  negotiated limit;
- one short spike: hold rate;
- confirmed high filtered loss or sustained ACK-throughput plateau: reduce rate;
- pacing rate: internal target plus small filtered-loss headroom, capped by the
  negotiated maximum.

This is intentionally not BBR. Mundo X Brutal does not estimate BtlBw/RTprop or
run a BBR state machine. It is closer to an adaptive Brutal-style pacer:
aggressive, bounded by the negotiated maximum, and filtered so short network
jitter is not treated as a real client bandwidth drop.

The kernel module uses only the TCP congestion-control private area. There is no
per-connection dynamic allocation; connection state is released with the TCP
socket.

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
