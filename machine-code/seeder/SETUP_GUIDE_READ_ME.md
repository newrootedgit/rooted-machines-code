Tabletop Seeder Poll + TCP Server Setup Guide (Ubuntu 24.04 LTS, Raspberry Pi 4)

Assumptions
- User: rooted
- Project dir: /home/rooted/te-cli
- Scripts: tabletop_seeder_poll.py, tabletop_seeder_tcp_server.py
- JSON path: /home/rooted/te-cli/TE_Variable_Values.json
- TCP host/port: 192.168.10.1:8888
- Python venv: /home/rooted/te-cli/venv

----------------------------------------
Flash MicroSD
----------------------------------------
1) Open Pi Imager
2) Select Raspberry Pi 4 as hardware
3) Select Ubuntu 24.04 LTS Server as the OS
4) Select SD card to flash
5) Use custom settings to enable SSH
6) No need to provide Wi-Fi login info
7) Username: rooted
8) Password: ck9dmt5s1
9) Remove SD

----------------------------------------
Set Up Pi
----------------------------------------
1) Insert SD into Pi while powered down and Ethernet connected
2) Power on Pi
3) Use Fing App to determine local IP
4) SSH into the Pi using that IP and the credentials above

Update Pi and install git:
```bash
sudo apt update
sudo apt install -y git
```

----------------------------------------
Git Clone TE CLI
----------------------------------------
```bash
git clone https://github.com/Grayhill/te-cli.git
```

----------------------------------------
Install venv and build TE CLI in a virtual environment
----------------------------------------
```bash
sudo apt install -y python3.12-venv
cd ~/te-cli
python3 -m venv venv
. venv/bin/activate
python3 -m pip install --upgrade pip
python3 -m pip install .
python3 -m pip install ".[dev]"
```

----------------------------------------
Install HID libraries and hidapi (NEW)
----------------------------------------
```bash
sudo apt-get install -y libhidapi-hidraw0 libhidapi-libusb0 libusb-1.0-0 libusb-1.0-0-dev
pip install hidapi
```

----------------------------------------
Update TE CLI udev permissions
----------------------------------------
```bash
cd ~/te-cli
sudo cp udev/* /etc/udev/rules.d
sudo udevadm control --reload-rules && sudo udevadm trigger
```

(Optional) If encoder is still permission-blocked for user 'rooted', add an explicit rule:
1) Find vendor/product IDs:
```bash
lsusb
for f in /sys/class/hidraw/hidraw*/device/uevent; do echo "== $f =="; egrep 'HID_NAME|HID_ID' "$f"; done
```

2) Create rule (replace VVVV and PPPP with hex IDs):
```bash
sudo tee /etc/udev/rules.d/60-grayhill-te.rules >/dev/null <<'EOF'
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="VVVV", ATTRS{idProduct}=="PPPP", MODE="0666", TAG+="uaccess"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger
```

----------------------------------------
Test TE CLI install
----------------------------------------
```bash
te -h
lsusb
te ls
```

----------------------------------------
Set a static LAN on eth0 and allow Wi-Fi SSH
----------------------------------------
```bash
sudo nano /etc/netplan/50-cloud-init.yaml
```

Paste exactly:
```yaml
network:
    version: 2
    renderer: networkd

    ethernets:
        eth0:
            dhcp4: no
            addresses:
              - 192.168.10.1/24
            optional: true

    wifis:
        wlan0:
            dhcp4: yes
            access-points:
                "urbanfarms":
                    password: "RoboticFarming"
```

Apply:
```bash
sudo netplan apply
sudo ip link set wlan0 up
ip a | grep wlan0
```

----------------------------------------
 Install the two scripts and prepare JSON (REPLACES old single-script + crontab)
----------------------------------------
1) Upload via FileZilla to /home/rooted/te-cli:
   - tabletop_seeder_poll.py
   - tabletop_seeder_tcp_server.py

2) Make them executable:
```bash
cd ~/te-cli
chmod +x tabletop_seeder_poll.py tabletop_seeder_tcp_server.py
```

3) Create default JSON if missing (poller will also auto-create it):
```bash
python3 - <<'PY'
import json, os
p="/home/rooted/te-cli/TE_Variable_Values.json"
os.makedirs(os.path.dirname(p), exist_ok=True)
if not os.path.exists(p):
    with open(p,"w") as f: json.dump({"ready_to_run":False,"active_variety":None}, f, indent=4)
print("ok")
PY
```

----------------------------------------
Create systemd services for autorun + auto-restart (REPLACES crontab)
----------------------------------------
Copy and paste this entire block to create both services and start them:
```bash
sudo tee /etc/systemd/system/seeder_poll.service >/dev/null <<'EOF'
[Unit]
Description=Tabletop Seeder Poll
After=network-online.target
Wants=network-online.target
[Service]
Type=simple
User=rooted
WorkingDirectory=/home/rooted/te-cli
ExecStart=/home/rooted/te-cli/venv/bin/python /home/rooted/te-cli/tabletop_seeder_poll.py
Restart=always
RestartSec=2
NoNewPrivileges=true
[Install]
WantedBy=multi-user.target
EOF

sudo tee /etc/systemd/system/seeder_tcp_server.service >/dev/null <<'EOF'
[Unit]
Description=Tabletop Seeder TCP Server
After=network-online.target
Wants=network-online.target
[Service]
Type=simple
User=rooted
WorkingDirectory=/home/rooted/te-cli
ExecStart=/home/rooted/te-cli/venv/bin/python /home/rooted/te-cli/tabletop_seeder_tcp_server.py
Restart=always
RestartSec=2
NoNewPrivileges=true
[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable seeder_poll.service
sudo systemctl enable seeder_tcp_server.service
sudo systemctl start seeder_poll.service
sudo systemctl start seeder_tcp_server.service
```

----------------------------------------
Verify services and function
----------------------------------------
```bash
sudo systemctl is-active seeder_poll.service
sudo systemctl is-active seeder_tcp_server.service
```

# JSON updates (size or mtime should change while turning the encoder)
```bash
watch -n 1 'stat -c "%y %s" /home/rooted/te-cli/TE_Variable_Values.json'
```

# TCP listening and response
```bash
ss -ltnp | grep 8888
nc 192.168.10.1 8888   # expect CSV: ready_to_run,active_variety,roller_speed,belt_speed,irrigation_delay,irrigation_duration,misting_delay,misting_duration,roller_delay,roller_duration
```

----------------------------------------
Troubleshooting
----------------------------------------
1) TCP bind error on 192.168.10.1:
```bash
# Edit tabletop_seeder_tcp_server.py and set HOST = "0.0.0.0"
sudo systemctl restart seeder_tcp_server.service
```

2) Poller permission issues / no touch encoders found:
```bash
# Temporary: run poller as root
sudo sed -i 's/^User=rooted/User=root/' /etc/systemd/system/seeder_poll.service
sudo systemctl daemon-reload
sudo systemctl restart seeder_poll.service

# Proper: fix udev rule (see above), then switch back to User=rooted
sudo sed -i 's/^User=root$/User=rooted/' /etc/systemd/system/seeder_poll.service
sudo systemctl daemon-reload
sudo systemctl restart seeder_poll.service
```

3) Minimal logs:
```bash
sudo journalctl -u seeder_poll.service -n 50 --no-pager
sudo journalctl -u seeder_tcp_server.service -n 50 --no-pager
```
