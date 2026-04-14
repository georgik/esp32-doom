# ESP32 Doom for ESP-IDF v6.0

This repository ports PrBoom, a Doom source port, to ESP32-S3 devices using ESP-IDF 6.x. This fork is adapted from the original Espressif [`esp32-doom`](https://github.com/espressif/esp32-doom) project and includes support for M5Stack AtomS3R hardware.

## Status

This is a working proof of concept that successfully runs Doom on ESP32-S3 hardware with PSRAM. The project has been tested and verified on M5Stack AtomS3R.

## Target Hardware

### Supported Hardware
- **M5Stack AtomS3R** (ESP32-S3-PICO with 8MB PSRAM) - Fully Supported
  - Documentation: https://docs.m5stack.com/en/core/AtomS3R
  - Required: PSRAM enabled for Doom engine operation

### Unsupported Hardware
- **M5Stack AtomS3** - NOT SUPPORTED (missing PSRAM)
  - The AtomS3 lacks the PSRAM required for Doom's memory allocation needs
  - Attempting to run on AtomS3 will result in memory allocation failures

### Hardware Requirements
- ESP32-S3 with **PSRAM** (minimum 8MB recommended)
- 8MB flash (AtomS3R) or 16MB flash (ESP32-S3-BOX-3)
- SPI display interface (128x128 resolution for AtomS3R)
- ESP-IDF >= 6.0.0

## Technical Modifications

This fork includes significant modifications to support AtomS3R hardware and optimize memory usage:

### Memory Management System
- **PSRAM-based memory allocation**: Replaced ESP32's limited flash MMU system with direct PSRAM allocation using `malloc()` and `esp_partition_read()`
- **Eliminated MMU exhaustion**: Removed dependency on `esp_partition_mmap()` which was causing address space exhaustion during Doom initialization
- **Optimized heap usage**: Modified Doom's memory allocator (`z_zone.c`) to fallback to PSRAM allocation when internal RAM is exhausted
- **Increased mapping capacity**: Support for 64 concurrent memory mappings vs. previous 32-limit

### Display System
- **GC9A01 display driver**: Implemented proper initialization for AtomS3R's 128x128 round TFT display
- **Resolution scaling**: Added real-time downscaling from Doom's native 320x240 to AtomS3R's 128x128 display
- **Correct GPIO pin mapping**: Configured for AtomS3R-specific pins (SCK=GPIO15, MOSI=GPIO21, CS=GPIO14, DC=GPIO42, RST=GPIO48)
- **SPI DMA optimization**: Chunk-based display updates matching DMA buffer size (32 lines)

### System Integration
- **Time management**: Replaced problematic `gettimeofday()` calls with FreeRTOS `xTaskGetTickCount()` to avoid lock initialization crashes
- **Watchdog handling**: Disabled task watchdog and implemented periodic feeding during memory operations
- **Audio system**: Disabled music and sound on AtomS3R (no audio hardware available)
- **Task configuration**: Increased Doom task stack to 32KB for PSRAM operations

### Flash Configuration
- **Custom partition table**: `partitions-atom.csv` for 8MB flash devices
  - Factory app: 1928KB
  - NV data: 16KB  
  - prboom.wad: 512KB @ 0x200000
  - DOOM1.WAD: 4608KB @ 0x280000
- **PSRAM configuration**: Optimized settings in `sdkconfig.defaults` for external memory usage

## Build Instructions

### Prerequisites
- ESP-IDF 6.0 or later
- Active ESP-IDF environment (run `export.sh` from your ESP-IDF installation)

### Building for AtomS3R
```bash
idf.py build
```

The default configuration is set for AtomS3R. For ESP32-S3-BOX-3, use:
```bash
idf.py set-target esp32s3
idf.py build
```

## Flash The Firmware

Flash the application:
```bash
idf.py -p <PORT> flash
```

Replace `<PORT>` with your serial port (e.g., `/dev/ttyUSB0` or `COM3`).

## Flash The Game Data

The firmware requires two WAD files in flash partitions:

- **prboom.wad** @ 0x200000 (prboom game data)
- **DOOM1.WAD** @ 0x280000 (Doom game assets)

### Automated Flashing
Use the provided script which auto-detects serial ports:
```bash
./flashwad.sh
./flashwad.sh /dev/cu.usbmodem114201  # Specify port manually
```

### Manual Flashing
```bash
esptool.py --chip esp32-s3 write_flash 0x200000 prboom.wad
esptool.py --chip esp32-s3 write_flash 0x280000 DOOM1.WAD
```

Note: For 8MB flash devices (AtomS3R), ensure your WAD files fit within the partition sizes defined in `partitions-atom.csv`.

## Controls

### AtomS3R Controls
Currently, joystick input is disabled pending I2C hardware implementation. The game will start and display properly, but controls require further development.

### ESP32-S3-BOX-3 Controls
Touchscreen emulation:
```
                  escape
   up             open
left  right       shoot
   down           wpn_ch
```

## Performance Notes

- **Display scaling**: Real-time 320x240 to 128x128 scaling impacts performance
- **Memory usage**: Approximately 4-6MB PSRAM usage during gameplay
- **Frame rate**: Variable depending on scene complexity and SD card access
- **Startup time**: 10-15 seconds for full initialization

## Known Limitations

- Save/load functionality not implemented
- AtomS3R joystick control pending I2C implementation
- Audio not available on AtomS3R (disabled)
- Display scaling results in small UI elements
- No multiplayer or network functionality
- Limited to DOOM1.WAD due to flash size constraints

## Troubleshooting

### "esp_mmu_map: no mem" errors
- Ensure PSRAM is properly configured in `sdkconfig.defaults`
- Verify `CONFIG_SPIRAM=y` is set

### Display shows old data or corruption
- Check GPIO pin configuration matches AtomS3R specifications
- Verify SPI clock speed (40MHz maximum for reliable operation)

### Crashes during initialization
- Increase task stack size in `app_main.c` (currently 32768 bytes)
- Verify watchdog timer is disabled: `CONFIG_ESP_TASK_WDT_TIMEOUT_S=10`

### WAD file not found errors
- Confirm partition table matches your flash size (8MB vs 16MB)
- Verify WAD files are flashed to correct addresses
- Check `flashwad.sh` uses addresses matching your partition table

## Future Development

- Implement AtomS3R joystick support via I2C
- Add LP5562 I2C backlight control
- Optimize display scaling performance
- Add keyboard support via USB or Bluetooth
- Support additional ESP32-S3 development boards
- Implement save/load functionality

## Credits

- **Doom** - Originally released by id Software under the GNU GPL
- **PrBoom** - Doom source port with contributors listed in `components/prboom/AUTHORS`
- **ESP32 Port** - Original work by Espressif Systems

## License

This project maintains the GNU GPL license from the original Doom source code. See individual component files for specific licensing information.

## References

- [M5Stack AtomS3R Documentation](https://docs.m5stack.com/en/core/AtomS3R)
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/index.html)
- [PrBoom Source Code](https://sourceforge.net/projects/prboom/)
- [Original ESP32 Doom Project](https://github.com/espressif/esp32-doom)

