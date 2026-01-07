# Broad Overview of Functionality
This project sets up a Raspberry Pi as a WiFi hotspot with a captive portal using NetworkManager
and Flask. It allows users to connect to the Pi's hotspot, access a web page to select and connect to available WiFi networks, and manages network connections seamlessly.

## Ethernet vs WiFi Setup
- **Ethernet Setup**: Configures the Pi to use a static IP address over Ethernet's. Using networkd instead of NetworkManager for Ethernet ensures stability and avoids conflicts.
- **WiFi Setup**: Utilizes NetworkManager to manage WiFi connections, allowing easy switching between hotspot and client modes.




## Copy files to the Raspberry Pi
From your local machine, copy the project directory to the Pi (ensure SSH is enabled and the Pi is reachable):
```bash
# from the project directory on your host
scp -r . <username>@rootedpi.local:~/rootedpisetup
```

## Install dependencies
Run the following on the Raspberry Pi. **Do this before cutting the Pi off from the internet.**

### One-Command Install (Recommended)
This installs everything needed for offline setup via ethernet SSH:
```bash
sudo apt update && sudo apt upgrade -y

sudo DEBIAN_FRONTEND=noninteractive apt install -y \
  network-manager dnsmasq dnsmasq-base iptables  iptables-persistent \
  rfkill wireless-tools \
  python3 python3-pip python3-flask python3.12-venv \
  libhidapi-hidraw0 libhidapi-libusb0 libusb-1.0-0 libusb-1.0-0-dev \
  git curl
```

### What Each Package Does
| Package | Purpose |
|---------|---------|
| `network-manager` | Manages WiFi connections and hotspot |
| `dnsmasq`, `dnsmasq-base` | DNS/DHCP for captive portal |
| `iptables`, `iptables-persistent` | Traffic redirection for captive portal |
| `rfkill`, `wireless-tools` | WiFi hardware control |
| `python3`, `python3-pip`, `python3-flask` | Captive portal web app |
| `python3.12-venv` | Virtual environment for seeder |
| `libhidapi-*`, `libusb-*` | HID/USB support for TE-CLI (seeder) |
| `git`, `curl` | Cloning repos and downloading scripts |

### Python Packages (for seeder only)
If using the seeder functionality:
```bash
pip3 install hidapi
```

After installing dependencies, you can disconnect WiFi and continue all setup via ethernet SSH.

## Move project to /opt/rootedpi
```bash
sudo mv ~/rootedpisetup /opt/rootedpi
sudo chown -R rooted:rooted /opt/rootedpi
```

## Run the setup scripts
```bash
cd /opt/rootedpi

find . -name "*.sh" -exec sed -i 's/\r$//' {} \;
# Run the Ethernet setup script
# Recommended to run this first and ssh into the Pi over Ethernet
sudo bash wifi-setup/setup-ethernet.sh 

## THE CURRENT ETHERNET SETTINGS ARE AS FOLLOWS - MODIFY setup-ethernet.sh IF NEEDED
ETH_INTERFACE="eth0"
ETH_IP="192.168.10.1"
ETH_NETMASK="24"

## IMPORTANT: PLEASE ENSURE THAT YOU ARE SSHED INTO THE PI OVER ETHERNET BEFORE PROCEEDING FURTHER
## THE NEXT STEP RESTARTS NETWORKMANAGER WHICH WILL DISCONNECT ANY EXISTING WIFI CONNECTIONS
## IF PI DOES NOT SHOW UP, SET SUBNET ON YOUR LOCAL MACHINE TO 192.168.10.X/24 AND PING 192.168.10.1
# 



# Run the WiFi manager setup script
sudo bash wifi-setup/setup-nm.sh
# Verify NetworkManager is running
sudo nmcli device status
# SHOULD SHOW 
rooted@ChannelWasher:/opt/rootedpi$ nmcli device status
DEVICE         TYPE      STATE         CONNECTION
wlan0          wifi      disconnected  --
p2p-dev-wlan0  wifi-p2p  disconnected  --
eth0           ethernet  unmanaged     --
lo             loopback  unmanaged     --
```

## Start Hotspot and Captive Portal
```bash
# Start the WiFi hotspot
sudo bash /opt/rootedpi/wifi-setup/wifi-manager-nmcli.sh add-hotspot Rooted-Robotics-Setup
sudo bash /opt/rootedpi/wifi-setup/wifi-manager-nmcli.sh start-hotspot Rooted-Robotics-Setup
# Verify hotspot is running
sudo nmcli device status
# SHOULD SHOW 
rooted@ChannelWasher:/opt/rootedpi$ nmcli device status
DEVICE         TYPE      STATE         CONNECTION
wlan0          wifi      connected     Rooted-Robotics-Setup
p2p-dev-wlan0  wifi-p2p  disconnected  --
eth0           ethernet  unmanaged     --
lo             loopback  unmanaged     --


# # Connect to local wifi
# sudo bash /opt/rootedpi/wifi-manager-nmcli.sh add-wifi YourSSID yourpassword
# sudo bash /opt/rootedpi/wifi-manager-nmcli.sh connect-wifi YourSSID 

# SHOULD SHOW 
rooted@ChannelWasher:/opt/rootedpi$ nmcli device status
DEVICE         TYPE      STATE         CONNECTION
wlan0          wifi      connected     urbanfarms
p2p-dev-wlan0  wifi-p2p  disconnected  --
eth0           ethernet  unmanaged     --
lo             loopback  unmanaged     --




# Start the Flask captive portal app

# Run the Flask app setup script, this doesn't start the app, sets it up to run on hotspot
cd /opt/rootedpi/wifi-setup
chmod +x setup-captive-portal.sh
sudo ./setup-captive-portal.sh

#start the Flask app
sudo systemctl daemon-reload
sudo systemctl restart captive-portal 
sudo systemctl enable captive-portal 
```

## Useful Commands

### wifi-manager-nmcli.sh script
The `wifi-manager-nmcli.sh` script provides various functionalities to manage WiFi connections using `nmcli`.
**Usage:**
```bash
bash /opt/rootedpi/wifi-manager-nmcli.sh [command] [arguments]
```
**Commands:**
- `connect-wifi SSID`: Connects to the specified WiFi network.
- `add-wifi SSID PASSWORD`: Adds a new WiFi network with the given SSID and PASSWORD. 
- `start-hotspot`: Switches the Pi to hotspot mode. 
- `search-wifi`: Scans and lists available WiFi networks.


### nmcli
To view available WiFi networks:
```bash
nmcli device wifi list
```
To connect to a WiFi network:
```bash
bash /opt/rootedpi/wifi-manager-nmcli.sh connect-wifi SSID
```
To add a new WiFi network:
```bash
bash /opt/rootedpi/wifi-manager-nmcli.sh add-wifi SSID PASSWORD
```
To switch to hotspot mode:
```bash
bash /opt/rootedpi/wifi-manager-nmcli.sh start-hotspot
```
To restart hotspot: 
```bash
bash /opt/rootedpi/wifi-manager-nmcli.sh start-hotspot
```


To check the status of the captive portal service:
```bash
sudo systemctl status captive-portal
```
To view the logs of the captive portal service:
```bash
sudo journalctl -u captive-portal -f
```

