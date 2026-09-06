#!/usr/bin/env python3
"""Seeded scheduling model; no codec, authentication or SRFEC wire implementation."""
import heapq
import itertools
import json
import math
import random


def evaluate(loss, first_factor=1.5, retry_factor=0.75, seed=71237, epoch_frames=100):
    rng = random.Random(seed)
    business_r, max_r, frames = 2.4, 5.0, 3000
    first_r = min(max_r, business_r * first_factor)
    retry_r = min(max_r, business_r * retry_factor)
    queue, serial = [], itertools.count()
    stats = dict(planned_copies=0, sent_copies=0, first_maintenance=0,
                 retry_maintenance=0, rewritten_unsent=0, missing=0, stale=0)
    state, slots, delivered, last_retry = {}, [-1, -1], set(), {}

    def add(t, kind, data):
        heapq.heappush(queue, (t, next(serial), kind, data))

    def count(r):
        return math.floor(r) + (rng.random() < r - math.floor(r))

    def network(t, kind, data):
        if rng.random() >= loss:
            add(t + rng.uniform(0.010, 0.090), kind, data)

    def maintain(t, epoch, retry):
        n = count(retry_r if retry else first_r)
        assert n <= math.ceil(max_r)
        stats["retry_maintenance" if retry else "first_maintenance"] += n
        for copy in range(n):
            add(t + copy * 0.004, "maintenance_send", epoch)

    # Two physical partitions. An epoch here denotes
    # one modeled maintenance unit plus its retire; this is not the six-slot store.
    for frame in range(frames):
        t = frame * 0.005
        epoch = frame // epoch_frames
        if frame % epoch_frames == 0:
            maintain(t, epoch, False)
        n = count(business_r)
        state[frame] = dict(epoch=epoch, expiry=t + 0.200, sent=0, total=n)
        stats["planned_copies"] += n
        for copy in range(n):
            add(t + 0.001 + copy * 0.050, "data_send", frame)

    while queue:
        t, _, kind, item = heapq.heappop(queue)
        current_epoch = min(int(t / (epoch_frames * 0.005)), (frames - 1) // epoch_frames)
        if kind == "maintenance_send":
            network(t, "maintenance_arrive", item)
        elif kind == "maintenance_arrive":
            if item > slots[item % 2]:
                slots[item % 2] = item
        elif kind == "data_send":
            s = state[item]
            s["sent"] += 1
            stats["sent_copies"] += 1
            network(t, "data_arrive", (item, s["epoch"]))
        elif kind == "data_arrive":
            frame, epoch = item
            if frame in delivered:
                continue
            if slots[epoch % 2] == epoch:
                delivered.add(frame)
            else:
                stale = slots[epoch % 2] > epoch
                stats["stale" if stale else "missing"] += 1
                network(t, "nack", (frame, epoch, stale))
        elif kind == "nack":
            frame, epoch, stale = item
            s = state[frame]
            if frame in delivered or t > s["expiry"]:
                continue
            if stale or epoch < current_epoch - 1:
                if s["sent"] < s["total"] and s["epoch"] != current_epoch:
                    s["epoch"] = current_epoch
                    stats["rewritten_unsent"] += s["total"] - s["sent"]
                # No new business copy and no rescue of the expired dictionary.
            elif t - last_retry.get(epoch, -1.0) >= 0.010:
                maintain(t, epoch, True)
                last_retry[epoch] = t
        else:
            raise AssertionError(kind)
    assert stats["sent_copies"] == stats["planned_copies"]
    return dict(loss=loss, epoch_seconds=epoch_frames * 0.005, first_r=first_r, retry_r=retry_r, max_r=max_r,
                frames=frames, delivered=len(delivered), residual_loss=1-len(delivered)/frames, **stats)


if __name__ == "__main__":
    for epoch_frames in (100, 10):
        for probability in (0.0, 0.1, 0.3):
            print(json.dumps(evaluate(probability, epoch_frames=epoch_frames), sort_keys=True))
    # A larger retry coefficient is allowed, with the same max_r clamp.
    result = evaluate(0.1, first_factor=0.5, retry_factor=4.0)
    assert result["retry_r"] == result["max_r"] and result["retry_r"] > result["first_r"]
    print(json.dumps(result, sort_keys=True))
