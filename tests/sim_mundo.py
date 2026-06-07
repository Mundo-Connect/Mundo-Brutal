#!/usr/bin/env python3
import math
import random

HEADROOM = 5
DELIVERY_HEADROOM = 30
DELIVERY_LOW = 50
DELIVERY_FLAT = 30
BAD_CONFIRM = 2
APP_LIMITED_PROBE_DIV = 16
MIN_PROBE_STEP = 16384
MIN_RATE = 62500
INIT_RATE = 125000
FAST_START_DIV = 4
DIRECT_START_RATE = 1000000


class MundoModel:
    def __init__(self, max_rate):
        self.max_rate = max_rate
        if max_rate > DIRECT_START_RATE:
            self.rate = max(INIT_RATE, max_rate / FAST_START_DIV)
        else:
            self.rate = min(INIT_RATE, max_rate)
        self.loss_ewma = 0
        self.peak_delivery = 0
        self.stable_rounds = 0
        self.bad_rounds = 0

    def pacing_rate(self):
        return min(self.rate + self.rate * HEADROOM / 1000, self.max_rate)

    def raise_rate(self, fast):
        if fast:
            step = self.rate / 2
        else:
            step = self.rate / 4
        gap = self.max_rate - self.rate
        self.rate = min(self.max_rate, self.rate + min(max(step, MIN_PROBE_STEP), gap))

    def tick(self, loss, goodput, active=True):
        loss_pm = int(loss * 1000)
        self.loss_ewma = loss_pm if self.loss_ewma == 0 else (self.loss_ewma * 7 + loss_pm) >> 3
        if not active:
            return

        delivery_ceiling = goodput * (1000 + DELIVERY_HEADROOM) / 1000
        if goodput > self.peak_delivery:
            self.peak_delivery = goodput

        peak_growth = max(self.peak_delivery * DELIVERY_FLAT / 1000, 16 * 1024)
        plateau = (
            delivery_ceiling > MIN_RATE
            and self.rate > delivery_ceiling
            and self.rate - delivery_ceiling > self.rate * DELIVERY_LOW / 1000
            and self.peak_delivery
            and goodput <= self.peak_delivery + peak_growth
        )

        if plateau:
            self.stable_rounds = 0
            if self.bad_rounds < BAD_CONFIRM:
                self.bad_rounds += 1
            if self.bad_rounds >= BAD_CONFIRM:
                self.peak_delivery = goodput
                peak_ceiling = self.peak_delivery * (1000 + DELIVERY_HEADROOM) / 1000
                self.rate = max(MIN_RATE, min(self.max_rate, peak_ceiling))
                self.bad_rounds = 0
        else:
            self.bad_rounds = 0
            self.stable_rounds = min(255, self.stable_rounds + 1)
            self.raise_rate(True)



def loss_model(send_rate, capacity, rng, jitter=False):
    noise = rng.uniform(0.000, 0.012)
    if jitter and rng.random() < 0.06:
        noise += rng.uniform(0.04, 0.12)
    if send_rate <= capacity:
        return min(0.95, noise)

    excess = (send_rate - capacity) / send_rate
    return min(0.95, noise + excess * 0.9)


def delivered(send_rate, capacity, loss):
    return min(send_rate, capacity) * (1 - loss)


def run(capacity_fn, seconds=220, max_mbps=200, jitter=False, seed=7):
    rng = random.Random(seed)
    model = MundoModel(max_mbps * 1000 * 1000 / 8)
    rows = []
    for sec in range(seconds):
        cap = capacity_fn(sec) * 1000 * 1000 / 8
        send = model.pacing_rate()
        loss = loss_model(send, cap, rng, jitter=jitter)
        goodput = delivered(send, cap, loss)
        model.tick(loss, goodput)
        rows.append((sec, cap, send, goodput, loss, model.rate, model.loss_ewma))
    return rows


def run_demand(capacity_fn, demand_fn, seconds=220, max_mbps=1000, jitter=False, seed=7):
    rng = random.Random(seed)
    model = MundoModel(max_mbps * 1000 * 1000 / 8)
    rows = []
    for sec in range(seconds):
        cap = capacity_fn(sec) * 1000 * 1000 / 8
        demand = demand_fn(sec) * 1000 * 1000 / 8
        wanted = model.pacing_rate()
        send = min(wanted, demand)
        loss = loss_model(send, cap, rng, jitter=jitter)
        goodput = delivered(send, cap, loss)
        active = demand >= wanted * 0.95 or (
            model.peak_delivery and goodput >= model.peak_delivery / APP_LIMITED_PROBE_DIV
        )
        model.tick(loss, goodput, active=active)
        rows.append((sec, cap, demand, wanted, send, goodput, loss, model.rate, model.loss_ewma))
    return rows


