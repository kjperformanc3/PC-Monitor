"""
PC Monitor - PC side sender (version 4)

What this script does, in plain words:
  1. Every second it asks LibreHardwareMonitor (which must be running as
     administrator with its web server switched on) for all sensor values.
  2. It picks out the values we want (CPU, GPU, RAM, temperatures, power,
     network, disk...) and puts them in ONE line of text.
  3. It finds the ESP32 on its own and sends that line over the USB cable.
     If no ESP32 is plugged in, it just prints the line on screen.

How to run it (in PowerShell, in the folder where this file is saved):
    python pc_monitor.py

If a value is missing or looks wrong, run this to save a list of every
sensor LibreHardwareMonitor can see (it creates sensor_dump.txt):
    python pc_monitor.py --dump

Stop the script with Ctrl+C. Stop it BEFORE flashing the ESP32, because only
one program at a time can use the ESP32's USB port.

Running without a window (for start-up with Windows):
    pythonw pc_monitor.py
In that mode nothing is shown; status messages go to pc_monitor.log (next to
this file). Only one copy of the script can run at a time.

The line it sends looks like this (only values that were found are included):
    PC,CPU:6,CPUT:38.1,GPU:1,GPUT:39.3,RAM:90,...
"""

import json
import os
import re
import sys
import time
import urllib.request

import serial
from serial.tools import list_ports

# ----------------------------------------------------------------------
# SETTINGS - safe to change
# ----------------------------------------------------------------------
LHM_URL = "http://127.0.0.1:8085/data.json"   # LibreHardwareMonitor web server
SEND_EVERY_SECONDS = 1.0                      # how often a line is sent
PORT_OVERRIDE = ""                            # e.g. "COM7"; empty = find it automatically
BAUD = 115200                                 # ignored by native USB, but required
ESPRESSIF_VID = 0x303A                        # USB maker ID of the ESP32-S3 board
# ----------------------------------------------------------------------

# Running with pythonw (no window) has no screen to print to.
HAS_CONSOLE = sys.stdout is not None
LOG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pc_monitor.log")
LOG_MAX_BYTES = 200000          # the log starts again from empty when it gets this big

# Every value name we try to send. Used to tell you what was not found.
EXPECTED_KEYS = [
    "CPU", "CPUT", "CCD1", "CCD2", "CPUS", "PCPU",
    "GPU", "GPUT", "GPUM", "PGPU",
    "RAM", "DIMM1", "DIMM2",
    "SSDT", "DSKU", "DSKF", "DSKR", "DSKW",
    "NETD", "NETU",
    "CHIP", "MOBO",
    "PTOT",
    "RAMU", "RAMT", "VRMU", "VRMT", "VRAM",
    "PKCPU", "PKGPU", "DSKT",
]

CPU_IDS = ("/amdcpu", "/intelcpu")

# Older LibreHardwareMonitor versions do not label the sensor type, so we
# fall back to the name of the group the sensor sits in.
GROUP_TO_TYPE = {
    "Voltages": "voltage", "Clocks": "clock", "Temperatures": "temperature",
    "Load": "load", "Fans": "fan", "Powers": "power", "Data": "data",
    "Throughput": "throughput", "Levels": "level", "Controls": "control",
    "Factors": "factor",
}


# ----------------------------------------------------------------------
# Messages and "only one copy" guard
# ----------------------------------------------------------------------
_mutex_handle = None


def say(message):
    """Show a message in the window. When there is no window, write it to the
    log file instead."""
    if HAS_CONSOLE:
        print(message)
        return
    try:
        if os.path.exists(LOG_FILE) and os.path.getsize(LOG_FILE) > LOG_MAX_BYTES:
            os.remove(LOG_FILE)
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + message + "\n")
    except Exception:
        pass


def already_running():
    """True if another copy of this script is already running (Windows only)."""
    global _mutex_handle
    try:
        import ctypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        _mutex_handle = kernel32.CreateMutexW(None, False, "Local\\PCMonitorSender")
        if ctypes.get_last_error() == 183:   # 183 = "already exists"
            return True
    except Exception:
        pass   # not Windows, or no permission: just carry on
    return False


