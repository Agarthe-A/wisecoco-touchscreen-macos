# Wisecoco 12.3" bar touchscreen on macOS

The Wisecoco 12.3" 1920×720 "bar" display has a capacitive touch panel that works out of the box on Windows (and reportedly Linux) but does nothing on macOS. The cause is one USB detail: macOS enables *remote wakeup* when it configures the touch controller, and the controller firmware then falls back to a broken single-touch mouse emulation that only reports zeros. This repository documents the problem and provides a small, unprivileged command-line tool that clears remote wakeup and replays the requests Windows sends. After that, the controller streams standard multitouch reports. Verified on a Mac mini M4 Pro running macOS 26 (October 2026).

**TL;DR:** send `CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP)` to USB device `0712:0010`, then replay the Windows init requests (MS OS compat ID, `SET_IDLE`, HID report descriptor, feature reports 2 and 3). `make && ./install.sh` installs a LaunchAgent that does this at every plug-in and wake from sleep. macOS still does not turn the resulting digitizer reports into pointer events, so you also need a user-space touch driver (see [Getting touch input](#getting-touch-input)).

Not affiliated with Wisecoco. Tested with one unit; other firmware revisions may behave differently.

## Hardware

| | |
|---|---|
| Display | Wisecoco 12.3" bar, 1920 × 720, capacitive touch |
| Ports | `POWER (no touch)`, `TYPE-C`, `HDMI` |
| Touch controller | USB device behind the bar: VID `0x0712`, PID `0x0010` |
| USB strings | manufacturer `hotlotus`, product `wcidtest` |
| Versions | bcdDevice `0x0200`, bcdUSB 2.00, full speed |
| Device class | `EF/02/01` (Miscellaneous, with an Interface Association Descriptor) |
| Interface 0 | vendor class `0xFF`, bulk IN `0x86` / OUT `0x05`, 64 B; answers MS OS 1.0 string descriptor `0xEE` = `MSFT100`, vendor code `0x60`, compat ID `WINUSB` |
| Interface 1 | HID, boot subclass, mouse protocol; interrupt IN `0x82`, 64 B, bInterval 2; 840-byte report descriptor |

HID reports on interface 1:

| Report ID | Type | Content |
|---|---|---|
| 4 | input | Windows 8 style multitouch digitizer, 10 contacts, 56 bytes ([format](#report-4-format)) |
| 5 | input | absolute mouse: 3 buttons, X 0..1920, Y 0..720, wheel |
| 2 | feature | Contact Count Maximum (returns 10) |
| 3 | feature | vendor page `0xFF00`, usage `0xC5`: 256-byte "THQA" certification blob |
| 9 | output | vendor, 4 bytes (STALLs) |

## Symptom

On macOS, touching the panel does nothing. The controller only sends report ID 5 "mouse emulation" packets, and they are all zeros (no button, X = 0, Y = 0) for as long as the panel is touched.

If a host reads feature report 2 or 3 while the controller is in this state, the controller goes completely silent until it is power-cycled. A USB bus reset is not enough.

## Root cause

macOS enables `DEVICE_REMOTE_WAKEUP` when it configures the device. Windows does not:

| Host | `GET_STATUS` (device) |
|---|---|
| macOS | `02 00` (remote wakeup enabled) |
| Windows | `00 00` |

The firmware apparently uses this to detect a Mac and falls back to a broken single-touch mouse emulation. Clearing the feature, then sending the requests Windows sends after `SET_CONFIGURATION`, switches it to report ID 4.

## The fix: control-transfer sequence

All requests go to the default control pipe of the already configured device, in this order:

| # | Request | bmRequestType | bRequest | wValue | wIndex | wLength |
|---|---|---|---|---|---|---|
| 1 | `CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP)` | `0x00` | `0x01` | `0x0001` | `0x0000` | 0 |
| 2a | Vendor: MS OS extended compat ID | `0xC0` | `0x60` | `0x0000` | `0x0004` | 16 |
| 2b | Vendor: MS OS extended compat ID | `0xC0` | `0x60` | `0x0000` | `0x0004` | 40 |
| 3 | `GET_STATUS` (device), expect `00 00` | `0x80` | `0x00` | `0x0000` | `0x0000` | 2 |
| 4 | HID `SET_IDLE` | `0x21` | `0x0A` | `0x0000` | `0x0001` | 0 |
| 5 | `GET_DESCRIPTOR` (HID report) | `0x81` | `0x06` | `0x2200` | `0x0001` | 904 |
| 6a | HID `GET_REPORT` (feature 2) | `0xA1` | `0x01` | `0x0302` | `0x0001` | 257 |
| 6b | HID `GET_REPORT` (feature 3) | `0xA1` | `0x01` | `0x0303` | `0x0001` | 257 |

The controller then streams report ID 4, for example `04 41 fb 03 22 02 ...` = contact 1 down at X = 1019, Y = 546.

- Step 1 is the actual fix. Steps 2 to 6 reproduce what Windows sends after `SET_CONFIGURATION`, in the same order.
- No root and no entitlement needed: `DeviceRequest` through `IOUSBDeviceInterface` works without opening the device (`USBDeviceOpen` also succeeds, non-exclusively).
- The sequence must be re-applied after every plug-in and after every wake from sleep. Report 5 packets showing up again are a good signal to re-apply.
- `wisecoco-touch-enable` does not read the feature reports (step 6) unless step 3 confirms that remote wakeup is off, because reading them in mouse-emulation mode would silence the controller.

## Quick start

Requirements: macOS 12 or later (verified on macOS 26, Apple Silicon) and the Command Line Tools (`xcode-select --install`). No Xcode project, no root.

```sh
make                                      # builds build/wisecoco-touch-enable and build/wisecoco-touch-dump
./build/wisecoco-touch-enable --once -v   # apply once, logging each request
./install.sh                              # install the LaunchAgent (per user, no sudo)
```

`install.sh` copies the binary to `~/.local/bin/wisecoco-touch-enable`, writes `~/Library/LaunchAgents/io.github.wisecoco-touch-enable.plist` from the template in `launchd/`, and starts it with `launchctl bootstrap gui/$(id -u)`. The agent applies the sequence to the controller when it starts, whenever the controller is plugged in, and 2 s after every wake from sleep. Its log is `~/Library/Logs/wisecoco-touch-enable.log`. Remove everything with `./install.sh uninstall`.

### `wisecoco-touch-enable`

| Option | Effect |
|---|---|
| *(none)* | keep running: apply to connected devices, then to each newly attached one (after 300 ms, retrying for about 1 s until it is configured), and to all of them 2 s after each wake |
| `--once` | apply to connected devices and exit: `0` at least one enabled, `1` none found, `2` error |
| `-v` | log every request: bmRequestType, bRequest, result and transferred length |
| `--vid 0xVVVV`, `--pid 0xPPPP` | match another vendor / product ID (hexadecimal) |

### `wisecoco-touch-dump`

A diagnostic reader built on `IOHIDManager`. It prints decoded report 4 touches and flags report 5 mouse-emulation packets.

```sh
./build/wisecoco-touch-dump --seconds 20
```

```
listening for 0712:0010 for 20 s; touch the screen now
[   0.012] device: hotlotus wcidtest (0712:0010)
[   2.481] report 4  count=1  [id 1 down x=1019 y=546]
```

A line such as `report 5  mouse emulation (zeros) -> run wisecoco-touch-enable` means the fix is not applied. The dump tool needs Input Monitoring permission for the terminal app (see below).

## Getting touch input

Enabling the controller is necessary but not sufficient: macOS does not turn HID digitizer reports into pointer events, so nothing moves on screen yet. A user-space reader has to read report 4 and inject pointer events.

- The reader opens the device with `IOHIDManager`, which requires **System Settings → Privacy & Security → Input Monitoring** for the reader app. Injecting events (for example with `CGEventPost`) typically requires **Accessibility** as well.
- [Touch-Up](https://github.com/shueber/Touch-Up) is an existing open-source user-space touchscreen driver that reads digitizer reports through `IOHIDManager`. It should work once the controller is enabled (not tested with this display). Keep `wisecoco-touch-enable` running so the controller stays in multitouch mode.
- To check that touch reports arrive at all, use `wisecoco-touch-dump`.

## Report 4 format

56 bytes including the report ID. Contact `i` (0 to 9) starts at byte `1 + 5·i`.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | report ID `0x04` |
| 1 + 5·i | 1 | bits 0–5: contact ID; bit 6: tip switch (1 = down); bit 7: padding |
| 2 + 5·i | 2 | X, uint16 little-endian, 0..1920 |
| 4 + 5·i | 2 | Y, uint16 little-endian, 0..720 |
| 51 | 4 | scan time, uint32 little-endian |
| 55 | 1 | contact count |

Example: `04 41 fb 03 22 02` → contact byte `0x41` = ID 1, tip down; X = `0x03fb` = 1019; Y = `0x0222` = 546.

## Troubleshooting

- **No reports at all (silent controller).** The feature reports were probably read in the wrong state. Disconnect the bar's cables (`TYPE-C`, and `POWER` if used), reconnect them, then run `wisecoco-touch-enable --once` or let the agent do it. A USB reset does not help; it needs a real power cycle.
- **Zeros again after sleep or replug.** The agent handles both; check its log. Otherwise run `wisecoco-touch-enable --once -v`.
- **Is the controller visible?** `ioreg -p IOUSB -w0 | grep -i wcidtest` should list it. The full listing (`ioreg -p IOUSB -l -w0`) shows `idVendor = 1810` (0x0712) and `idProduct = 16` (0x0010). Connect the bar's `TYPE-C` port to the Mac; as its label says, the `POWER` port carries no touch.
- **Different IDs.** If your unit enumerates with another VID/PID, pass them as arguments: `wisecoco-touch-enable --vid 0x.... --pid 0x....` (the dump tool accepts the same options). Untested.
- **`not permitted (0xe00002e2)` from the dump tool.** Grant Input Monitoring to the terminal app, then quit and reopen it.
- **Agent status.** `launchctl print gui/$(id -u)/io.github.wisecoco-touch-enable` and `tail -f ~/Library/Logs/wisecoco-touch-enable.log`.

## What did not work

- Reading feature reports 2/3 alone: the controller goes silent.
- The MS OS vendor requests alone.
- `SET_PROTOCOL` (boot or report protocol).
- Sending output report 9: STALL.
- Reading the vendor bulk interface (interface 0).
- Seizing the HID interface: macOS refuses.
- Capturing the device as root and reading the interrupt endpoint directly: silent.
- Racing macOS at plug-in with the Windows sequence but *without* clearing remote wakeup: silent.
- Video over HDMI instead of USB-C alt mode: same zeros.
- `IOUSBDeviceInterface` `ResetDevice` from user space: returns immediately and does not actually reset the device.

## How it was found

The Windows enumeration was captured with USBPcap and Wireshark on a Windows PC and compared with what macOS does. Besides the requests Windows sends after `SET_CONFIGURATION`, the decisive difference was the device status: `GET_STATUS` returns `02 00` on macOS and `00 00` on Windows. Replaying the Windows requests on macOS did nothing until remote wakeup was cleared first.

## Files

```
src/wisecoco-touch-enable.c                    enabler (plain C, IOKit + CoreFoundation)
src/wisecoco-touch-dump.c                      diagnostic reader (IOHIDManager)
launchd/io.github.wisecoco-touch-enable.plist  LaunchAgent template
install.sh                                     build and install / uninstall the agent
Makefile                                       builds both tools into build/
```

## License

MIT, see [LICENSE](LICENSE).

**Keywords:** Wisecoco, hotlotus, wcidtest, 0712:0010, VID 0x0712 PID 0x0010, touchscreen macOS, touch not working on Mac, Apple Silicon, Mac mini M4 Pro, 12.3 inch bar display, 1920x720, mouse emulation zeros, remote wakeup, DEVICE_REMOTE_WAKEUP, HID multitouch digitizer.

## Résumé en français

L'écran barre Wisecoco 12,3" (1920×720, tactile capacitif) fonctionne directement sous Windows, mais son tactile ne fait rien sous macOS : le contrôleur USB `0712:0010` (« hotlotus wcidtest ») n'envoie que des rapports souris (ID 5) remplis de zéros.
Cause : macOS active le *remote wakeup* USB en configurant le périphérique, et le firmware se rabat alors sur une émulation souris défectueuse.
Correctif : envoyer `CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP)`, puis rejouer les requêtes de Windows (descripteur MS OS, `SET_IDLE`, descripteur de rapport HID, rapports feature 2 et 3) ; le contrôleur envoie alors des rapports multitouch (ID 4).
`make && ./install.sh` installe, sans droits root, un LaunchAgent qui réapplique la séquence à chaque branchement et à chaque sortie de veille.
Il faut encore un pilote en espace utilisateur (par exemple Touch-Up, avec l'autorisation « Surveillance de l'entrée ») pour transformer ces rapports en événements de pointeur.
Si le contrôleur devient muet, débrancher puis rebrancher l'écran, puis relancer `wisecoco-touch-enable --once`.
