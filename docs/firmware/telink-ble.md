# Telink BLE transport

The Ultimate 2C Wireless runs one LE session at a time. Slots A, B, X and Y
select Generic BLE or Switch 2, with separate addresses and bonds. Generic BLE
advertises a static random address; Switch 2 advertises a public address. The
selected slot and bonds survive a reboot.

The SDK owns the radio, connections, L2CAP reassembly and SMP. Upstream
BTstack's unmodified ATT database interpreter serves profile databases from
flash, avoiding the SDK's 24-byte RAM attribute entries. CCC values reset on
disconnect.

Switch 2 sessions enable 2M PHY, negotiate 251-byte link-layer limits in both
directions, and answer the console's MTU exchange with 512. Smaller negotiated
link-layer limits prevent this console's initial MTU request; a reply of 185
stops setup before the proprietary pairing commands.

The receive FIFO has four full-size entries. Outgoing L2CAP PDUs are fragmented
to at most 70 bytes, independently of the negotiated maximum. Four small TX
entries hold these fragments. Every SDK FIFO insertion is checked before the
copy. Two pending single-packet input reports leave two entries for LL control;
the app enforces this budget while using the SDK's priority transmit path.

ATT responses use a bounded 160-byte buffer even when the negotiated MTU is
larger. Discovery returns fewer entries per response; supported attribute values
fit this buffer. The largest profile command reply is 142 bytes. The app bounds
first-fragment copies before the SDK's L2CAP reassembler and rejects oversized
fragmented ATT writes. Complete PDUs remain in the full-size controller RX FIFO.
Host writes are limited to 96 bytes and run in the control loop.

Responses and notifications have separate buffers. Responses take priority.
A command reply can replace pending input; input cannot replace a command reply.

The SDK's initial connection validator rejects intervals below six 1.25 ms
units. The app permits four units for Switch 2, retaining the other checks and
restoring the actual interval before SDK connection setup. Linker aliases retain
original SDK entry points while redirecting selected calls to app guards; the
SDK archive remains unmodified.

Generic BLE uses legacy Just Works pairing and starts input after encryption.
The slot's saved bond can restore the SDK's bond cache. Switch 2 uses its own
unencrypted ATT pairing exchange, with SMP Security Requests disabled. Its AES
key and connected host address are saved in the slot. On an LTK request, the app
reverses the stored AES key to the controller's least-significant-byte-first
order. Encryption preserves the running profile state.

Explicit connect requests for a bonded Switch 2 slot can set the advertising
wake byte to `0x81`. Automatic startup and reconnection leave it clear. The app
clears the flag on the first connection so a later disconnect cannot repeat the
wake request. This wakes the paired Switch 2 console. When the console
terminates the BLE connection with HCI reason `0x13` (remote user termination),
the app stops reconnect advertising and returns to idle. No separate Bluetooth
sleep command was observed.
