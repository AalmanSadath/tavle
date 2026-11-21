#!/usr/bin/env python3
"""tavle setup: build, flash and write the hardware config of a device.

Run in the ESP-IDF shell from the repository root:

    python tools/tavle.py                      ask for everything
    python tools/tavle.py --config hw.json     use a saved answer file
    python tools/tavle.py --config-only        write only the hardware config
    python tools/tavle.py --dry-run            show the commands, run none

The firmware is built per variant (chip and memory). Pins, the display and
its options are the hardware config, written to its own flash sector, so
they change without a rebuild. Wi-Fi, settings and the device name are left
as they are.
"""

import argparse
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Same layout as main/hwcfg.c: one sector after the two layout slots in the
# "storage" partition, a header then the JSON.
HWCFG_OFFSET = 0x20000
HWCFG_MAGIC = 0x68766174  # "tavh"
HWCFG_JSON_MAX = 4096 - 16

PINS = [
    ("sck", "SPI clock (SCL on the display)", True),
    ("mosi", "SPI data out (SDA)", True),
    ("miso", "SPI data in (SDA-O), only for the ID read", False),
    ("cs", "chip select (CS)", True),
    ("dc", "data/command (DC)", True),
    ("rst", "reset (RST)", False),
    ("bl", "backlight (BL)", False),
    ("button", "push button to GND", False),
]


class Abort(Exception):
    pass


# ---- files ---------------------------------------------------------------

def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def variants():
    out = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "variants", "*", "variant.json"))):
        v = load_json(path)
        v["id"] = os.path.basename(os.path.dirname(path))
        out[v["id"]] = v
    return out


def presets(kind):
    return [load_json(p) for p in sorted(glob.glob(os.path.join(ROOT, "presets", kind, "*.json")))]


def storage_offset(variant):
    """Offset of the "storage" partition in the variant's partition table."""
    with open(os.path.join(ROOT, variant["partitions"]), encoding="utf-8") as f:
        for line in f:
            cols = [c.strip() for c in line.split("#")[0].split(",")]
            if len(cols) >= 5 and cols[0] == "storage":
                return int(cols[3], 0), int(cols[4], 0)
    raise Abort(f"no storage partition in {variant['partitions']}")


# ---- questions -----------------------------------------------------------

def ask(prompt, default=None):
    hint = f" [{default}]" if default not in (None, "") else ""
    try:
        answer = input(f"{prompt}{hint}: ").strip()
    except EOFError:
        raise Abort("no answer")
    return answer if answer else ("" if default is None else str(default))


def ask_yes(prompt, default):
    while True:
        a = ask(prompt + " (y/n)", "y" if default else "n").lower()
        if a in ("y", "yes"):
            return True
        if a in ("n", "no"):
            return False


def choose(prompt, items, label, default=0):
    print(prompt)
    for i, item in enumerate(items, 1):
        print(f"  {i}. {label(item)}")
    while True:
        a = ask("Number", default + 1)
        if a.isdigit() and 1 <= int(a) <= len(items):
            return items[int(a) - 1]


# ---- checks --------------------------------------------------------------

