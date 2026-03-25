# ESP32 Doom for ESP-IDF 6

This repository ports PrBoom, itself a Doom source port, to the ESP32-S3 using ESP-IDF 6.x. This fork is adapted from the original Espressif [`esp32-doom`](https://github.com/espressif/esp32-doom) project and is configured for the ESP32-S3-BOX-3.

## Status

This is a proof of concept, not an official Espressif application note. It should be treated as experimental code.

## Target Hardware

- ESP32-S3-BOX-3
- ESP32-S3 with PSRAM enabled
- 16 MB flash layout from [`sdkconfig.defaults`](/Users/pedrominatel/Documents/Espressif/github/esp32-doom-esp-idf-6/sdkconfig.defaults)

The project depends on the `espressif/esp-box-3_noglib` component and requires ESP-IDF `>= 6.0.0`.

## Build

Activate an ESP-IDF 6.x environment, then build normally:

```bash
idf.py build
```

## Flash The Firmware

Flash the application with the standard ESP-IDF flow:

```bash
idf.py -p <PORT>
```

Adjust the serial port as needed for your machine.

## Flash The Game Data

The firmware expects two extra flash partitions defined in `partitions.csv`:

- `prwad` at `0x200000` for `prboom.wad`
- `wad` at `0x280000` for the Doom IWAD such as `DOOM1.WAD`

The repo includes `flashwad.sh` as an example of how those assets can be written. The paths and serial device in that script are machine-specific, so update them before using it.

If you want to flash the files manually, the relevant addresses are:

```bash
esptool.py --chip esp32-s3 write_flash 0x200000 prboom.wad
esptool.py --chip esp32-s3 write_flash 0x280000 DOOM1.WAD
```

If your IWAD is larger than the current `wad` partition, increase that partition size in `partitions.csv`, then rebuild and reflash.

## Controls

The controller is emulated on the ESP32-S3-BOX-3 touchscreen:

```text
                  escape
    up            open
left  right       shoot
   down           wpn_ch
```

### Todo

- Add USB keyboard support.
- Add Bluetooth HID support.

## Known Limitations

- Save and load are not supported.
- The project is tightly configured around the ESP32-S3-BOX-3 setup in this fork.

## Credits

Doom was released by id Software under the GNU GPL. PrBoom is a modification of that codebase; its contributors are listed in [`components/prboom/AUTHORS`](/Users/pedrominatel/Documents/Espressif/github/esp32-doom-esp-idf-6/components/prboom/AUTHORS). The ESP32-specific work originates from Espressif and this fork adapts that code for ESP-IDF 6.
