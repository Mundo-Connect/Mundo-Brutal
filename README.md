# Mundo X Brutal

Adaptive TCP congestion control for Mundo Connect. Algorithm name: `brutal`.

- Website: <https://668993.xyz>
- Telegram: <https://t.me/mconnectofficial>
- License: MIT

## Install

```sh
sh install.sh
```

Manual build requires matching headers for the running kernel (Linux 4.9+):

```sh
make
sudo insmod mundo.ko
```

The IPv6 socket option requires Linux 5.8+ with `CONFIG_IPV6`.

## Usage

Select `brutal` with `TCP_CONGESTION`, then set `TCP_BRUTAL_PARAMS = 23301`
using this packed 12-byte value:

```c
struct mundo_params {
    uint64_t rate;      /* Negotiated maximum send rate, bytes/second */
    uint32_t cwnd_gain; /* Window gain in tenths; 18 means 1.8x */
} __attribute__((packed));
```

Active server connections sharing `(client IP, server IP, server port)` share
the ceiling. Suspended connections yield their share. Without parameters, the
algorithm probes automatically per connection.

```sh
python3 example/server.py --server-max-mbps 500 -p 1234
python3 example/client.py example.com 100 -p 1234
```