def check_config(cfg, variant):
    """Returns a list of problems (refused) and a list of warnings."""
    problems, warnings = [], []
    pins = cfg.get("pins", {})
    used = {}
    for name, _, required in PINS:
        pin = pins.get(name, -1)
        if pin == -1:
            if required:
                problems.append(f"{name} is required")
            continue
        if not isinstance(pin, int):
            problems.append(f"{name}: not a GPIO number")
            continue
        if pin not in variant["gpios"]:
            problems.append(f"{name}: GPIO {pin} does not exist on the {variant['chip']}")
        elif pin in variant["reserved"]:
            problems.append(f"{name}: GPIO {pin} is taken by flash, PSRAM or USB")
        elif pin in variant["strapping"] and not (name == "button" and pin == 0):
            warnings.append(f"{name}: GPIO {pin} is a strapping pin; the board may not "
                            "start if it is pulled the wrong way at reset")
        if pin in used:
            problems.append(f"{used[pin]} and {name} are both GPIO {pin}")
        used[pin] = name
    d = cfg.get("display", {})
    mhz = d.get("spi_mhz", 40)
    if not isinstance(mhz, int) or not 1 <= mhz <= 80:
        problems.append("spi_mhz: 1 to 80")
    elif mhz > 40:
        iomux = variant["iomux_spi"]
        off = [n for n in ("sck", "mosi", "cs") if pins.get(n) != iomux[n]]
        if off:
            warnings.append(f"{mhz} MHz needs SCK, MOSI and CS on the IOMUX pins "
                            f"({iomux['sck']}, {iomux['mosi']}, {iomux['cs']}); "
                            f"{', '.join(off)} are not")
    tx = cfg.get("wifi_tx_power_qdbm", 0)
    if not isinstance(tx, int) or not 0 <= tx <= 84:
        problems.append("wifi_tx_power_qdbm: 0 to 84")
    return problems, warnings


# ---- device --------------------------------------------------------------

def list_ports():
    try:
        from serial.tools import list_ports
    except ImportError:
        raise Abort("pyserial not found: run this in the ESP-IDF shell (. export.sh)")
    return [p for p in list_ports.comports() if p.vid is not None]


def pick_port(given):
    if given:
        return given
    ports = list_ports()
    if not ports:
        raise Abort("no USB serial port found; plug the board in")
    if len(ports) == 1:
        p = ports[0]
        print(f"Port: {p.device} ({p.description}, {p.serial_number})")
        return p.device
    return choose("Serial ports:", ports,
                  lambda p: f"{p.device}  {p.description}  {p.serial_number or ''}").device


def esptool(args, dry_run, capture=False):
    cmd = [sys.executable, "-m", "esptool"] + args
    print("$ " + " ".join(cmd))
    if dry_run:
        return ""
    r = subprocess.run(cmd, text=True, capture_output=capture)
    if r.returncode != 0:
        if capture:
            sys.stdout.write(r.stdout + r.stderr)
        raise Abort("esptool failed")
    return r.stdout if capture else ""


def detect(port, dry_run):
    """Chip name, flash MB and PSRAM MB of the board on the port."""
    if dry_run:
        return None
    out = esptool(["--port", port, "--after", "hard_reset", "flash_id"], False, capture=True)
    chip = re.search(r"Chip is (ESP32-\w+)", out)
    flash = re.search(r"Detected flash size: (\d+)MB", out)
    psram = re.search(r"Embedded PSRAM (\d+)MB", out)
    if not chip:
        sys.stdout.write(out)
        raise Abort("could not tell the chip")
    info = (chip.group(1), int(flash.group(1)) if flash else 0,
            int(psram.group(1)) if psram else 0)
    print(f"Found {info[0]}, {info[1]} MB flash, {info[2]} MB PSRAM")
    return info


def pick_variant(all_variants, found, given):
    if given:
        if given not in all_variants:
            raise Abort(f"no variant {given}; known: {', '.join(all_variants)}")
        return all_variants[given]
    items = list(all_variants.values())
    if found:
        chip, flash, psram = found
        items = [v for v in items if v["chip"] == chip and v["flash_mb"] <= flash
                 and v["psram_mb"] <= psram]
        if not items:
            raise Abort(f"no firmware variant for a {chip} with {flash} MB flash and "
                        f"{psram} MB PSRAM yet")
        # most memory the board has
        items.sort(key=lambda v: (v["psram_mb"], v["flash_mb"]), reverse=True)
    if len(items) == 1:
        print(f"Variant: {items[0]['id']} ({items[0]['name']})")
        return items[0]
    return choose("Firmware variant:", items, lambda v: f"{v['id']}  {v['name']}")


