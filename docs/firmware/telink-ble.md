# Telink BLE transport

The Ultimate 2C Wireless runs one LE session at a time. Slots A, B, X and Y
select Generic BLE or Switch 2, with separate static random addresses and
bonds. The selected slot is saved in flash for the next boot.

The SDK owns the radio, connections, L2CAP reassembly and SMP. ATT requests
are intercepted after L2CAP reassembly and answered by upstream BTstack's
unmodified ATT database interpreter. Profile databases stay in flash instead
of being translated to the SDK's 24-byte RAM attribute entries. CCC values
are per connection and reset on disconnect.

The local ATT MTU is 185. The controller accepts 27-byte receive packets and
supports 80-byte transmit packets through data-length negotiation. Its transmit
FIFO has eight entries. L2CAP handles fragmentation when a PDU exceeds the
negotiated packet size.

ATT responses and notifications have separate buffers. Responses take priority.
A profile command reply can replace a pending input notification; input cannot
replace a command reply. Host writes use a bounded queue and run in the control
loop, outside SDK receive callbacks.

Generic BLE uses the SDK's legacy Just Works pairing and starts input after
encryption. The slot's saved bond can restore the SDK's bond cache.

Switch 2 uses unencrypted ATT commands for its proprietary pairing exchange.
SMP Security Requests are disabled for that profile. Its derived AES key is
saved in the slot with the connected host address. The app supplies that key
to the SDK's controller on an LTK request, reversing the stored key to the
controller's least-significant-byte-first order. Console pairing and encrypted
reconnection on this board are not established.