# ----------------------------------------------------------------------
# Reading LibreHardwareMonitor
# ----------------------------------------------------------------------
def fetch():
    """Download data.json from LibreHardwareMonitor and return it as Python data."""
    # ProxyHandler({}) = never use a proxy for this local request.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(LHM_URL, timeout=4) as response:    # LibreHardwareMonitor can be slow when the PC is busy
        body = response.read()
    return json.loads(body.decode("utf-8-sig"))


# Speeds are converted to bytes per second (LibreHardwareMonitor counts 1 KB = 1024 B).
SPEED_UNITS = {"B/s": 1.0, "KB/s": 1024.0, "MB/s": 1024.0 ** 2, "GB/s": 1024.0 ** 3}


def number_from(node):
    """Get a plain number from a sensor.

    On this version of LibreHardwareMonitor, RawValue is TEXT such as
    "35,4 °C" or "178343,4 B/s" (decimal comma + unit), so we cut the number
    out of the text and turn the comma into a dot. Newer versions may give a
    real number instead; that works too."""
    return parse_number(node["RawValue"] if "RawValue" in node else node.get("Value", ""))


def max_from(node):
    """The highest value LibreHardwareMonitor has seen for this sensor since it started."""
    raw = node["RawMax"] if "RawMax" in node else node.get("Max")
    return None if raw is None else parse_number(raw)


def parse_number(raw):
    if isinstance(raw, bool):
        return None
    if isinstance(raw, (int, float)):
        return float(raw)
    match = re.match(r"\s*(-?\d+(?:[.,]\d+)?)\s*(.*)$", str(raw))
    if not match:
        return None  # no reading right now (for example "-")
    number = float(match.group(1).replace(",", "."))
    return number * SPEED_UNITS.get(match.group(2).strip(), 1.0)


def flatten(node, group="", hw_name="", out=None):
    """Walk the whole sensor tree and return a flat list of sensors."""
    if out is None:
        out = []
    if "HardwareId" in node:
        hw_name = node.get("Text", "")
    children = node.get("Children") or []
    if children:
        for child in children:
            flatten(child, node.get("Text", ""), hw_name, out)
        return out
    if "HardwareId" in node:
        return out  # a device that has no sensors
    if "Value" not in node and "RawValue" not in node:
        return out
    sensor_id = node.get("SensorId") or ""
    # "/amdcpu/0/temperature/2" -> device id "/amdcpu/0"
    hw = sensor_id.rsplit("/", 2)[0] if sensor_id.count("/") >= 3 else ""
    kind = (node.get("Type") or GROUP_TO_TYPE.get(group, "")).lower()
    out.append({
        "hw": hw,
        "hw_name": hw_name,
        "type": kind,
        "name": node.get("Text", ""),
        "value": number_from(node),
        "max": max_from(node),
        "sid": sensor_id,
    })
    return out


