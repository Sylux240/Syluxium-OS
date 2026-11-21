#!/bin/sh
# WPA2 data path without hardware (boot option mac80211_hwsim.radios=2):
# wlan0 = access point (hostapd, WPA2-PSK, CCMP), wlan1 = client
# (wpa_supplicant). After the handshake the client's DHCP DISCOVER must
# arrive decrypted at the AP side, and a broadcast frame from the AP side
# must arrive at the client.
cat > /tmp/ap.conf <<CFG
interface=wlan0
driver=nl80211
ssid=SyluxTest
hw_mode=g
channel=6
wpa=2
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
wpa_passphrase=testpassword123
CFG
cat > /tmp/sta.conf <<CFG
ctrl_interface=/run/wpa_test
network={
    ssid="SyluxTest"
    psk="testpassword123"
    key_mgmt=WPA-PSK
}
CFG
ip link set wlan0 up; ip link set wlan1 up
hostapd -B /tmp/ap.conf > /tmp/hostapd.log 2>&1
sleep 2
wpa_supplicant -B -D nl80211 -i wlan1 -c /tmp/sta.conf > /tmp/wpa.log 2>&1
sleep 6
echo "STA state: $(wpa_cli -p /run/wpa_test -i wlan1 status | grep wpa_state)"
python3 - <<'PY'
import socket, time, struct
# AP side: listen for the client's DHCP DISCOVER (UDP 68 -> 67).
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
s.bind(("wlan0", 0))
s.settimeout(1.0)
seen = 0
end = time.time() + 12
while time.time() < end:
    try:
        f = s.recv(2048)
    except socket.timeout:
        continue
    if len(f) > 42 and f[12:14] == b"\x08\x00" and f[23] == 17 and f[36:38] == b"\x00\x43":
        seen += 1
print("AP side: DHCP DISCOVERs from the client:", seen, "PASS" if seen else "FAIL")
# AP -> client: a broadcast IPv4/UDP frame to port 68 (the client counts packets received).
mac = open("/sys/class/net/wlan0/address").read().strip() if False else None
frame = b"\xff" * 6 + b"\x02\x00\x00\x00\x00\x01" + b"\x08\x00" + b"\x45" + b"\x00" * 19 + b"\x00" * 8
for i in range(5):
    s.send(frame)
    time.sleep(0.2)
PY
sleep 2
echo "client wlan1 packets received: $(grep wlan1 /proc/net/dev | awk '{print $2}')"
grep -E "AP-STA-CONNECTED|EAPOL|WPA" /tmp/hostapd.log | tail -3
