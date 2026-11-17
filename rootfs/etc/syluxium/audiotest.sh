#!/bin/sh
# Phase 8 audio test (run by hand: sh /etc/syluxium/audiotest.sh).
# Plays a fixed sequence that the host can verify in QEMU's WAV capture,
# separated by ~1 s of silence:
#   1. raw-ioctl sine, 440 Hz, 2 s           (sndtest play)
#   2. aplay -D hw:0,0, 1000 Hz, 2 s         (libasound, hw device)
#   3. same file after "amixer sset Master 50%" (quieter)
#   4. aplay through "default" (plug), 44.1 kHz file resampled, 750 Hz, 2 s
#   5. speaker-test -t sine -f 600, one loop (both channels)
T=/opt/syluxium/tests/sndtest
echo "=== audiotest: start"
cat /proc/asound/cards
cat /proc/asound/pcm
aplay -l
$T info
$T wav /tmp/sine1k.wav 1000 2
$T wav /tmp/sine750.wav 750 2 44100
amixer -c 0 scontrols
sleep 1
echo "=== 1: raw ioctl 440 Hz"
$T play 440 2
sleep 1
echo "=== 2: aplay hw:0,0 1000 Hz"
aplay -D hw:0,0 /tmp/sine1k.wav
sleep 1
echo "=== 3: Master 50%"
amixer sset Master 50%
amixer get Master
aplay -D hw:0,0 /tmp/sine1k.wav
amixer sset Master 100%
sleep 1
echo "=== 4: default device (plug) 44.1 kHz 750 Hz"
aplay /tmp/sine750.wav
sleep 1
echo "=== 5: speaker-test"
speaker-test -c 2 -t sine -f 600 -l 1 -p 100000
sleep 1
echo "=== ABI checks"
$T abi
echo "=== capture"
$T capture 1
arecord -D hw:0,0 -f S16_LE -r 48000 -c 2 -d 1 /tmp/rec.wav && ls -l /tmp/rec.wav
cat /proc/asound/card0/pcm0p/sub0/status
echo "=== audiotest: done"
