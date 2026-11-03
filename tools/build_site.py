"""Builds the public website (skysentinel.neyhard.com) as a folder of static files.

    pio run -e c5 -e v8 -e heltec -e cyd -e classic
    python3 tools/build_site.py --version 0.9.0 [--git <repo URL>]
    -> dist/site/            upload this folder's contents to the web server
       dist/site.zip         the same, zipped

What's in it:
    index.html, flash.html, docs.html, assets/   (from site/)
    app/index.html                               the console (webui/index.html)
    app/firmware/<env>.{app,factory}.bin + manifest.json   (used by Install and by the console's FIRMWARE)
    downloads/skysentinel-<version>-src.zip + SHA256SUMS + index.html   (the source, so it stays open
                                                                        even without a public git host)
    assets/config.json                           git / mirror links for the How-tos page

The server needs HTTPS (browsers only allow Web Serial on secure pages) and nothing else:
no PHP, no database. Serve .bin as application/octet-stream.
"""
import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILDS = [
    ("c5", "ESP32-C5 DevKitC-1"),
    ("v8", "Marauder V8"),
    ("heltec", "Heltec Wireless Tracker"),
    ("cyd", "CYD / ESP32-32E"),
    ("classic", "ESP32-WROOM-32 DevKit"),
    ("tembed", "LilyGO T-Embed CC1101"),
]
SKIP_DIRS = {".pio", ".git", "dist", "__pycache__", ".vscode"}
SKIP_FILES = {"webui_gz.h", "test_bin"}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def source_zip(dest, version):
    name = f"skysentinel-{version}-src"
    out = os.path.join(dest, name + ".zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for base, dirs, files in os.walk(ROOT):
            dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not (base == os.path.join(ROOT, "webui") and d == "firmware"))
            for f in sorted(files):
                if f in SKIP_FILES:
                    continue
                full = os.path.join(base, f)
                z.write(full, os.path.join(name, os.path.relpath(full, ROOT)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=datetime.date.today().strftime("%Y.%m.%d"))
    ap.add_argument("--git", default="", help="public git repository URL (Gitea, Forgejo, Codeberg...)")
    ap.add_argument("--mirror", default="", help="optional second copy of the repository")
    ap.add_argument("--allow-missing", action="store_true", help="publish even if some boards aren't built")
    a = ap.parse_args()

    dist = os.path.join(ROOT, "dist", "site")
    shutil.rmtree(dist, ignore_errors=True)
    shutil.copytree(os.path.join(ROOT, "site"), dist)
    shutil.copyfile(os.path.join(ROOT, "LICENSE"), os.path.join(dist, "LICENSE.txt"))
    shutil.copyfile(os.path.join(ROOT, "THIRD_PARTY.md"), os.path.join(dist, "THIRD_PARTY.txt"))

    # The console
    app = os.path.join(dist, "app")
    os.makedirs(os.path.join(app, "firmware"))
    shutil.copyfile(os.path.join(ROOT, "webui", "index.html"), os.path.join(app, "index.html"))

    # Firmware
    builds, missing = [], []
    for env, label in BUILDS:
        src = os.path.join(ROOT, ".pio", "build", env)
        appbin, factory = os.path.join(src, "firmware.bin"), os.path.join(src, "firmware.factory.bin")
        if not (os.path.exists(appbin) and os.path.exists(factory)):
            missing.append(env)
            continue
        shutil.copyfile(appbin, os.path.join(app, "firmware", f"{env}.app.bin"))
        shutil.copyfile(factory, os.path.join(app, "firmware", f"{env}.factory.bin"))
        builds.append({"env": env, "label": label, "app": f"{env}.app.bin", "factory": f"{env}.factory.bin",
                       "app_sha256": sha256(appbin), "factory_sha256": sha256(factory)})
    if missing and not a.allow_missing:
        raise SystemExit(f"not built: {', '.join(missing)} (pio run -e {' -e '.join(missing)}), or pass --allow-missing")
    manifest = {"version": a.version, "built": datetime.datetime.now().strftime("%Y-%m-%d %H:%M"), "builds": builds}
    with open(os.path.join(app, "firmware", "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)

    # Source, so the project stays open without depending on any git host
    dl = os.path.join(dist, "downloads")
    os.makedirs(dl)
    src = source_zip(dl, a.version)
    sums = [f"{sha256(src)}  {os.path.basename(src)}"]
    for b in builds:
        sums.append(f"{b['app_sha256']}  app/firmware/{b['app']}")
        sums.append(f"{b['factory_sha256']}  app/firmware/{b['factory']}")
    with open(os.path.join(dl, "SHA256SUMS"), "w") as f:
        f.write("\n".join(sums) + "\n")
    rows = "".join(f'<li><a href="{os.path.basename(src)}">{os.path.basename(src)}</a></li>' for _ in [0])
    with open(os.path.join(dl, "index.html"), "w") as f:
        f.write(f"""<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>SkySentinel downloads</title><link rel="stylesheet" href="../assets/site.css"></head><body><div class="wrap" style="padding:40px 20px">
<h2>Downloads</h2><p class="kicker">Version {a.version}, built {manifest['built']}.</p>
<h3>Source</h3><ul>{rows}</ul>
<h3>Firmware</h3><ul>{''.join(f'<li>{b["label"]}: <a href="../app/firmware/{b["app"]}">update</a> &middot; <a href="../app/firmware/{b["factory"]}">full</a></li>' for b in builds)}</ul>
<p><a href="SHA256SUMS">SHA256SUMS</a> &middot; <a href="../">back to the site</a></p></div></body></html>""")

    with open(os.path.join(dist, "assets", "config.json"), "w") as f:
        json.dump({"version": a.version, "git": a.git, "mirror": a.mirror}, f, indent=2)

    zpath = os.path.join(ROOT, "dist", "site.zip")
    if os.path.exists(zpath):
        os.remove(zpath)
    shutil.make_archive(zpath[:-4], "zip", dist)
    print(f"{len(builds)} firmware builds, source {os.path.basename(src)} -> dist/site/ and dist/site.zip"
          + (f" (missing: {', '.join(missing)})" if missing else ""))


main()
