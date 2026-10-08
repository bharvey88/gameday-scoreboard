#!/usr/bin/env python3
"""Flash a batch of Game Day Scoreboard controllers and label each one.

For every controller plugged in over USB it erases the whole flash, so no
Wi-Fi network or setting from testing survives, then writes the release
factory image and reads the MAC address. The MAC gives the name the panel
uses on the network, gameday- plus the MAC's last six hex digits, the same
name ESPHome builds at boot. Each unit gets a row in factory/log.csv and a
label in factory/labels/<name>.svg, 50 x 25 mm, ready to print.

Run it with the ESPHome venv's python, which has esptool and pyserial:

    python scripts/factory_flash.py --release v1.5.5
    python scripts/factory_flash.py --image path/to/firmware.factory.bin
    python scripts/factory_flash.py --release v1.5.5 --dry-run
    python scripts/factory_flash.py --selftest

--release downloads the factory image from the GitHub release once and keeps
it in factory/firmware/. The version and pinout are read from the image
itself, and an image built for the other pinout is refused. --dry-run prints
the esptool commands with a made-up MAC instead of talking to a board.

Leave out --port for a batch. Each controller has its own USB serial number,
so Windows gives every one a new COM port, and the script finds it each time.
"""

import argparse
import csv
import datetime
import hashlib
import re
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

REPO = "gameday-scoreboard/gameday-scoreboard"
CHIP = "esp32s3"
ESPRESSIF_VID = 0x303A
LOG_FIELDS = ["time", "name", "mac", "variant", "version", "image", "sha256", "result", "boot_log"]
LABEL_W_MM = 50
LABEL_H_MM = 25
# ESPHome logs this at boot: "Project bharvey88.gameday-scoreboard version 1.5.5"
BOOT_VERSION_RE = re.compile(r"gameday-scoreboard version (\d+\.\d+\.\d+)")


def release_asset(tag, variant):
    """File name of the factory image on the GitHub release (see build.yml)."""
    if variant == "moonhub75":
        return f"gameday-scoreboard-{tag}.factory.bin"
    return f"gameday-scoreboard-{tag}-{variant}.factory.bin"


def version_from(text):
    m = re.search(r"v?(\d+\.\d+\.\d+)", text)
    return m.group(1) if m else ""


def image_info(data):
    """(variant, version) the firmware in an image was built for, "" if not found.

    The update manifest URL names the variant, and the boot log line holds the
    project version, both as plain strings in the app.
    """
    variant = re.search(rb"/firmware/([a-z0-9]+)/manifest\.json", data)
    version = re.search(rb"gameday-scoreboard version (\d+\.\d+\.\d+)", data)
    return (variant.group(1).decode() if variant else "", version.group(1).decode() if version else "")


def save(what, write):
    """Run a file write, asking the operator to close the file while it's locked."""
    while True:
        try:
            write()
            return
        except PermissionError:
            input(f"  Can't write {what}. Close it if it's open (Excel locks it) and press Enter: ")


def check_image(data):
    """Return a problem with a factory image, or None if it looks right.

    A factory image starts with the bootloader at 0x0 and has the partition
    table at 0x8000. An OTA image is the app alone and would leave the board
    with no bootloader.
    """
    if len(data) < 0x10000:
        return f"only {len(data)} bytes, too small for a factory image"
    if data[0] != 0xE9:
        return "no ESP image header at 0x0"
    if data[0x8000:0x8002] != b"\xaa\x50":
        return "no partition table at 0x8000 (an OTA .bin, not the .factory.bin?)"
    return None


def parse_mac(output):
    """MAC address from esptool read-mac output, lower case with colons.

    Chips with an EUI-64 print MAC, BASE MAC and MAC_EXT. The base MAC is the
    one the Wi-Fi station uses, so prefer it.
    """
    macs = {}
    for label, mac in re.findall(r"^\s*(MAC|BASE MAC):\s*([0-9A-Fa-f:]+)\s*$", output, re.M):
        macs.setdefault(label, mac.lower())
    mac = macs.get("BASE MAC") or macs.get("MAC")
    if mac is None or len(mac.split(":")) != 6:
        return None
    return mac