# ----------------------------------------------------------------------
# Picking the values we want
# ----------------------------------------------------------------------
def collect(sensors):
    """Return {key: (number, decimals)} for every value we could find."""
    values = {}

    def put(key, number, decimals):
        if number is not None:
            values[key] = (number, decimals)

    def get(kind, names, where=None, field="value"):
        """First sensor of this type whose name is in `names` (tried in order).
        field="max" returns the highest value seen since LibreHardwareMonitor started."""
        for name in names:
            for s in sensors:
                if s["value"] is None or s["type"] != kind or s["name"] != name:
                    continue
                if where is not None and not where(s):
                    continue
                return s[field]
        return None

    def kb(bytes_per_second):
        return None if bytes_per_second is None else bytes_per_second / 1024.0

    # Which graphics card to use: NVIDIA first, then AMD, then Intel.
    gpu_prefix = None
    for prefix in ("/gpu-nvidia", "/gpu-amd", "/gpu-intel"):
        if any(s["hw"].startswith(prefix) for s in sensors):
            gpu_prefix = prefix
            break

    def is_cpu(s):
        return s["hw"].startswith(CPU_IDS)

    def is_gpu(s):
        return gpu_prefix is not None and s["hw"].startswith(gpu_prefix)

    def is_board(s):  # motherboard chips: not the CPU, not a graphics card
        return not s["hw"].startswith(CPU_IDS + ("/gpu",))

    def is_ram(s):  # physical memory ("Total Memory"), not "Virtual Memory"
        return s["hw"] == "/ram" or s["hw_name"] == "Total Memory"

    # --- CPU ---
    put("CPU", get("load", ["CPU Total"], is_cpu), 0)
    put("CPUT", get("temperature", ["Core (Tctl/Tdie)", "CPU Package", "Core Max"], is_cpu), 1)
    put("CCD1", get("temperature", ["CCD1 (Tdie)"], is_cpu), 1)
    put("CCD2", get("temperature", ["CCD2 (Tdie)"], is_cpu), 1)
    put("CPUS", get("temperature", ["CPU"], is_board), 1)  # socket sensor on the motherboard
    put("PCPU", get("power", ["Package", "CPU Package"], is_cpu), 1)
    put("PKCPU", get("temperature", ["Core (Tctl/Tdie)", "CPU Package", "Core Max"], is_cpu, "max"), 1)  # peak CPU temp

    # --- GPU ---
    put("GPU", get("load", ["GPU Core"], is_gpu), 0)
    put("GPUT", get("temperature", ["GPU Core"], is_gpu), 1)
    put("GPUM", get("temperature", ["GPU Memory Junction", "GPU Memory"], is_gpu), 1)
    put("PGPU", get("power", ["GPU Package", "GPU Power", "GPU Board Power"], is_gpu), 1)
    vram_used = get("smalldata", ["GPU Memory Used"], is_gpu)   # MB
    vram_total = get("smalldata", ["GPU Memory Total"], is_gpu)  # MB
    put("VRMU", None if vram_used is None else vram_used / 1024.0, 1)    # GB
    put("VRMT", None if vram_total is None else vram_total / 1024.0, 1)  # GB
    put("VRAM", get("load", ["GPU Memory"], is_gpu), 0)         # video memory used in %
    put("PKGPU", get("temperature", ["GPU Core"], is_gpu, "max"), 1)     # peak GPU temp

    # --- RAM ---
    put("RAM", get("load", ["Memory"], is_ram), 0)
    ram_used = get("data", ["Memory Used"], is_ram)             # GB
    ram_free = get("data", ["Memory Available"], is_ram)        # GB
    put("RAMU", ram_used, 1)
    if ram_used is not None and ram_free is not None:
        put("RAMT", ram_used + ram_free, 1)                     # total RAM in GB
    dimms = [s for s in sensors
             if s["type"] == "temperature" and s["name"].startswith("DIMM")
             and s["value"] is not None]
    for number, s in enumerate(dimms[:4], start=1):
        put("DIMM%d" % number, s["value"], 1)

    # --- Storage (uses the first drive LibreHardwareMonitor lists) ---
    disk_hw = None
    for s in sensors:
        if s["type"] == "load" and s["name"] == "Used Space":
            disk_hw = s["hw"]
            break
    if disk_hw is not None:
        def on_disk(s):
            return s["hw"] == disk_hw
        put("DSKU", get("load", ["Used Space"], on_disk), 0)
        put("DSKF", get("data", ["Free Space"], on_disk), 0)
        put("DSKT", get("data", ["Total Space"], on_disk), 0)    # drive size in GB
        put("DSKR", kb(get("throughput", ["Read Rate"], on_disk)), 0)
        put("DSKW", kb(get("throughput", ["Write Rate"], on_disk)), 0)
        put("SSDT", get("temperature", ["Composite Temperature", "Temperature"], on_disk), 1)

    # --- Network (busiest adapter, in KB/s) ---
    downs = [s["value"] for s in sensors if s["type"] == "throughput"
             and s["name"] == "Download Speed" and s["value"] is not None]
    ups = [s["value"] for s in sensors if s["type"] == "throughput"
           and s["name"] == "Upload Speed" and s["value"] is not None]
    if downs:
        put("NETD", kb(max(downs)), 0)
    if ups:
        put("NETU", kb(max(ups)), 0)

    # --- Motherboard ---
    put("CHIP", get("temperature", ["Chipset"], is_board), 1)
    put("MOBO", get("temperature", ["Motherboard"], is_board), 1)

    # --- Power estimate: CPU + GPU (only when both are known) ---
    if "PCPU" in values and "PGPU" in values:
        put("PTOT", values["PCPU"][0] + values["PGPU"][0], 1)

    return values


