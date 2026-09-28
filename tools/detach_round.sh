#!/bin/bash
# One detach round against a Linux host run by tools/hostbt.py: Linux pairs
# with pad slot 1, then disconnects (HOLD seconds later; HOLD=12 lets BlueZ
# drop the idle link itself).  Prints the pairing result, the reset counter,
# and the LMP markers from the detach on (needs PATCH_BTCTLER "markers").
cd /work/cyclone2-firmware
A=FA:09:3A:F3:ED:E9
P=tools/padbus.py; H=tools/hostbt.py
$P request claude >/dev/null
$H bt "remove $A" >/dev/null; sleep 1
$P send claude "config log.hcidump=true" >/dev/null
R0=$(grep -a -c 'BTSTACK: stack ready' /work/nslog/padbus.log)
$P send claude "pair 1" >/dev/null
for i in $(seq 50); do [ $(grep -a -c 'BTSTACK: stack ready' /work/nslog/padbus.log) -gt $R0 ] && break; sleep 0.2; done
sleep 1
S=$($H seq)
$H bt "scan on" >/dev/null
$H wait $S "Device $A" -t 25 >/dev/null || { echo "not discovered"; exit 1; }
$H bt "scan off" >/dev/null
$H run "pair $A" --wait 'Pairing successful|Failed' -t 30 | tail -1
sleep ${HOLD:-2}
D=$($H seq)
$H bt "disconnect $A" >/dev/null
$H wait $D "$A.*disconnected|Disconnected" -t 10 | tail -1
sleep ${AFTER:-15}
$P send claude "debug bringup" >/dev/null; sleep 2
L=$(grep -a -n '^reset-mailbox' /work/nslog/padbus.log | tail -1 | cut -d: -f1)
echo "pad: $(sed -n "${L}p" /work/nslog/padbus.log | grep -o 'boots=[0-9a-f]*')"
grep -a -E 'RESET|no ACK|disconnected peer|encrypted peer' /work/nslog/padbus.log | tail -4 | cut -c1-120
sed -n "${L}p" /work/nslog/padbus.log | tr ' ' '\n' | grep -o '^[0-9a-f]\{8\}$' | cut -c1-4 | tr '\n' ' ' | grep -o '7d02.*' | cut -c1-400
echo
