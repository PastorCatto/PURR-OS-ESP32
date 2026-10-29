#!/bin/zsh
# Waits for the T-Deck's USB port, erases the whole flash, writes the blank-device image, and then KEEPS THE BOARD IN THE CHIP'S DOWNLOAD MODE:
# the ROM loader waits for the flasher, nothing of PURR OS runs, and the peripheral power rail, the display, its backlight and the keyboard stay off.
# If the board ever leaves that mode (it restarts, or the port goes away and comes back), it is put straight back. Stop with Ctrl-C, or kill the script;
# press the board's reset (or run esptool with --after hard_reset) to start it.
#
#   tools/hw/flash_and_hold.sh [IMAGE]       default image: out/hw/tdeck_plus-blank-flash.bin
. ~/esp/esp-idf-v5.3.5/export.sh >/dev/null 2>&1
ROOT=${0:A:h:h:h}
IMG=${1:-$ROOT/out/hw/tdeck_plus-blank-flash.bin}
port() { ls /dev/cu.usbmodem* 2>/dev/null | head -1 }
until P=$(port) && [[ -n $P ]]; do sleep 2; done
echo "port $P"; sleep 3
python -m esptool --chip esp32s3 -p $P -b 460800 --after no_reset erase_flash || exit 1
python -m esptool --chip esp32s3 -p $P -b 460800 --before default_reset --after no_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 $IMG || exit 1
echo "FLASH DONE"
echo "holding the board in download mode (display and keyboard off); Ctrl-C to stop"
while true; do
  P=$(port)
  if [[ -n $P ]]; then
    if python -m esptool --chip esp32s3 -p $P --before default_reset --after no_reset chip_id >/dev/null 2>&1; then
      echo "$(date +%H:%M:%S) held in download mode"
    else
      echo "$(date +%H:%M:%S) could not reach the board; trying again"
    fi
  else
    echo "$(date +%H:%M:%S) no USB port; waiting"
  fi
  sleep 12
done