def build_line(values):
    parts = ["%s:%.*f" % (key, decimals, number) for key, (number, decimals) in values.items()]
    return "PC," + ",".join(parts)


# ----------------------------------------------------------------------
# Talking to the ESP32
# ----------------------------------------------------------------------
def find_esp_port():
    """Return the COM port of the ESP32, or None if it is not plugged in."""
    if PORT_OVERRIDE:
        return PORT_OVERRIDE
    for port in list_ports.comports():
        if port.vid == ESPRESSIF_VID:
            return port.device
    return None


def open_port(port):
    # dtr/rts are switched off BEFORE opening, so that opening the port
    # does not reset the ESP32.
    s = serial.Serial()
    s.port = port
    s.baudrate = BAUD
    s.timeout = 0
    s.write_timeout = 1
    s.dtr = False
    s.rts = False
    s.open()
    return s


# ----------------------------------------------------------------------
# Extra tool: list every sensor to a file
# ----------------------------------------------------------------------
def dump():
    sensors = flatten(fetch())
    path = os.path.abspath("sensor_dump.txt")
    with open(path, "w", encoding="utf-8") as f:
        f.write("device id | device name | type | sensor name | value\n")
        for s in sensors:
            f.write("%s | %s | %s | %s | %s\n" % (
                s["hw"], s["hw_name"], s["type"], s["name"], s["value"]))
    print("Saved %d sensors to: %s" % (len(sensors), path))


# ----------------------------------------------------------------------
# Main loop
# ----------------------------------------------------------------------
def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    if "--dump" in sys.argv:
        try:
            dump()
        except Exception as error:
            print("Could not read LibreHardwareMonitor: %s" % error)
        return

    if already_running():
        say("Another copy of PC Monitor is already running, so this one closes. "
            "(To run it here instead, end 'Python' in Task Manager first.)")
        return

    say("PC Monitor sender started. Press Ctrl+C to stop.")
    last_message = {}

    def report(topic, message):
        """Show a status message only when it changes (no spam)."""
        if last_message.get(topic) != message:
            last_message[topic] = message
            say(message)

    ser = None
    checked_missing = False
    next_time = time.monotonic()

    try:
        while True:
            # 1) Read the sensors
            line = None
            try:
                values = collect(flatten(fetch()))
                line = build_line(values)
                report("lhm", "LibreHardwareMonitor connected.")
                if not checked_missing:
                    checked_missing = True
                    missing = [k for k in EXPECTED_KEYS if k not in values]
                    if missing:
                        say("Not found (will be left out): " + ", ".join(missing))
                        say("Tip: run  python pc_monitor.py --dump  to see what is available.")
            except Exception as error:
                report("lhm", "Cannot read LibreHardwareMonitor (%s). Is it running as "
                              "administrator with Options > Remote Web Server > Run on?" % error)

            # 2) Make sure we are connected to the ESP32
            if ser is None:
                port = find_esp_port()
                if port:
                    try:
                        ser = open_port(port)
                        report("esp", "Connected to ESP32 on %s." % port)
                    except Exception as error:
                        ser = None
                        report("esp", "ESP32 found on %s but cannot open it (%s). "
                                      "Is another program using it?" % (port, error))
                else:
                    report("esp", "No ESP32 found yet - waiting for it to be plugged in.")

            # 3) Send the line, and show anything the ESP32 says back
            if ser is not None and line is not None:
                try:
                    ser.write((line + "\n").encode("ascii"))
                    waiting = ser.in_waiting
                    if waiting:
                        text = ser.read(waiting).decode("utf-8", "replace").strip()
                        # the regular "OK ..." heartbeat is only shown in a window
                        if text and (HAS_CONSOLE or not text.startswith("OK")):
                            say("ESP32 says: " + text)
                except Exception as error:
                    report("esp", "Lost connection to the ESP32 (%s). Looking for it again..." % error)
                    try:
                        ser.close()
                    except Exception:
                        pass
                    ser = None

            if line is not None and HAS_CONSOLE:
                print(time.strftime("%H:%M:%S"), line)

            # 4) Wait until the next second
            next_time += SEND_EVERY_SECONDS
            delay = next_time - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            else:
                next_time = time.monotonic()
    except KeyboardInterrupt:
        say("Stopped.")
    finally:
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass


if __name__ == "__main__":
    main()
