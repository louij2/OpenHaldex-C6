#!/usr/bin/env python3
"""
Read the controller's passive bus monitor (GET /api/busmon) from any computer.

Unlike can_capture.py this needs no analyzer mode: the controller keeps working
normally and just reports every CAN ID it has received on each side (car /
haldex) with count, rate, first/last seen and how often the payload changed.
Polling it does not count as UI activity, so it never keeps the controller (or
the car) awake.

  python3 tools/busmon.py --host 10.0.0.73 show --dbc vw_pq.dbc
  python3 tools/busmon.py --host 10.0.0.73 save before.json    # snapshot to a file
  python3 tools/busmon.py diff before.json after.json --dbc vw_pq.dbc
  python3 tools/busmon.py --host 10.0.0.73 reset               # start counting afresh
  python3 tools/busmon.py --host 10.0.0.73 baseline            # save current IDs on the device
  python3 tools/busmon.py --host 10.0.0.73 baseline --clear
  python3 tools/busmon.py --host 10.0.0.73 log                 # sleep logger, wall-clock times
  python3 tools/busmon.py --host 10.0.0.73 log --csv s.csv     # also write the 2-min samples

`show` marks IDs that neither the DBC nor the firmware's own ID list names as
UNKNOWN, and (once a baseline is saved on the device) IDs outside it as NEW.
"""
import argparse
import json
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from can_capture import BUS_NAME, load_names  # noqa: E402


def call(host, path, method="GET"):
    req = urllib.request.Request(f"http://{host}{path}", method=method)
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.load(r)


def fetch(host):
    d = call(host, "/api/busmon")
    d["host_time"] = time.time()
    return d


def ago(ms):
    s = ms / 1000
    return f"{s:.1f}s" if s < 120 else f"{s / 60:.0f}m" if s < 7200 else f"{s / 3600:.1f}h"


def print_snapshot(d, names):
    print(f"{d['idCount']} IDs, {d['frames'][0]} car + {d['frames'][1]} haldex frames "
          f"in {ago(d['sinceResetMs'])} since reset; baseline {d['baselineCount'] or 'none'}"
          + (f"; table FULL ({d['droppedFrames']} frames untracked)" if d["droppedFrames"] else ""))
    for e in sorted(d["ids"], key=lambda e: (e["bus"], int(e["id"], 16))):
        cid = int(e["id"], 16)
        name = names.get(cid)
        flags = ("  NEW" if e["new"] else "") + ("" if name else "  UNKNOWN")
        print(f"  {BUS_NAME[e['bus']]:6s} {e['id']:>10s} {e['hz']:8.2f}/s n={e['count']:<8d} "
              f"last {ago(e['lastAgoMs']):>6s} chg={e['changes']:<6d} {e['data']:<16s} {name or '?'}{flags}")
    for m in d.get("missing", []):
        print(f"  MISSING since reset: {BUS_NAME[m['bus']]} {m['id']} {names.get(int(m['id'], 16), '?')}")


def cmd_show(a):
    print_snapshot(fetch(a.host), load_names(a.dbc))


def cmd_save(a):
    d = fetch(a.host)
    with open(a.file, "w") as f:
        json.dump(d, f, indent=1)
    print(f"saved {d['idCount']} IDs to {a.file}")


def cmd_diff(a):
    names = load_names(a.dbc)
    old, new = (json.load(open(p)) for p in (a.before, a.after))
    key = lambda e: (e["bus"], e["id"])  # noqa: E731
    o, n = {key(e): e for e in old["ids"]}, {key(e): e for e in new["ids"]}
    for k in sorted(set(o) | set(n), key=lambda k: (k[0], int(k[1], 16))):
        tag = f"{BUS_NAME[k[0]]:6s} {k[1]:>10s} {names.get(int(k[1], 16), 'UNKNOWN')}"
        if k not in o:
            print(f"  + {tag}  {n[k]['hz']:.2f}/s  {n[k]['data']}")
        elif k not in n:
            print(f"  - {tag}  (was {o[k]['hz']:.2f}/s)")
        elif o[k]["data"] != n[k]["data"] and a.payloads:
            print(f"  ~ {tag}  {o[k]['data']} -> {n[k]['data']}")


def cmd_reset(a):
    print(call(a.host, "/api/busmon/reset", "POST"))


def cmd_baseline(a):
    print(call(a.host, "/api/busmon/baseline", "DELETE" if a.clear else "POST"))


def cmd_log(a):
    """Sleep logger. Device times are uptime seconds per boot; lines from the
    current boot get a wall-clock time, older boots keep their uptime."""
    now = time.time()
    d = call(a.host, "/api/buslog")
    req = urllib.request.Request(f"http://{a.host}/api/buslog/events")
    with urllib.request.urlopen(req, timeout=10) as r:
        events = r.read().decode(errors="replace").splitlines()
    boot0 = now - d["uptimeS"]
    wall = lambda up: time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(boot0 + up))  # noqa: E731
    print(f"boot #{d['boot']}, up {ago(d['uptimeS'] * 1000)}; quiet now: car={d['quiet'][0]} haldex={d['quiet'][1]}")
    for line in events:
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[0] == f"b{d['boot']}" and parts[1].startswith("t"):
            print(f"  {wall(int(parts[1][1:]))}  {parts[2]}")
        else:
            print(f"  (earlier boot) {line}")
    rows = d["samples"]
    if rows:
        start = d["endUptimeS"] - d["intervalS"] * len(rows)
        if a.csv:
            with open(a.csv, "w") as f:
                f.write("time,car_fps,haldex_fps,car_ids,haldex_ids,low_power\n")
                for i, r in enumerate(rows):
                    f.write(wall(start + d["intervalS"] * (i + 1)) + "," + ",".join(map(str, r)) + "\n")
            print(f"{len(rows)} samples written to {a.csv}")
        else:
            print(f"last samples ({d['intervalS']} s each): time car_fps haldex_fps car_ids haldex_ids lp")
            for i, r in list(enumerate(rows))[-a.tail:]:
                print(f"  {wall(start + d['intervalS'] * (i + 1))}  " + "  ".join(f"{v:5d}" for v in r))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="openhaldex.local")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show", help="print the live per-ID list")
    s.add_argument("--dbc", help="DBC for naming IDs (e.g. opendbc vw_pq.dbc)")
    s.set_defaults(func=cmd_show)
    v = sub.add_parser("save", help="save a snapshot to a JSON file")
    v.add_argument("file")
    v.set_defaults(func=cmd_save)
    d = sub.add_parser("diff", help="IDs that appeared/disappeared between two snapshots")
    d.add_argument("before")
    d.add_argument("after")
    d.add_argument("--dbc")
    d.add_argument("--payloads", action="store_true", help="also list IDs whose last payload differs")
    d.set_defaults(func=cmd_diff)
    r = sub.add_parser("reset", help="clear the device's table")
    r.set_defaults(func=cmd_reset)
    b = sub.add_parser("baseline", help="save (or --clear) the device-side baseline")
    b.add_argument("--clear", action="store_true")
    b.set_defaults(func=cmd_baseline)
    lg = sub.add_parser("log", help="sleep logger: events + 2-minute samples")
    lg.add_argument("--csv", help="write all samples to this CSV")
    lg.add_argument("--tail", type=int, default=15, help="samples to print (default 15)")
    lg.set_defaults(func=cmd_log)
    a = p.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()