def panel_name(mac):
    return "gameday-" + mac.replace(":", "")[-6:]


def label_svg(name, mac, version):
    detail = f"MAC {mac}" + (f"   v{version}" if version else "")
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{LABEL_W_MM}mm" height="{LABEL_H_MM}mm" '
        f'viewBox="0 0 {LABEL_W_MM} {LABEL_H_MM}">\n'
        f'  <rect width="{LABEL_W_MM}" height="{LABEL_H_MM}" fill="#fff"/>\n'
        '  <text x="25" y="7" font-family="Helvetica, Arial, sans-serif" font-size="3.6" '
        'text-anchor="middle">Game Day Scoreboard</text>\n'
        '  <text x="25" y="15.5" font-family="Menlo, Consolas, monospace" font-size="5.4" '
        f'font-weight="bold" text-anchor="middle">{name}</text>\n'
        '  <text x="25" y="21.5" font-family="Menlo, Consolas, monospace" font-size="2.4" '
        f'text-anchor="middle" xml:space="preserve">{detail}</text>\n'
        "</svg>\n"
    )


def read_log(path):
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def append_log(path, row):
    new = not path.exists()
    with path.open("a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=LOG_FIELDS)
        if new:
            w.writeheader()
        w.writerow(row)


def esptool_cmd(port, baud, *args):
    return [sys.executable, "-m", "esptool", "--chip", CHIP, "--port", port, "--baud", str(baud), *args]


def run(cmd, dry_run):
    print("  $ " + " ".join(cmd))
    if dry_run:
        return ""
    proc = subprocess.run(cmd, capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    if proc.returncode != 0:
        raise RuntimeError(f"esptool failed (exit {proc.returncode}):\n{out.strip()}")
    return out


def pick_port():
    """The one Espressif USB device, or the only serial port there is."""
    from serial.tools import list_ports

    ports = list(list_ports.comports())
    espressif = [p for p in ports if p.vid == ESPRESSIF_VID]
    if len(espressif) == 1:
        return espressif[0].device
    if len(ports) == 1:
        return ports[0].device
    listed = ", ".join(f"{p.device} ({p.description})" for p in ports) or "none"
    raise RuntimeError(f"can't tell which port is the controller; pass --port. Ports: {listed}")


def watch_boot(port, seconds):
    """Version the firmware logs at boot, or None if it isn't seen in time.

    Native USB drops off the bus while the chip resets, so keep trying to open
    the port until the time runs out.
    """
    import serial

    deadline = time.monotonic() + seconds
    buf = ""
    while time.monotonic() < deadline:
        try:
            with serial.Serial(port, 115200, timeout=0.5) as ser:
                while time.monotonic() < deadline:
                    buf += ser.read(512).decode("utf-8", "replace")
                    m = BOOT_VERSION_RE.search(buf)
                    if m:
                        return m.group(1)
                    buf = buf[-200:]
        except (OSError, serial.SerialException):
            time.sleep(0.5)
    return None


def fetch_release(tag, variant, folder):
    folder.mkdir(parents=True, exist_ok=True)
    name = release_asset(tag, variant)
    path = folder / name
    if not path.exists():
        url = f"https://github.com/{REPO}/releases/download/{tag}/{name}"
        print(f"Downloading {url}")
        with urllib.request.urlopen(url, timeout=60) as r:
            data = r.read()
        tmp = path.with_suffix(".part")
        tmp.write_bytes(data)
        tmp.replace(path)
    return path


def flash_one(args, image, image_sha, version, out):
    port = "COM?" if args.dry_run and not args.port else (args.port or pick_port())
    print(f"Port {port}")
    # esptool prints the MAC when it connects, so one write-flash call does it all
    erase = [] if args.no_erase else ["--erase-all"]
    row = {
        "time": datetime.datetime.now().isoformat(timespec="seconds"),
        "name": "",
        "mac": "",
        "variant": args.variant,
        "version": version,
        "image": image.name,
        "sha256": image_sha[:16],
        "result": "failed",
        "boot_log": "",
    }
    log_path = out / "log.csv"
    try:
        output = run(esptool_cmd(port, args.baud, "write-flash", *erase, "0x0", str(image)), args.dry_run)
    except RuntimeError as e:
        mac = parse_mac(str(e))
        if mac:
            row.update(name=panel_name(mac), mac=mac)
        save(log_path, lambda: append_log(log_path, row))
        raise
    mac = "aa:bb:cc:12:34:56" if args.dry_run else parse_mac(output)
    if mac is None:
        raise RuntimeError(f"flashed, but no MAC in the esptool output:\n{output.strip()}")
    name = panel_name(mac)
    row.update(name=name, mac=mac, result="ok")
    print(f"MAC {mac} -> {name}")
    earlier = [r for r in read_log(log_path) if r["mac"] == mac and r["result"] == "ok"]
    if earlier:
        print(f"  Flashed before, at {earlier[-1]['time']}. Use the label from this run.")

    if args.boot_check > 0 and not args.dry_run:
        seen = watch_boot(port, args.boot_check)
        if seen is None:
            row["boot_log"] = "not seen"
            print("  Boot log: no version line seen. Check the panel lights up.")
        else:
            row["boot_log"] = seen
            print(f"  Boot log: firmware {seen}")
            if version and seen != version:
                row["result"] = "wrong version"
                save(log_path, lambda: append_log(log_path, row))
                raise RuntimeError(f"{name} runs firmware {seen}, expected {version}. No label printed.")

    label = out / "labels" / f"{name}.svg"
    if not args.dry_run:
        label.parent.mkdir(parents=True, exist_ok=True)
        save(label, lambda: label.write_text(label_svg(name, mac, version), encoding="utf-8"))
        save(log_path, lambda: append_log(log_path, row))
    print(f"OK {name}  label {label}")


def selftest():
    s3 = "esptool v5.3.1\nConnected to ESP32-S3 on COM7:\nMAC:                34:85:18:2f:6a:70\nHard resetting...\n"
    assert parse_mac(s3) == "34:85:18:2f:6a:70"
    eui = "MAC:                60:55:f9:ff:fe:12:34:56\nBASE MAC:           60:55:f9:12:34:56\nMAC_EXT:            ff:fe\n"
    assert parse_mac(eui) == "60:55:f9:12:34:56"
    assert parse_mac("A fatal error occurred: Failed to connect") is None
    assert parse_mac("MAC: 34:85:18:2F:6A:70") == "34:85:18:2f:6a:70"
    assert panel_name("34:85:18:2f:6a:70") == "gameday-2f6a70"

    assert release_asset("v1.5.5", "scoreboard75") == "gameday-scoreboard-v1.5.5-scoreboard75.factory.bin"
    assert release_asset("v1.5.5", "moonhub75") == "gameday-scoreboard-v1.5.5.factory.bin"
    assert version_from("gameday-scoreboard-v1.5.5-scoreboard75.factory.bin") == "1.5.5"
    assert version_from("firmware.factory.bin") == ""

    good = bytearray(b"\xff" * 0x20000)
    good[0] = 0xE9
    good[0x8000:0x8002] = b"\xaa\x50"
    assert check_image(bytes(good)) is None
    ota = bytearray(good)
    ota[0x8000:0x8002] = b"\xff\xff"
    assert "partition table" in check_image(bytes(ota))
    assert "header" in check_image(b"\x00" * 0x20000)
    assert "too small" in check_image(b"\xe9")

    built = bytes(good) + (b"https://gamedayscoreboard.app/firmware/scoreboard75/manifest.json\x00"
                           b"Project bharvey88.gameday-scoreboard version 1.5.5\x00")
    assert image_info(built) == ("scoreboard75", "1.5.5")
    assert image_info(bytes(good)) == ("", "")

    attempts = []

    def locked_once():
        attempts.append(1)
        if len(attempts) == 1:
            raise PermissionError("log.csv is open in Excel")

    import builtins

    real_input = builtins.input
    builtins.input = lambda prompt="": ""
    try:
        save("log.csv", locked_once)
    finally:
        builtins.input = real_input
    assert len(attempts) == 2

    svg = label_svg("gameday-2f6a70", "34:85:18:2f:6a:70", "1.5.5")
    assert ">gameday-2f6a70<" in svg and "v1.5.5" in svg and 'width="50mm"' in svg
    assert "v" not in label_svg("gameday-2f6a70", "34:85:18:2f:6a:70", "").split("MAC")[1].split("<")[0]

    assert BOOT_VERSION_RE.search("[I][app:100]: Project bharvey88.gameday-scoreboard version 1.5.5").group(1) == "1.5.5"

    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / "log.csv"
        row = dict.fromkeys(LOG_FIELDS, "")
        append_log(log, {**row, "mac": "34:85:18:2f:6a:70", "result": "ok"})
        append_log(log, {**row, "mac": "34:85:18:00:00:01", "result": "failed"})
        rows = read_log(log)
        assert [r["mac"] for r in rows] == ["34:85:18:2f:6a:70", "34:85:18:00:00:01"]
        assert log.read_text(encoding="utf-8").count("time,name,mac") == 1

    cmd = esptool_cmd("COM7", 460800, "write-flash", "--erase-all", "0x0", "x.bin")
    assert cmd[1:] == ["-m", "esptool", "--chip", "esp32s3", "--port", "COM7", "--baud", "460800",
                       "write-flash", "--erase-all", "0x0", "x.bin"]
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--release", help="release tag to download the factory image from, e.g. v1.5.5")
    src.add_argument("--image", type=Path, help="local .factory.bin to flash")
    ap.add_argument("--variant", default="scoreboard75", choices=["scoreboard75", "moonhub75"])
    ap.add_argument("--port", help="serial port, for one board only (default: find the Espressif USB device "
                    "for each unit)")
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--out", type=Path, default=Path("factory"), help="log, labels and downloads (default: factory/)")
    ap.add_argument("--count", type=int, default=0, help="stop after this many units (default: until you quit)")
    ap.add_argument("--no-erase", action="store_true", help="skip the full erase (keeps old settings)")
    ap.add_argument("--boot-check", type=int, default=20, metavar="SECONDS",
                    help="watch the boot log this long for the firmware version, 0 to skip")
    ap.add_argument("--dry-run", action="store_true", help="print the esptool commands, touch no board")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        selftest()
        return 0
    if not args.release and not args.image:
        ap.error("pass --release or --image")

    if args.release:
        if args.dry_run:
            image = args.out / "firmware" / release_asset(args.release, args.variant)
        else:
            image = fetch_release(args.release, args.variant, args.out / "firmware")
        version = version_from(args.release)
    else:
        image = args.image
        version = version_from(image.name)

    image_sha = ""
    if image.exists():
        data = image.read_bytes()
        problem = check_image(data)
        if problem:
            print(f"{image}: {problem}", file=sys.stderr)
            return 1
        built_for, built_version = image_info(data)
        if built_for and built_for != args.variant:
            print(f"{image}: built for {built_for}, not {args.variant}. Pass --variant {built_for} "
                  "if that's right.", file=sys.stderr)
            return 1
        version = built_version or version
        image_sha = hashlib.sha256(data).hexdigest()
        print(f"Image {image} ({len(data)} bytes, {built_for or 'unknown pinout'} "
              f"v{version or '?'}, sha256 {image_sha[:16]})")
    elif not (args.dry_run and args.release):
        print(f"{image}: not found", file=sys.stderr)
        return 1
    args.out.mkdir(parents=True, exist_ok=True)

    done = 0
    while not args.count or done < args.count:
        reply = input(f"\nPlug in controller {done + 1} and press Enter (q to stop): ").strip().lower()
        if reply == "q":
            break
        try:
            flash_one(args, image, image_sha, version, args.out)
            done += 1
        except (RuntimeError, OSError) as e:
            print(f"FAILED: {e}")
    verb = "would be flashed (dry run)" if args.dry_run else "flashed this run"
    print(f"\n{done} {verb}. Log: {args.out / 'log.csv'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
