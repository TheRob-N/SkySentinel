#!/bin/bash
# Quick health check of a SkySentinel cluster from the loggers' files (tools/sslog.py).
#   tools/sscheck.sh                  # logs in your home folder: master.log, devkit.log, cyd.log
#   LOGDIR=/some/where tools/sscheck.sh
# Reads only the log files; never touches a board's port.
D="${LOGDIR:-$HOME}"
python3 -I - "$D/master.log" <<'EOF'
import json, sys
def last(path):
    try:
        lines = open(path, errors="ignore").read().splitlines()
    except OSError:
        return None, None
    for line in reversed(lines):
        i = line.find('{"t":"status"')
        if i >= 0:
            try:
                return line[:19], json.loads(line[i:])
            except Exception:
                pass
    return None, None
when, m = last(sys.argv[1])
if not m:
    print("master: no status in", sys.argv[1]); sys.exit()
print("master status at %s: up %ds, last start %s, ble_adv %s, rx_dropped %s, slow %s, loop_max %s ms, usb kicks/resets %s/%s" % (
    when, m["ms"] // 1000, m.get("rst"), m.get("ble_adv"), m.get("rx_dropped"), m.get("slow"), m.get("loop_max"),
    m.get("usb_kicks"), m.get("usb_resets")))
if m.get("shed"):
    print("  flood: skipped %s sighting frames from nodes (%s sightings) to keep up" % (m["shed"], m.get("shedn", 0)))
def fw(x):
    b = str(x.get("bld") or "")
    if not x.get("rel"):
        return "fw 2.1.2-"
    return "fw %s 20%s" % (x["rel"], b[:6]) if len(b) == 10 else "fw " + x["rel"]
print("  master build: %s" % fw(m))
if "lch" in m:
    c = m.get("lchc") or []
    ml = m.get("lchl") or []
    print("  link channel: %s (%s)%s  survey 1/6/11 (1 h avg): %s  measured loss: %s%s" % (m["lch"], m.get("lchm", "?"),
          ", moving to %s" % m["lchto"] if m.get("lchto") else "", "/".join(str(x) for x in c),
          "/".join("-" if x < 0 else "%.1f%%" % (x / 10.0) for x in ml) or "-",
          ", last move %s->%s %s, %ds ago" % (m.get("lchfrom"), m["lch"], m.get("lchwhy"), m["lchmv"]) if "lchmv" in m else ""))
for n in m.get("nodes", []):
    f, l = n.get("frames", 0), n.get("lost", 0)
    drv = "  slowest send %s ms" % n["txw"] if n.get("txw") else ""
    rec = ""
    if "l10" in n:
        rec = "  lately %.1f%% (10 min)" % (n["l10"] / 10.0)
        if "l60" in n:
            rec += " %.1f%% (%s)" % (n["l60"] / 10.0, "1 h" if n.get("l60s", 0) >= 3540 else "%d min" % (n.get("l60s", 0) // 60))
    if n.get("stk"):
        drv += ", stuck %.1f s before last restart" % (n["stk"] / 1000.0)
    print("  %-16s %s  %-7s link %4s dBm  lost %5d/%-6d (%4.1f%%)  up %7ss  last start %-9s  job %s, link %s  %s%s%s" % (
        n.get("label") or n["id"], n["id"], "online" if n.get("online") else "OFFLINE", n.get("link"), l, f,
        100.0 * l / max(f + l, 1), n.get("up"), n.get("rst"), n.get("force"), n.get("lmd"), fw(n), rec, drv))
EOF
echo "master log last line:  $(tail -1 "$D/master.log" 2>/dev/null | cut -c1-19)   (now: $(date '+%Y-%m-%d %H:%M:%S'))"
echo "node drops at master:  $(grep -c '"event":"lost"' "$D/master.log" 2>/dev/null)"
echo "devkit lost-master:    $(grep -a -c 'lost master' "$D/devkit.log" 2>/dev/null)"
echo "cyd memory:            $(grep -a -o '"heap":[0-9]*,"heap_min":[0-9]*,"heap_big":[0-9]*' "$D/cyd.log" 2>/dev/null | tail -1)"
echo "cyd send failures:     $(grep -a -o '"send_fail":[0-9]*' "$D/cyd.log" 2>/dev/null | tail -1)"
echo "cyd queue drops:       $(grep -a -o '"drops":[0-9]*' "$D/cyd.log" 2>/dev/null | tail -1)   loop stack left: $(grep -a -o '"loop_stack_min":[0-9]*' "$D/cyd.log" 2>/dev/null | tail -1)"
# Sub-GHz bursts the master logged (one line each), per band: how many, and the last one
python3 - "$D/master.log" <<'PYEOF'
import json, sys
bands = {}
try:
    for line in open(sys.argv[1], errors="ignore"):
        if '"t":"burst"' not in line:
            continue
        try:
            b = json.loads(line[line.index("{"):])
        except ValueError:
            continue
        k = "%.2f MHz" % b["mhz"]
        c = bands.setdefault(k, [0, ""])
        c[0] += b.get("n", 1)
        c[1] = "%s %d dBm (%s)" % (line[11:19], b["rssi"], b["node"])
except OSError:
    pass
print("sub-GHz bursts:        " + ("  ".join("%s x%d, last %s" % (k, v[0], v[1]) for k, v in sorted(bands.items())) or "none logged"))
PYEOF
