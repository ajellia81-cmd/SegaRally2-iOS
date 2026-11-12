"""Reads a frame log written with LW_FRAMELOG=path and reports how evenly the
images reached the screen and how evenly the motion they show advanced.

usage: python tools/frame_log.py out/frames.log [--skip SECONDS] [--list N]
"""
import argparse
import math
import statistics


def percentile(values, p):
    if not values:
        return 0.0
    values = sorted(values)
    return values[min(len(values) - 1, int(p / 100 * len(values)))]


def describe(name, values, unit="ms"):
    if not values:
        print(f"{name}: none")
        return
    print(f"{name}: mean {statistics.mean(values):.3f} {unit}  sd {statistics.pstdev(values):.3f}  "
          f"p1 {percentile(values, 1):.3f}  p50 {percentile(values, 50):.3f}  p99 {percentile(values, 99):.3f}  "
          f"max {max(values):.3f}  (n {len(values)})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--skip", type=float, default=5.0, help="seconds left out at the start")
    ap.add_argument("--list", type=int, default=15, help="worst presents listed")
    ap.add_argument("--raw", action="store_true", help="screen moments as reported, not put on the vblank grid")
    args = ap.parse_args()

    presents, shown, game = {}, {}, []
    fields = ("id loop display shown_time t built latest latest_time take new_2d scene_ms tex_rows "
              "fence_ms acquire_ms record_ms present_ms loop_ms lateness").split()
    order = []
    for line in open(args.log):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "P" and len(parts) == len(fields) + 1:
            p = dict(zip(fields, (float(x) for x in parts[1:])))
            order.append(p)
            if p["id"]:
                presents[int(p["id"])] = p
        elif parts[0] == "S":
            shown[int(parts[1])] = float(parts[2])
        elif parts[0] == "G":
            game.append((int(parts[1]), float(parts[2])))

    if not order:
        print("no presents in the log")
        return
    start = order[0]["loop"] + args.skip
    order = [p for p in order if p["loop"] >= start]
    game = [g for g in game if g[1] >= start]

    print(f"{len(order)} presents, {len(shown)} with their time on screen, {len(game)} game frames")
    gd = [(b[1] - a[1]) * 1000 for a, b in zip(game, game[1:]) if b[0] == a[0] + 1]
    describe("game frame interval", gd)
    loop_iv = [(b["loop"] - a["loop"]) * 1000 for a, b in zip(order, order[1:])]
    describe("present loop interval", loop_iv)
    describe("loop work (CPU, without waits)", [p["loop_ms"] - p["fence_ms"] - p["acquire_ms"] - p["present_ms"] for p in order])
    describe("  fence wait", [p["fence_ms"] for p in order])
    describe("  acquire wait", [p["acquire_ms"] for p in order])
    describe("  vkQueuePresent", [p["present_ms"] for p in order])
    describe("  scene build", [p["scene_ms"] for p in order])
    describe("  record", [p["record_ms"] for p in order])

    seq = [p for p in order if int(p["id"]) in shown]
    if len(seq) > 2 and not args.raw:
        # The moments VK_KHR_present_wait reports are rounded (0.5 ms steps on
        # NVIDIA): with a fixed refresh they are put back on the vblank grid
        # of the predictions (DWM), each near its own prediction, keeping the
        # phase they show on average
        period = statistics.median([(b["display"] - a["display"]) for a, b in zip(order, order[1:])
                                    if 0 < b["display"] - a["display"] < 0.02])
        angles = [2 * math.pi * ((shown[int(p["id"])] - p["display"]) / period % 1) for p in seq]
        phase = math.atan2(sum(map(math.sin, angles)), sum(map(math.cos, angles))) / (2 * math.pi) * period
        for p in seq:
            i = int(p["id"])
            shown[i] = p["display"] + phase + round((shown[i] - p["display"] - phase) / period) * period
    if len(seq) > 2:
        sv = [(shown[int(b["id"])] - shown[int(a["id"])]) * 1000 for a, b in zip(seq, seq[1:])]
        describe("on-screen interval", sv)
        period = statistics.median(sv)
        print(f"refresh ~{1000 / period:.1f} Hz; intervals > 1.5 refreshes: {sum(v > 1.5 * period for v in sv)}, "
              f"< 0.5 refreshes: {sum(v < 0.5 * period for v in sv)}")
        lat = [(shown[int(p["id"])] - p["display"]) * 1000 for p in seq]
        describe("on screen minus predicted moment", lat)
        describe("on screen minus start of its loop (inputs read)", [(shown[int(p["id"])] - p["loop"]) * 1000 for p in seq])
        describe("on screen minus game time shown (picture age)", [(shown[int(p["id"])] - p["shown_time"]) * 1000 for p in seq])
        # Motion: game time advanced per present over screen time advanced
        rates, errors = [], []
        for a, b in zip(seq, seq[1:]):
            ds = shown[int(b["id"])] - shown[int(a["id"])]
            dg = b["shown_time"] - a["shown_time"]
            if 0 < ds < 0.1 and abs(dg) < 0.1:
                rates.append(dg / ds)
                errors.append((dg - ds) * 1000)
        describe("motion speed (1 = even)", rates, "")
        describe("motion error per present", errors)
        uneven = sum(abs(e) > 1.0 for e in errors)
        print(f"presents whose motion step is off by more than 1 ms: {uneven} ({100 * uneven / len(errors):.1f}%, "
              f"{uneven / ((seq[-1]['loop'] - seq[0]['loop']) or 1):.1f} per second)")
        worst = sorted(zip(seq, seq[1:], errors), key=lambda x: -abs(x[2]))[: args.list]
        print("\nworst presents (motion error ms, screen interval ms, game interval ms, take, new_2d, tex rows, "
              "scene ms, record ms, fence ms, acquire ms, loop ms):")
        for a, b, e in worst:
            ds = (shown[int(b["id"])] - shown[int(a["id"])]) * 1000
            dg = (b["shown_time"] - a["shown_time"]) * 1000
            print(f"  id {int(b['id'])} t={b['loop'] - order[0]['loop'] + args.skip:7.2f}s  err {e:+6.2f}  screen {ds:6.2f}  "
                  f"game {dg:6.2f}  take {int(b['take'])} 2d {int(b['new_2d'])} tex {int(b['tex_rows'])} "
                  f"scene {b['scene_ms']:.2f} rec {b['record_ms']:.2f} fence {b['fence_ms']:.2f} "
                  f"acq {b['acquire_ms']:.2f} loop {b['loop_ms']:.2f} late {b['lateness'] * 1000:.2f}")


if __name__ == "__main__":
    main()
