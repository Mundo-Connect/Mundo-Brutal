#!/bin/sh
# Run only in a disposable VM with mundo.ko loaded. Requires iproute2 and root.
# MUNDO_PROBE may select an already compiled tests/kernel_probe.c.
set -eu

test_dir=$(mktemp -d)
test_ifb="mt$$_ifb"
test_link="mt$$_a"
test_peer="mt$$_b"
test_ns="mt$$"
owns_link=0
owns_ns=0
owns_ifb=0
cleanup()
{
    if [ "$owns_link" = 1 ]; then ip link del "$test_link"; fi
    if [ "$owns_ifb" = 1 ]; then ip link del "$test_ifb"; fi
    if [ "$owns_ns" = 1 ]; then ip netns del "$test_ns"; fi
    rm -rf -- "$test_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

if [ -n "${MUNDO_PROBE:-}" ]; then
    probe=$MUNDO_PROBE
else
    test_sources=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
    "${CC:-cc}" -O2 -pthread "$test_sources/kernel_probe.c" -o "$test_dir/probe"
    probe=$test_dir/probe
fi

modprobe -a veth ifb sch_fq sch_ingress cls_matchall act_mirred sch_netem
ip netns add "$test_ns"
owns_ns=1
ip link add "$test_link" type veth peer name "$test_peer"
owns_link=1
ip link set "$test_peer" netns "$test_ns"
ip addr add 192.0.2.1/30 dev "$test_link"
ip -6 addr add fd00:4d54::1/64 dev "$test_link" nodad
ip link set "$test_link" up
ip -n "$test_ns" addr add 192.0.2.2/30 dev "$test_peer"
ip -n "$test_ns" -6 addr add fd00:4d54::2/64 dev "$test_peer" nodad
ip -n "$test_ns" link set "$test_peer" up
ip -n "$test_ns" link set lo up
ip netns exec "$test_ns" tc qdisc add dev "$test_peer" root fq
export MUNDO_TEST_NS="/run/netns/$test_ns"

ip link add "$test_ifb" type ifb
owns_ifb=1
ip link set "$test_ifb" up
tc qdisc add dev "$test_link" handle ffff: ingress
tc filter add dev "$test_link" parent ffff: matchall action mirred egress redirect dev "$test_ifb"
tc qdisc add dev "$test_link" root netem delay 50ms

# Egress netem can make TCP Small Queues throttle the sender artificially.
# Cross the namespace first to orphan sender SKBs, then shape receiver ingress.
tc qdisc add dev "$test_ifb" root netem delay 50ms rate 100mbit limit 10000
"$probe" lifecycle
"$probe" suspended
"$probe" transfer 20 1 1000
tc qdisc change dev "$test_ifb" root netem delay 50ms loss 1% rate 100mbit limit 10000
"$probe" transfer 20 1 1000
tc qdisc change dev "$test_ifb" root netem delay 50ms rate 100mbit limit 10000
"$probe" transfer 20 4 100
"$probe" transfer 20 1 1000 ipv6
