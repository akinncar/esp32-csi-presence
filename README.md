# ESP32 CSI presence sensor

A WiFi-based presence sensor: an ESP32-S3 detects whether anyone is in the room
(**0 vs 1+ people**) without a camera or motion sensor, using WiFi **Channel
State Information (CSI)**.

<img width="310"  alt="image" src="https://github.com/user-attachments/assets/0580c776-6856-4ecf-add5-df2be8f707eb" />
<img width="310"  alt="image" src="https://github.com/user-attachments/assets/abf9810d-212d-4d3b-aaaa-7b585e6d59c7" />
<br />
<img width="310" alt="WhatsApp Image 2026-10-14 at 20 37 34" src="https://github.com/user-attachments/assets/e0c0b1c6-8a1e-4c12-bfb0-1a0b847f2a0b" />
<img width="310" alt="WhatsApp Image 2026-10-14 at 20 37 34 (1)" src="https://github.com/user-attachments/assets/159a40ad-5c36-46f8-b672-f7b5dc66a444" />

The board joins your WiFi and pings the router ~100 times per second. Every reply
carries CSI, a per-subcarrier snapshot of how the signal travelled through the
room. People moving change those paths, so the signal fluctuates more. The board
measures that fluctuation, compares it with a calibrated empty-room baseline, and
decides on its own. It only needs USB power, no computer.

## Features

- **RGB LED indicator:** red = someone present, off = empty.
- **Web dashboard** at `http://presence.local/`: live status, a motion graph
  against the threshold, last movement, WiFi signal and sample rate. It also
  has a sensitivity slider and a calibrate button.
- **One-button calibration:** press **BOOT**, leave the room, done. Calibration
  is stored in flash.
- **WPA2 and WPA3** networks. The WiFi password is never stored in the repo.
- **Raw data streaming** over USB, for experiments and future model training.

| LED | Meaning |
|---|---|
| Red | Someone present |
| Off | Empty |
| Blinking yellow | Calibration countdown (leave the room, 10 s) |
| Blinking blue | Recording the empty room (30 s) |

## Hardware

- ESP32-S3 dev board with the onboard RGB LED on GPIO48 (e.g. ESP32-S3-DevKitC-1,
  N16R8 clones). Other ESP32 boards work with small changes to `platformio.ini`.
- A 2.4 GHz WiFi network.

## Project layout

```
firmware/
  platformio.ini      PlatformIO build config
  wifi_env.py         Passes WiFi credentials from env vars into the build
  src/main.cpp        WiFi, CSI capture, LED, button, web server
  src/detector.h      Presence detection and calibration
  src/web_ui.h        Dashboard page (served by the board)
presence.py           Optional Mac/PC tool: live readout and raw data logging
requirements.txt      Python tools (PlatformIO, esptool, pyserial, numpy)
```

## Getting started

### 1. Install the tools

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

### 2. Flash the firmware

Plug the board in over USB. The first flash needs your WiFi details. The
password is typed at a prompt, so it ends up in neither a file nor your
shell history:

```bash
read -rs "WIFI_PASS?WiFi password: " && echo && WIFI_SSID="YourNetwork" WIFI_PASS="$WIFI_PASS" .venv/bin/pio run -d firmware -t upload; unset WIFI_PASS
```

(`read -rs "VAR?prompt"` is zsh syntax. In bash, use `read -rsp "WiFi password: " WIFI_PASS`.)

The board saves the WiFi details, so later updates don't need them:

```bash
.venv/bin/pio run -d firmware -t upload
```

The port is auto-detected. Add `--upload-port <port>` if you have several boards.

### 3. Calibrate

On first boot the board calibrates automatically. **Leave the room as soon as
flashing finishes** and wait until the LED stops blinking (~40 s). Recalibrate at
any time with the BOOT button or from the dashboard.

### 4. Use it

Open `http://presence.local/` on any device on the same WiFi. If `.local` names
don't resolve on your network, use the IP address the board prints over serial.

## Mac/PC tool (optional)

`presence.py` streams the raw CSI over USB and runs the same detection on the
computer. It's useful for tuning, and for recording labelled data:

```bash
.venv/bin/python presence.py --calibrate      # live readout in the terminal
.venv/bin/python presence.py --log data.csv   # also record raw samples
```

## How detection works

1. Each CSI sample is converted to amplitudes for the active subcarriers and
   normalised by its mean. This cancels the radio's automatic gain changes.
2. Over a 2-second window (~200 samples), the standard deviation of each
   subcarrier is averaged into a single **motion score**.
3. Calibration records 30 s of an empty room. The **threshold** is the 95th
   percentile of that score × the sensitivity margin (default 1.5×).
4. A score above the threshold means presence. The state is held for 10 s after
   the last movement, so brief still moments don't flip it to empty.

## Tips and limitations

- Place the board so the room is **between it and the router**. That path is
  where people affect the signal most.
- Recalibrate after moving the board, the router or large furniture.
- People sitting perfectly still are hard to detect with CSI. Increase `HOLD_MS`
  in `firmware/src/detector.h` if it drops to empty too quickly.
- Triggers in an empty room → move the sensitivity slider right. Misses you →
  move it left.
- WiFi passes through walls, so movement next door can register. Concrete floors
  attenuate it more than walls.
- This detects **presence**, not an exact head count. Counting a few people
  (0/1/2/3) would need recorded, labelled data and a trained model. That's the
  next step.

## Security notes

- The WiFi password comes from an environment variable at build time. It's stored
  on the board (NVS) and in local build output (`firmware/.pio/`, gitignored),
  never in source.
- The dashboard has no authentication. Anyone on your WiFi can view it and
  trigger recalibration.
