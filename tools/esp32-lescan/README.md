# ESP32 passive LE scanner

Controller-only ESP-IDF app (ESP32, ESP-IDF 6.1).  Prints every LE
advertisement carrying Nintendo's company ID 0x0553 (address, RSSI, data) and a
count of all reports every 5 s.  Used for the BTstack LE bring-up
(docs/ble-port.md).

    nix develop /shared/esp-idf-v6.1-shell.drv --command bash -c \
      'cd tools/esp32-lescan && idf.py set-target esp32 && idf.py build && \
       idf.py -p /dev/bwrap/ttyUSB-esp0 flash'
    nix develop /shared/esp-idf-v6.1-shell.drv --command \
      python tools/esp32-lescan/mon.py 30          # read 30 s
