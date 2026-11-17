#!/bin/sh
# Phase 8 capture + ABI test (sh /etc/syluxium/capturetest.sh). Needs an
# audio backend with input (QEMU: -audiodev none gives silent input).
T=/opt/syluxium/tests/sndtest
echo "=== capturetest: start"
$T abi
$T capture 2
arecord -D hw:0,0 -f S16_LE -r 48000 -c 2 -d 2 /tmp/rec.wav
ls -l /tmp/rec.wav
amixer sset Capture 50%
amixer get Capture
amixer sset Capture toggle
amixer get Capture | grep Front
cat /proc/asound/card0/codec#0
cat /proc/asound/devices
ls -l /dev/snd
echo "=== capturetest: done"
