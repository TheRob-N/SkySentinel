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
for n in m.get("nodes", []):
    f, l = n.get("frames", 0), n.get("lost", 0)
    print("  %-16s %s  %-7s link %4s dBm  lost %5d/%-6d (%4.1f%%)  up %7ss  last start %-9s  job %s, link %s" % (
        n.get("label") or n["id"], n["id"], "online" if n.get("online") else "OFFLINE", n.get("link"), l, f,
        100.0 * l / max(f + l, 1), n.get("up"), n.get("rst"), n.get("force"), n.get("lmd")))
EOF
echo "master log last line:  $(tail -1 "$D/master.log" 2>/dev/null | cut -c1-19)   (now: $(date '+%Y-%m-%d %H:%M:%S'))"
echo "node drops at master:  $(grep -c '"event":"lost"' "$D/master.log" 2>/dev/null)"
echo "devkit lost-master:    $(grep -a -c 'lost master' "$D/devkit.log" 2>/dev/null)"
echo "cyd memory:            $(grep -a -o '"heap":[0-9]*,"heap_min":[0-9]*,"heap_big":[0-9]*' "$D/cyd.log" 2>/dev/null | tail -1)"
echo "cyd send failures:     $(grep -a -o '"send_fail":[0-9]*' "$D/cyd.log" 2>/dev/null | tail -1)"
echo "cyd queue drops:       $(grep -a -o '"drops":[0-9]*' "$D/cyd.log" 2>/dev/null | tail -1)   loop stack left: $(grep -a -o '"loop_stack_min":[0-9]*' "$D/cyd.log" 2>/dev/null | tail -1)"
