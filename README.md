# PC Monitor – 1.75" round AMOLED desk display

Live CPU, GPU, RAM, temperature, network, SSD and power data from a Windows PC, shown on a
**Waveshare ESP32-S3-Touch-AMOLED-1.75** round touchscreen that is connected with one USB-C cable.

<!-- Add photos or a short video here, for example:  ![The 5 screens](docs/screens.jpg) -->

> ## ⚠️ Please read first – this is inspiration, not a finished product
>
> This is a hobby project that I built for **my own PC**. It is **not plug-and-play**.
> The code is shared so you can use it, learn from it and adapt it to your own computer.
> Expect to change sensor names, texts and limits to match your hardware.
> No support and no warranty – use it at your own risk.
>
> **Built and tested on:** AMD Ryzen 9 5950X, ASUS ROG STRIX X570-F, NVIDIA GeForce RTX 5070 (12 GB),
> 32 GB RAM, one NVMe SSD, Windows 11.
> Other CPUs, graphics cards, motherboards and drives use different sensor names, so some values
> will need adjusting (see [Adapting it to your PC](#adapting-it-to-your-pc)).

## The 5 screens

| # | Screen | Shows |
|---|--------|-------|
| 1 | Gaming | CPU and GPU usage on animated arcs, plus both temperatures |
| 2 | Memory & Net | download / upload speed, RAM and VRAM usage |
| 3 | Power Draw | CPU and GPU watts and a total estimate |
| 4 | Temperatures | CPU, CCD1, CCD2, CPU socket, GPU core and memory, RAM sticks, SSD, chipset, motherboard, peak CPU / GPU temperature |
| 5 | Network, SSD & Power | network and disk speed, SSD usage bar with GB used / total, CPU and GPU power arcs |

**Controls:** swipe left / right = change screen · slide a finger up / down = brightness ·
double tap = full brightness · turn the display = auto rotation.

**Other features:** smooth arcs and bars · last values are kept for 6 seconds when the data stops for a
moment (for example while a heavy program starts) · speed units switch between KB, MB and GB by
themselves · the PC script can run silently in the background and start with Windows.

## How it works

```
LibreHardwareMonitor  ──(local web page, JSON)──►  pc_monitor.pyw  ──(USB, one line per second)──►  ESP32-S3  ──►  AMOLED
 reads all sensors         http://127.0.0.1:8085        Python script                                 LVGL screens
```

1. **LibreHardwareMonitor** runs on the PC and publishes every sensor value on a local web page.
2. **`pc_monitor.pyw`** reads that page once per second, picks the values we want and sends them
   to the ESP32 as one line of text.
3. The **firmware** reads the line and draws the screens (LVGL 8.3, design made in SquareLine Studio).

## What you need

**Hardware**
- Waveshare ESP32-S3-Touch-AMOLED-1.75
- USB-C **data** cable (not a charge-only cable)
- A stand or case of your choice (optional)

**Software (all free)**
- Windows 10 / 11
- [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)
- Python 3 with the `pyserial` package
- [VS Code](https://code.visualstudio.com/) with the PlatformIO extension (to flash the ESP32)
- SquareLine Studio – only if you want to change the design

## Quick start

### 1. LibreHardwareMonitor
1. Open PowerShell and run: `winget install LibreHardwareMonitor.LibreHardwareMonitor`
   (this also installs the PawnIO driver it needs for CPU temperature and power).
2. Start it as administrator: type `LibreHardwareMonitor` in PowerShell and click **Yes**.
3. In its **Options** menu tick **Remote Web Server → Run**. For automatic start also tick
   **Run On Windows Startup**, **Start Minimized** and **Minimize on Close**.
4. Check that `http://127.0.0.1:8085` shows a list of sensors in your browser.

> LibreHardwareMonitor needs administrator rights and loads a driver. It is a well-known
> open-source tool, but please review it and install it at your own risk. Do **not** open its
> port in your firewall – the script runs on the same PC.

### 2. Python
```
winget install -e --id Python.Python.3.13
python -m pip install pyserial
```
Open a new PowerShell window afterwards so the new commands are found.

### 3. Flash the ESP32
1. Open the `pc_monitor` folder in VS Code (it is a PlatformIO project).
2. Click **Upload** in the PlatformIO toolbar.
3. **Stop the PC script before every upload** (see below) – only one program at a time can use the
   USB port of the ESP32.

### 4. Run the script
- Quick test with messages: `python pc_monitor.pyw`
  (the screen shows values after a few seconds; stop with Ctrl+C).
- Silent: double-click `pc_monitor.pyw` (a `.pyw` file runs without a window).
  Messages go to `pc_monitor.log` next to the script.
- Start with Windows: press **Win + R**, type `shell:startup`, and put a shortcut to
  `pc_monitor.pyw` in that folder.
- Stop the silent copy: `Stop-Process -Name pythonw` in PowerShell.

Only one copy of the script can run at a time.

## Adapting it to your PC

1. **Find your sensors.** Run `python pc_monitor.pyw --dump`. It creates `sensor_dump.txt` with every
   sensor LibreHardwareMonitor sees (device, type, name, value).
2. **Match the names.** In `pc_monitor.pyw` the function `collect()` picks the sensors by name. Change or
   add names so they match your hardware. For example the CPU temperature tries
   `Core (Tctl/Tdie)`, `CPU Package` and `Core Max`; the graphics card is NVIDIA first, then AMD,
   then Intel; storage uses the first drive listed; the network value comes from the busiest adapter.
3. **Texts inside the design.** Some texts are part of the background pictures (for example
   "32 GB CAP" and "12 GB CAP" on screen 2). Change them in SquareLine Studio.
4. **Firmware settings** at the top of `pc_monitor/src/main.cpp`:

| Setting | Default | Meaning |
|---------|---------|---------|
| `CPU_POWER_MAX_W` / `GPU_POWER_MAX_W` | 150 / 250 | watts at which the power arcs are full |
| `ARC_ANIM_MS` | 600 | time an arc or bar takes to glide to a new value |
| `DISPLAY_DELAY_MS` | 200 | numbers appear this long after the arc starts moving |
| `VALUE_HOLD_MS` | 6000 | how long the last value stays when data stops |
| `DISPLAY_FPS` | 30 | redraw rate (lower it if the arcs stutter) |

### Values sent to the display
The script sends one line per second, for example `PC,CPU:3,CPUT:35.4,GPU:1,...`.
A value that cannot be read is simply left out and shows `--`.

| Key | Meaning | Unit |
|-----|---------|------|
| `CPU`, `GPU`, `RAM` | usage | % |
| `CPUT`, `GPUT` | CPU / GPU temperature | °C |
| `CCD1`, `CCD2` | CPU chiplet temperatures (Ryzen) | °C |
| `CPUS` | CPU socket temperature (motherboard sensor) | °C |
| `GPUM` | GPU memory temperature | °C |
| `PKCPU`, `PKGPU` | peak CPU / GPU temperature since LibreHardwareMonitor started | °C |
| `DIMM1`, `DIMM2` | RAM stick temperatures | °C |
| `SSDT`, `CHIP`, `MOBO` | SSD, chipset, motherboard temperature | °C |
| `PCPU`, `PGPU` | CPU / GPU power | W |
| `PTOT` | CPU + GPU power (an estimate, **not** the power of the whole PC) | W |
| `RAMU`, `RAMT` | RAM used / total | GB |
| `VRMU`, `VRMT`, `VRAM` | video memory used / total / used in % | GB, GB, % |
| `DSKU`, `DSKF`, `DSKT` | SSD used in % / free / total size | %, GB, GB |
| `DSKR`, `DSKW` | disk read / write speed | KB/s |
| `NETD`, `NETU` | network download / upload speed | KB/s |

If you add values, keep the whole line under about 700 characters. The firmware raises the USB
receive buffer to 2048 bytes; the default of 256 bytes is too small for a line this long and
silently loses its end.

## Changing the design (SquareLine Studio)

1. In the SquareLine project settings use **LVGL 8.3** and **16-bit colour**, screen size 466 × 466.
2. After every export run the helper tool on the `src/ui` folder of the firmware:
   `python fix_squareline_export.py <path to src\ui>`
   It fixes the include line and converts the images to the LVGL 8.3 format (SquareLine writes them in a
   newer format that LVGL 8.3 cannot read). It is safe to run twice.
3. The values are connected to the widgets in the function `updateUI()` in `main.cpp`.

## Troubleshooting

| Problem | What to check |
|---------|---------------|
| Screen shows `--` everywhere | Is the script running? Is LibreHardwareMonitor running as administrator with **Remote Web Server → Run** ticked? Look at `pc_monitor.log`, or run the script in a window and read its messages. |
| Upload fails ("could not open COM…") | Another program is using the port. Stop the script (`Stop-Process -Name pythonw`), close serial monitors, and try again. |
| CPU temperature or power missing | LibreHardwareMonitor must run as administrator and the PawnIO driver must be installed. |
| "Another copy of PC Monitor is already running" | A silent copy is already running. Stop it with `Stop-Process -Name pythonw`. |
| Numbers look wrong on a non-English Windows | The script handles decimal commas. Run with `--dump` and check the sensor values. |

## Known limitations
- Windows only (LibreHardwareMonitor).
- The ESP32 restarts once whenever the script connects to its USB port. That is normal.
- Total power is CPU + GPU only.
- Tuned for one specific PC; other hardware needs changes in the script and in the design.

## Built with
[LVGL](https://lvgl.io/) 8.3 · [SquareLine Studio](https://squareline.io/) ·
[GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX) ·
[SensorLib](https://github.com/lewisxhe/SensorLib) ·
[LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) ·
PlatformIO · pyserial.
Each of them has its own license.

## License
Free to use, modify and share. See the `LICENSE` file.
