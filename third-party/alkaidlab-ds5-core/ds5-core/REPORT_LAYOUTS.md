# DS5 report layout authority table

This table is the byte offset ledger used by the C parser. Offsets include the
HID report ID byte unless the column says `common`; every value is little endian
where a width is shown. The Linux driver is the primary layout authority;
SDL's current PS5 HID driver is an independent implementation cross-check.

| Field | USB offset/size | Bluetooth offset/size | Authority and section |
|---|---:|---:|---|
| Report ID | 0/1 (`0x01` input, `0x02` output) | 0/1 (`0x31`) | `ds5-linux-hid-playstation-20261031`: `DS_*_REPORT_*`; `ds5-sdl-hidapi-ps5-20261031`: `EPS5ReportId` |
| Stick X/Y | 1..4/1 each | 2..5/1 each | Linux `struct dualsense_input_report` x/y/rx/ry; SDL `PS5StatePacketCommon_t` 0..3 |
| Triggers | 5..6/1 each | 6..7/1 each | Linux `z/rz`; SDL common 4..5 |
| Sequence | 7/1 | 8/1 | Linux `seq_number`; SDL common `ucCounter` 6 |
| Buttons and hat | 8..11/1 each | 9..12/1 each | Linux `buttons[4]`; SDL common `rgucButtonsAndHat[4]` 7..10 |
| Gyro | 16..21/3×s16 | 17..22/3×s16 | Linux common offset 15 plus USB/BT header; SDL common gyro offsets 15,17,19 |
| Accelerometer | 22..27/3×s16 | 23..28/3×s16 | Linux common offset 21 plus USB/BT header; SDL common accel offsets 21,23,25 |
| Sensor timestamp | 28..31/u32 | 29..32/u32 | Linux common offset 27 plus USB/BT header; SDL common offset 27 |
| Touch points | 33..40/2×4 | 34..41/2×4 | Linux common offset 32 plus USB/BT header; SDL common touch counter/data offsets 32..39 |
| Status | 53..55/3 | 54..56/3 | Linux common offset 52 plus USB/BT header; status bits are the battery/jack/mic facts |
| USB output common | 1..47/47 | 3..49/47 | Linux `dualsense_output_report_common`; SDL `DS5EffectsState_t` common offsets 0..46 |
| Output valid flags | USB 1,2,39; BT 3,4,41 | same common fields | Linux `valid_flag0/1/2`; SDL effects fields and BT header |
| USB output size | 63 | — | Linux `DS_OUTPUT_REPORT_USB_SIZE`, `dualsense_output_report_usb` |
| Bluetooth output size | — | 78 | Linux `DS_OUTPUT_REPORT_BT_SIZE`, `dualsense_output_report_bt` |
| Calibration/pairing/firmware | 0x05/41, 0x09/20, 0x20/64 | same IDs; CRC when requested | Linux `DS_FEATURE_REPORT_*`, `ps_get_report`; Spike A captured payload fixtures |
| Calibration values | 0x05 payload s16 at report bytes 1,3,...,33 | same payload; CRC when requested | Linux `dualsense_get_calibration_data` offsets 1..33 |
| Firmware hardware/firmware version | 0x20 u32 at report bytes 24 and 28 | same payload; CRC when requested | Linux `dualsense_get_firmware_info` lines 1166..1167 |
| Bluetooth CRC | — | trailing 4 bytes | Linux `ps_check_crc32` seeds input `0xA1`, output `0xA2`; SDL `VerifyCRC`/effect sender |

The fixture labels are deliberate. `spikeA-input-usb.hex` and the three feature
files are captured values from the Spike A probe. The descriptor files are
descriptor bytes used by the working Spike A server and are retained as source
data evidence. `spikeA-output-usb-63.hex` is **synthetic**: Spike A captured a
48-byte Windows HID output write, and no 63-byte output capture exists yet. It
contains the source-defined 63-byte shape and remains a slot for a future real
capture; tests never modify fixture bytes and set expected fields in separate
buffers.

USB/IP offsets follow `ds5-linux-usbip-protocol-20261031`: the 20-byte basic
header, the 48-byte `CMD_SUBMIT`/`CMD_UNLINK` request, and the 48-byte
`RET_UNLINK` reply (20-byte basic header, 4-byte status, 24-byte padding).

## Output report enable bits

The parser fills a field only when its enable bit is set. The bits follow the Linux `hid-playstation`
driver (`DS_OUTPUT_VALID_FLAG0/1/2`) and SDL's PS5 driver (`ucEnableBits1/2/3`); `tests/ds5_core_report_vectors_test.c`
checks every row against a report whose value bytes are all distinct.

| Byte | Bit | Meaning | Fields filled |
|---|---|---|---|
| valid_flag0 (1) | 0x01 | compatible vibration | motor right (3), motor left (4) |
| | 0x02 | haptics select (audio haptics off) | none |
| | 0x04 | right trigger effect | right trigger (11..21) |
| | 0x08 | left trigger effect | left trigger (22..32) |
| | 0x10 | headphone volume | headphone volume (5) |
| | 0x20 | speaker volume | speaker volume (6) |
| | 0x40 | microphone volume | microphone volume (7) |
| | 0x80 | audio control | audio control (8) |
| valid_flag1 (2) | 0x01 | mute LED | mute LED (9) |
| | 0x02 | microphone mute (power save control) | microphone mute (10 bit 4) |
| | 0x04 | lightbar colour | RGB (45..47) |
| | 0x08 | release LEDs | none |
| | 0x10 | player indicator | player LEDs (44) |
| | 0x80 | audio control 2 | audio control 2 (38) |
| valid_flag2 (39) | 0x01 | lightbar brightness | brightness (43); community notes, values 0 to 2 |
| | 0x02 | lightbar setup | none (not modelled) |
| | 0x04 | improved rumble emulation | motor right, motor left |