def mbps(value):
    return value * 8 / 1000000


def avg(rows, index):
    return sum(row[index] for row in rows) / len(rows)


def test_converges_without_waste():
    rows = run(lambda _sec: 50, seconds=260, max_mbps=200, jitter=True)
    tail = rows[180:]
    send = mbps(avg(tail, 2))
    goodput = mbps(avg(tail, 3))
    assert 40 <= goodput <= 55, goodput
    assert send <= 65, send


def test_reaches_limit_quickly_when_clean():
    rows = run(lambda _sec: 300, seconds=30, max_mbps=200, jitter=False, seed=3)
    send_5 = mbps(rows[5][2])
    send_10 = mbps(rows[10][2])
    assert send_5 >= 170, send_5
    assert send_10 >= 195, send_10


def test_tracks_capacity_changes():
    def cap(sec):
        if sec < 110:
            return 40
        if sec < 220:
            return 80
        return 30

    rows = run(cap, seconds=340, max_mbps=200, jitter=True, seed=11)
    good_40 = mbps(avg(rows[80:110], 3))
    good_80 = mbps(avg(rows[190:220], 3))
    send_30 = mbps(avg(rows[300:340], 2))
    good_30 = mbps(avg(rows[300:340], 3))

    assert 32 <= good_40 <= 44, good_40
    assert good_80 > good_40 + 18, (good_40, good_80)
    assert 23 <= good_30 <= 34, good_30
    assert send_30 <= 46, send_30


def test_spikes_do_not_collapse_rate():
    rows = run(lambda _sec: 70, seconds=240, max_mbps=200, jitter=True, seed=23)
    rates = [mbps(row[5]) for row in rows[160:]]
    assert min(rates) > 45, min(rates)
    assert not math.isnan(sum(rates))


def test_bursty_demand_fast_probe_and_idle_loss_filter():
    def demand(sec):
        if sec < 20:
            return 0.1
        if sec < 70:
            return 1000
        if sec < 110:
            return 1
        if sec < 150:
            return 1000
        return 0.1

    rows = run_demand(lambda _sec: 300, demand, seconds=170, max_mbps=1000, jitter=True, seed=31)
    first_burst = rows[25:45]
    idle = rows[80:100]
    second_burst = rows[115:135]

    assert mbps(avg(first_burst, 4)) > 230, mbps(avg(first_burst, 4))
    assert mbps(avg(first_burst, 5)) > 210, mbps(avg(first_burst, 5))
    assert mbps(avg(idle, 4)) < 2, mbps(avg(idle, 4))
    assert mbps(avg(second_burst, 4)) > 230, mbps(avg(second_burst, 4))


def test_mixed_rollercoaster_demand():
    def cap(sec):
        if sec < 80:
            return 250
        if sec < 140:
            return 120
        if sec < 210:
            return 350
        return 80

    def demand(sec):
        # Chat baseline with webpage and video-cache bursts plus a large download.
        base = 0.2
        if sec % 17 in (0, 1):
            base += 20
        if 35 <= sec < 55 or 155 <= sec < 175:
            base += 600
        if 95 <= sec < 125:
            base += 1000
        if 220 <= sec < 245:
            base += 200
        return base

    rows = run_demand(cap, demand, seconds=260, max_mbps=1000, jitter=True, seed=91)
    big_120 = rows[100:125]
    fast_350 = rows[160:175]
    low_80 = rows[225:245]
    quiet = rows[245:260]

    assert mbps(avg(big_120, 4)) > 110, mbps(avg(big_120, 4))
    assert mbps(avg(fast_350, 4)) > 240, mbps(avg(fast_350, 4))
    assert mbps(avg(low_80, 4)) < 120, mbps(avg(low_80, 4))
    assert mbps(avg(quiet, 4)) < 5, mbps(avg(quiet, 4))


if __name__ == "__main__":
    test_converges_without_waste()
    test_reaches_limit_quickly_when_clean()
    test_tracks_capacity_changes()
    test_spikes_do_not_collapse_rate()
    test_bursty_demand_fast_probe_and_idle_loss_filter()
    test_mixed_rollercoaster_demand()
    print("mundo simulation tests passed")
