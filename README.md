# ESP32 Doom for ESP-IDF v6.0

This repository ports PrBoom, itself a Doom source port, to the ESP32-S3 using ESP-IDF 6.x. This fork is adapted from the original Espressif [`esp32-doom`](https://github.com/espressif/esp32-doom) project and is configured for the ESP32-S3-BOX-3 with support for the ESP-BOX Joystick extension board.

## Status

This is a proof of concept, not an official Espressif application note. It should be treated as experimental code.

## Target Hardware

- ESP32-S3-BOX-3
- ESP-BOX Joystick Controller (extension board with 74HC165 shift register and analog joysticks)
- ESP32-S3 with PSRAM enabled
- 16 MB flash layout from `sdkconfig.defaults`

The project depends on the `espressif/esp-box-3_noglib` component and requires ESP-IDF `>= 6.0.0`.

## Build

Activate an ESP-IDF 6.x environment, then build normally:

```bash
idf.py build
```

## Flash The Firmware

Flash the application with the standard ESP-IDF flow:

```bash
idf.py -p <PORT> flash
```

Adjust the serial port as needed for your machine.

## Flash The Game Data

The firmware expects two extra flash partitions defined in `partitions.csv`:

- `prwad` at `0x200000` for `prboom.wad`
- `wad` at `0x280000` for the Doom IWAD such as `DOOM1.WAD`

The repo includes `flashwad.sh` to flash both WAD files using the ESP-IDF environment. The script:

- locates and sources `export.sh`
- uses `esptool.py` from the active ESP-IDF setup
- auto-detects a serial port, or accepts one as the first argument

Typical usage:

```bash
./flashwad.sh
./flashwad.sh /dev/cu.usbmodem114201
```

If you want to flash the files manually, the relevant addresses are:

```bash
esptool.py --chip esp32-s3 write_flash 0x200000 prboom.wad
esptool.py --chip esp32-s3 write_flash 0x280000 DOOM1.WAD
```

If your IWAD is larger than the current `wad` partition, increase that partition size in `partitions.csv`, then rebuild and reflash.

## Controls

### ESP-BOX Joystick Controller

The ESP-BOX Joystick extension board provides full control through physical buttons and analog joysticks.

#### D-Pad and Face Buttons

| Physical Button | In-Game Action |
|----------------|----------------|
| D-Pad Up | Move Forward |
| D-Pad Down | Move Backward |
| D-Pad Left | Turn Left |
| D-Pad Right | Turn Right |
| A Button | Use / Open |
| B Button | Fire / Shoot |
| X Button | Switch Weapon |
| Y Button | Pause / Menu |
| LB Button | Strafe Left |
| RB Button | Strafe Right |
| Start Button | Start Game |
| Select Button | Automap |

#### Analog Joystick

The left analog joystick provides smooth movement control:

| Axis | Action |
|------|--------|
| Left/Right | Turn Left/Right |
| Up/Down | Move Forward/Backward |

The joystick input overrides the D-pad for directional movement.

#### Mute Button

The ESP32-S3-BOX-3 built-in mute button toggles game audio on and off during gameplay.

### Touchscreen Controls (Fallback)

Touch controls remain available as a fallback input method:

```text
              escape
  up          open
left right   shoot
  down        wpn_ch
```

## Todo

- Add USB keyboard support.
- Add Bluetooth HID support.
- Add support to multiple BSPs.

## Known Limitations

- Save and load are not supported.
- The project is tightly configured around the ESP32-S3-BOX-3 setup in this fork.
- Right joystick is not currently utilized.

## Credits

Doom was released by id Software under the GNU GPL. PrBoom is a modification of that codebase; its contributors are listed in `components/prboom/AUTHORS`. The ESP32-specific work originates from Espressif and this fork adapts that code for ESP-IDF 6.