# ---- building the config -------------------------------------------------

def ranges(nums):
    """[1, 2, 3, 5, 7, 8] -> "1-3, 5, 7-8"."""
    out, nums = [], sorted(nums)
    i = 0
    while i < len(nums):
        j = i
        while j + 1 < len(nums) and nums[j + 1] == nums[j] + 1:
            j += 1
        out.append(str(nums[i]) if i == j else f"{nums[i]}-{nums[j]}")
        i = j + 1
    return ", ".join(out)


def ask_pins(start, variant):
    """Asks every pin; only GPIOs that exist, are not reserved and are not
    already given to another pin are taken."""
    pins = dict(start)
    taken = {}
    print(f"GPIO numbers. Not usable on the {variant['chip']} (flash, PSRAM, USB): "
          f"{ranges(variant['reserved'])}.")
    for name, what, required in PINS:
        free = [g for g in variant["gpios"] if g not in variant["reserved"] and g not in taken]
        print(f"  {name}: {what}. Free: {ranges(free)}"
              + ("" if required else "; -1 = not connected"))
        while True:
            a = ask(f"  {name}", pins.get(name, -1))
            try:
                n = int(a)
            except ValueError:
                print("    a GPIO number")
                continue
            if n == -1:
                if required:
                    print("    required")
                    continue
                pins[name] = -1
                break
            if n not in variant["gpios"]:
                print(f"    no GPIO {n} on the {variant['chip']}")
                continue
            if n in variant["reserved"]:
                print(f"    GPIO {n} is taken by flash, PSRAM or USB")
                continue
            if n in taken:
                print(f"    GPIO {n} is already {taken[n]}")
                continue
            if n in variant["strapping"] and not (name == "button" and n == 0):
                print(f"    GPIO {n} is a strapping pin; the board may not start if it is "
                      "pulled the wrong way at reset")
                if not ask_yes("    use it anyway", False):
                    continue
            taken[n] = name
            pins[name] = n
            break
    return pins


def ask_config(variant):
    boards = [b for b in presets("boards") if b["variant"] == variant["id"]]
    custom = {"board": "custom"}
    board = choose("Board wiring:", boards + [custom],
                   lambda b: b["board"] if b is not custom else "custom: enter the pins")
    cfg = {"variant": variant["id"]}
    if board is custom:
        name = ask("Name for this wiring", "custom")
        start = boards[0]["pins"] if boards else {}
        cfg["board"] = name
        cfg["pins"] = ask_pins(start, variant)
        cfg["wifi_tx_power_qdbm"] = int(ask("Wi-Fi transmit power limit in 0.25 dBm "
                                            "(0 = chip default, 34 = 8.5 dBm)", 0))
    else:
        cfg["board"] = board["board"]
        cfg["pins"] = dict(board["pins"])
        cfg["wifi_tx_power_qdbm"] = board.get("wifi_tx_power_qdbm", 0)
        if ask_yes("Change any pins", False):
            cfg["pins"] = ask_pins(cfg["pins"], variant)

    displays = presets("displays")
    disp = choose("Display:", displays, lambda d: d["name"])
    d = dict(disp["display"])
    if ask_yes("Change the display options (clock, colour order, mirror)", False):
        d["spi_mhz"] = int(ask("  SPI clock in MHz", d.get("spi_mhz", 40)))
        for key, what in (("bgr", "colours in BGR order"), ("invert", "invert colours"),
                          ("mirror_x", "mirror left-right"), ("mirror_y", "mirror top-bottom")):
            d[key] = ask_yes(f"  {what}", d.get(key, False))
    cfg["display"] = d
    return cfg


