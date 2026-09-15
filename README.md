# PrintServer — ESP32-C5 Wi-Fi bridge for an HP Color LaserJet Pro M255dw

Firmware that turns a **LilyGo T-Display C5** (ESP32-C5) into a small, low-power
**Wi-Fi NAT repeater** dedicated to an **HP Color LaserJet Pro M255dw**, plus a
live status screen on the board's 1.9″ LCD.

The M255dw's built-in Wi-Fi is unreliable at range. This board sits next to the
printer, gives it a strong short-range access point, and routes its traffic onto
the home network — so AirPrint, IPP, and raw port 9100 keep working, and the
printer stops dropping off the network.

```
your devices ── home Wi-Fi ──▶ [ C5 station ] ──NAT──▶ [ C5 SoftAP ] ◀── printer
                                (reserved IP,           ("M255dw-Bridge",
                                 shown as PrintBridge)    printer joins this)
```

## Why a Wi-Fi bridge and not USB?

The obvious idea — plug the ESP32 into the printer's USB port — **does not work on
this chip.** The ESP32-C5 (and C6) have only a USB *device* interface, no USB
*host* controller, so they can't drive a printer over USB no matter what the
firmware does. Bluetooth is out too: the M255dw's Bluetooth is BLE used only for
app-based setup, not for printing.

What the printer *does* support well is plain IP networking over Wi-Fi. So the
right job for the ESP32 is not to replace the printer's data cable, but to give
it a reliable network path. That's what this firmware does, using the stock
ESP-IDF `wifi/softap_sta` NAT pattern.

**This helps when** the flakiness is caused by range, weak signal, or 2.4 GHz
congestion — the usual causes. It won't fix a bug inside the printer's own
network stack. In practice, moving the printer onto a strong short-range AP right
beside it is a solid, cheap win.

## Hardware

| Part | Notes |
|------|-------|
| **LilyGo T-Display C5** | ESP32-C5, dual-band Wi-Fi 6 (2.4/5 GHz), 1.9″ ST7789 170×320 LCD, 16 MB flash |
| **External antenna** | **Required** — without it the C5 sees no networks. Snap it onto the u.FL connector. |
| **HP Color LaserJet Pro M255dw** | Any client reaches it through the bridge; no printer-side changes beyond joining the bridge's Wi-Fi |

Place the board next to the printer and power it from any USB charger. Because the
C5 has a single radio, its access point shares a channel with the home Wi-Fi it
joins — give it a **5 GHz** home SSID and the printer link lands on 5 GHz too.

### LCD wiring (built into the T-Display C5)

Taken from LilyGo's `board_config.h`; SPI2, 170×320, gap 35, color inversion, BGR.

| Signal | GPIO | | Signal | GPIO |
|--------|------|-|--------|------|
| MOSI | 9 | | RST | 23 |
| SCLK | 7 | | Backlight | 25 |
| CS | 26 | | | |
| DC | 8 | | | |

## What it does

- **Bridges** the printer onto the home LAN with NAT (lwIP NAPT).
- **Forwards** the printer's service ports from the bridge's home-LAN IP to the
  printer, so you print *to the bridge's IP*:
  - `9100` raw / JetDirect, `631` IPP, `515` LPD, `80` the printer's web UI.
- **Announces** itself to the router as the DHCP hostname **`PrintBridge`**, so
  it's easy to find and reserve.
- **Shows live status** on the LCD: printer model, state (green/amber/red by
  health), pending jobs, Wi-Fi signal, connected clients, uptime, NTP clock, and
  the bridge IP. Printer info comes from an IPP `Get-Printer-Attributes` query.

## Repository layout

```
main/
  printserver_main.c   Wi-Fi STA+SoftAP, NAPT, port forwarding, status loop, SNTP
  ipp_client.c/.h      minimal IPP Get-Printer-Attributes client
  display.c/.h         ST7789 + LVGL status screen
  status.h             shared status snapshot
  secrets.example.h    credential template (copy to secrets.h)
  idf_component.yml    managed deps: lvgl, esp_lvgl_port
sdkconfig.defaults     esp32c5, 16 MB flash, NAPT + IP forwarding
CMakeLists.txt
```

## Build and flash

Uses **ESP-IDF v5.5.5**, target **esp32c5**.

1. **Credentials.** Copy the template and fill it in — `secrets.h` is git-ignored,
   so your passwords never get committed:
   ```bash
   cp main/secrets.example.h main/secrets.h
   # edit main/secrets.h: home Wi-Fi SSID/pass, and the SoftAP SSID/pass
   # the printer will join (WPA2, 8–63 char password)
   ```

2. **Timezone.** The LCD clock uses a POSIX `TIMEZONE` `#define` near the top of
   `main/printserver_main.c` (default US Pacific). Change it to your zone, e.g.
   `EST5EDT,M3.2.0,M11.1.0`.

3. **Build and flash** (adjust the serial port):
   ```bash
   . $IDF_PATH/export.sh          # your ESP-IDF v5.5.x checkout
   idf.py set-target esp32c5
   idf.py -p /dev/ttyACM0 build flash monitor
   ```

## First-time setup

1. Flash the board and power it beside the printer.
2. On the **printer**: Network → Wireless Setup Wizard → join the SoftAP SSID you
   set in `secrets.h` (default `M255dw-Bridge`).
3. In your **router**, reserve a static IP for the bridge (it appears as
   `PrintBridge`). All clients will print to this address.
4. **Add the printer on your computers**, pointing at the bridge IP.

### Adding the printer on Ubuntu/CUPS

Driverless (IPP Everywhere) — CUPS builds the driver by querying the printer, so
you get color and duplex with no HP packages:

```bash
lpadmin -p PrintBridge \
  -v ipp://BRIDGE_IP/ipp/print \
  -m everywhere \
  -D "HP Color LaserJet M255 (via ESP32 bridge)" \
  -E
lpoptions -d PrintBridge          # optional: make it the default
echo "test $(date)" | lp -d PrintBridge
```

Replace `BRIDGE_IP` with the reserved address. Raw fallback if `everywhere` is
unavailable: `-v socket://BRIDGE_IP:9100`.

## Limitations and notes

- **Discovery doesn't cross the NAT.** AirPrint/Bonjour/mDNS auto-discovery won't
  find the printer through the bridge — add it by the bridge's IP. (An mDNS
  advertiser on the C5 is a possible future addition.)
- **Delete stale printer queues.** After switching to the bridge, remove any old
  printer entry that points at the printer's former address, or jobs will hang in
  "Processing" forever while the printer sits idle.
- **Reserve the bridge's IP.** Clients print to it; a DHCP reservation keeps it
  from moving.
- **Single radio.** AP and station share one channel; throughput is shared but
  fine for printing.
- **Flash headroom.** With LVGL the app fills most of the default 1.5 MB app
  partition (~6% free). Adding much more needs a custom partition table — this
  does *not* affect print jobs, which stream through and are never stored.

## Credits and license

The Wi-Fi/NAT core is adapted from the public-domain ESP-IDF `wifi/softap_sta`
example. LCD parameters come from LilyGo's
[T-Display-C5](https://github.com/Xinyuan-LilyGO/T-Display-C5) board support.
Uses [LVGL](https://lvgl.io) and Espressif's `esp_lvgl_port`.

No license file is included yet — add one before publishing if you want to set
usage terms.
