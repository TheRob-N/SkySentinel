"""Copies the latest builds next to the web UI so a hosted copy (e.g. on your
website) can install them with one click from FIRMWARE.

    pio run -e c5 -e v8 -e classic -e cyd -e heltec
    python3 tools/release_firmware.py
    -> webui/firmware/<board>.{app,factory}.bin + manifest.json

Upload the whole webui/ folder. (Opened as a local file, the page can't fetch
these; pick the .bin with the file chooser instead.)
"""
import datetime
import json
import os
import re
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "webui", "firmware")
BUILDS = [
    ("c5", "C5 board"),
    ("v8", "Marauder V8"),
    ("classic", "Original ESP32 (Classic node)"),
    ("cyd", "CYD / ESP32-32E (Classic node + touch dashboard)"),
    ("heltec", "Heltec Wireless Tracker (LoRa node)"),
    ("tembed", "LilyGO T-Embed CC1101 (sub-GHz node)"),
]


from build_site import build_stamp, stamp_text  # noqa: E402  (same folder)


def main():
    os.makedirs(OUT, exist_ok=True)
    found = []
    for env, label in BUILDS:
        src = os.path.join(ROOT, ".pio", "build", env)
        app, factory = os.path.join(src, "firmware.bin"), os.path.join(src, "firmware.factory.bin")
        if not (os.path.exists(app) and os.path.exists(factory)):
            print(f"skip {env}: not built (pio run -e {env})")
            continue
        shutil.copyfile(app, os.path.join(OUT, f"{env}.app.bin"))
        shutil.copyfile(factory, os.path.join(OUT, f"{env}.factory.bin"))
        stamp = build_stamp(env)
        found.append({"env": env, "label": label, "app": f"{env}.app.bin", "factory": f"{env}.factory.bin",
                      "bld": stamp, "built": stamp_text(stamp)})
        print(f"{env}: copied")
    ver = re.search(r'SS_RELEASE "([^"]+)"', open(os.path.join(ROOT, "lib", "sscore", "ss_version.h")).read())
    manifest = {"version": ver.group(1) if ver else "", "built": datetime.datetime.now().strftime("%Y-%m-%d %H:%M"), "builds": found}
    with open(os.path.join(OUT, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"{len(found)} builds -> webui/firmware/")


main()