def summary(cfg, variant):
    p = cfg["pins"]
    d = cfg["display"]
    print()
    print(f"Variant  {variant['id']} ({variant['name']})")
    print(f"Board    {cfg['board']}")
    print(f"Display  {d['driver']} at {d.get('spi_mhz', 40)} MHz, "
          + ", ".join(k for k in ("bgr", "invert", "mirror_x", "mirror_y") if d.get(k)))
    print("Pins     " + "  ".join(f"{n} {p.get(n, -1)}" for n, _, _ in PINS))
    tx = cfg.get("wifi_tx_power_qdbm", 0)
    print(f"Wi-Fi    {'chip default' if not tx else f'max {tx / 4:g} dBm'}")
    print()


def blob(cfg):
    """The config sector as main/hwcfg.c reads it."""
    data = json.dumps({k: v for k, v in cfg.items() if k != "variant"},
                      separators=(",", ":")).encode()
    if len(data) > HWCFG_JSON_MAX:
        raise Abort("hardware config too large")
    return struct.pack("<IIII", HWCFG_MAGIC, len(data), zlib.crc32(data), 0) + data


# ---- main ----------------------------------------------------------------

def run(cmd, dry_run):
    print("$ " + " ".join(cmd))
    if not dry_run and subprocess.run(cmd, cwd=ROOT).returncode != 0:
        raise Abort(f"{cmd[0]} failed")


def main():
    ap = argparse.ArgumentParser(description="Build, flash and configure a tavle device.")
    ap.add_argument("--port", help="serial port, for example /dev/ttyACM0")
    ap.add_argument("--variant", help="firmware variant, for example c3 or s3-n16r8")
    ap.add_argument("--config", help="hardware config file to use instead of questions")
    ap.add_argument("--save", default="tavle-hw.json",
                    help="where to save the answers (default tavle-hw.json)")
    ap.add_argument("--config-only", action="store_true",
                    help="write only the hardware config, no build or flash")
    ap.add_argument("--dry-run", action="store_true", help="show the commands, run none")
    a = ap.parse_args()

    all_variants = variants()
    cfg = load_json(a.config) if a.config else None
    if cfg and not a.variant:
        a.variant = cfg.get("variant")

    port = pick_port(a.port) if not a.dry_run or a.port else "PORT"
    found = detect(port, a.dry_run) if port != "PORT" else None
    variant = pick_variant(all_variants, found, a.variant)
    if found and found[0] != variant["chip"]:
        raise Abort(f"the board is a {found[0]}, the variant is for {variant['chip']}")

    if cfg is None:
        cfg = ask_config(variant)
    cfg["variant"] = variant["id"]

    problems, warnings = check_config(cfg, variant)
    summary(cfg, variant)
    for w in warnings:
        print("warning: " + w)
    if problems:
        for p in problems:
            print("refused: " + p)
        raise Abort("fix the config and run again")

    if not a.config:
        with open(a.save, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2)
            f.write("\n")
        print(f"Saved to {a.save}; next time: python tools/tavle.py --config {a.save}")
    if not a.dry_run and not ask_yes("Go ahead", True):
        raise Abort("stopped")

    if not a.config_only:
        idf = shutil.which("idf.py")
        if not idf:
            raise Abort("idf.py not found: run this in the ESP-IDF shell (. export.sh)")
        build = ["idf.py", "-B", f"build/{variant['id']}", f"-DVARIANT={variant['id']}"]
        run(build + ["build"], a.dry_run)
        run(build + ["-p", port, "flash"], a.dry_run)

    start, size = storage_offset(variant)
    if size < HWCFG_OFFSET + 0x1000:
        raise Abort("storage partition too small for the hardware config")
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(blob(cfg))
        path = f.name
    try:
        esptool(["--chip", variant["chip"].lower().replace("-", ""), "--port", port,
                 "--after", "hard_reset", "write_flash", hex(start + HWCFG_OFFSET), path],
                a.dry_run)
    finally:
        os.unlink(path)
    print("Done. The device restarts with this hardware config.")


if __name__ == "__main__":
    try:
        main()
    except Abort as e:
        print(f"tavle: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print()
        sys.exit(130)
