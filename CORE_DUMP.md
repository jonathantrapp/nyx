# Capture a panic on Unit A without moving LittleFS

This applies to 0.3.8's partition layout. The installed Arduino 3.3.12 / ESP-IDF 5.5.5
prebuilt qio_opi SDK already has CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH and ELF/CRC32 enabled.
No fake app-level override of a prebuilt SDK option is used.

| Region | Offset | Size | Change |
|---|---|---|---|
| NVS | 0x9000 | 0x5000 | unchanged |
| PHY | 0xE000 | 0x1000 | unchanged |
| factory app | 0x10000 | 0x2C0000 | reduce unused upper capacity by 256 KiB |
| coredump | 0x2D0000 | 0x40000 | new, inside the former unused app tail |
| LittleFS (`spiffs`) | 0x310000 | 0xCF0000 | offset and size unchanged |

Use the normal PlatformIO app upload so the new partition table is flashed too. Do not
run erase-flash, uploadfs or format for this change. Existing captures/NVS stay at their
same addresses. App binaries are size-checked against the smaller app partition.

## Preserve the matching firmware symbols

The panic addresses need the ELF from the exact firmware actually flashed, including
UART vs native-usb build flags. Snapshot before rebuilding anything:

```powershell
Copy-Item -LiteralPath '.pio/build/native-usb/firmware.elf' -Destination 'ai_bundle/fw-0.3.8-native-usb.elf'
```

The delivered workspace already has snapshots for both environments and a SHA256
manifest. If you edit or rebuild before flashing, preserve that new build's ELF under
a distinct filename instead. The release snapshots no longer match an edited build.

## Verify on the board

```text
crash
```

Expect partition offset 0x002d0000 and capacity 262144, plus LittleFS offset 0x00310000
and size 0x00cf0000. Before the first captured crash, empty-or-invalid is normal. If the
partition is missing, the partition table was not uploaded; do not expect crash capture.

First extract any real crash already stored. Each later panic/watchdog crash can replace
the previous dump. Only then test deliberately:

```text
panic-test yes
```

This logs an intentional-test META note, calls abort(), and should reboot. Expect panic
reset, crumb.loop=panic_test, a saved flash image, and prior flush_context=meta from
writing the test note. This test does not erase sessions/settings. Keep power connected
until the dump has finished and the device has rebooted; cutting power during flash
writing can leave an incomplete dump. A failed or oversized dump must be investigated,
not treated as a valid backtrace.

```text
crash
```

Expect image=valid and a nonzero byte count no greater than capacity. No custom RTC panic
hook is installed: the normal SDK panic handler captures registers/stacks to flash.
RTC remains responsible for loop/radio breadcrumbs and pre-flush context only.

## Read the raw partition after the next crash

Commands below run in PowerShell from the project directory. Close the serial monitor
first. COM6 was Unit A's previously working port; substitute its current port if it
has re-enumerated. No target radios are contacted; this reads our ESP32's own flash.
Reading via the bootloader can reset the board/start a new capture session.

The official decoder and esptool were installed only under `.pio/coredump-tools` here.
On another machine, install the `esp-coredump` package into that directory first:

```powershell
python -m pip install --target '.pio/coredump-tools' esp-coredump
$env:PYTHONPATH = Join-Path (Get-Location) '.pio/coredump-tools'
```

Read exactly the new 256 KiB partition into the existing private captures directory:

```powershell
python -m esptool --chip esp32s3 --port COM6 read-flash 0x2D0000 0x40000 captures/coredump-0.3.8.bin
```

Use a new output filename for each event you want to retain. Do not intentionally
crash again or overwrite the matching ELF before this copy is safe. Coredumps can hold
packet fragments, MACs and other stack data; keep them private like raw captures.

## Decode offline with the official esp-coredump tool

The installed tool versions used to check command syntax were esp-coredump 1.18.1,
esptool 5.6.0 and PlatformIO's existing Xtensa GDB 11.2_20220823. Hardware extraction
and decoding a real dump still need Unit A verification.

```powershell
$env:PYTHONPATH = Join-Path (Get-Location) '.pio/coredump-tools'
python -m esp_coredump --chip esp32s3 info_corefile --core captures/coredump-0.3.8.bin --core-format raw --gdb 'C:/Users/<you>/.platformio/packages/tool-xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb.exe' ai_bundle/fw-0.3.8-native-usb.elf *> captures/coredump-0.3.8-report.txt
```

The decoder reads the header's actual dump length; trailing unused partition bytes are
not extra task data. Expect checksum validation, the crashed task, saved registers,
call stack and task list. For an intentional abort, expect the abort/command path;
EXCVADDR is not a meaningful bad-access address for every panic kind. For a memory
exception, inspect the saved cause and EXCVADDR along with the faulting PC.

If it reports a checksum/ELF mismatch or cannot unwind, retain the original binary,
matching ELF and messages. Do not invent frames or assume success. Missing optional
ROM symbols can limit ROM names; source addresses in the app still need the matching ELF.

For an interactive offline session:

```powershell
python -m esp_coredump --chip esp32s3 dbg_corefile --core captures/coredump-0.3.8.bin --core-format raw --gdb 'C:/Users/<you>/.platformio/packages/tool-xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb.exe' ai_bundle/fw-0.3.8-native-usb.elf
```

Claude can inspect the dump locally. Share anonymised failure reason/task/backtrace
and telemetry if needed; do not paste raw stack memory or packet payloads. Flash
coredumps usually include task stacks/registers, not a complete PSRAM or heap image.

Primary references: [ESP-IDF 5.5.5 coredumps](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-guides/core_dump.html),
[official esp-coredump tool](https://github.com/espressif/esp-coredump).
