#!/bin/bash
# NetworkManager WiFi Setup for Ubuntu 24.04 on Raspberry Pi 4
# Configures wlan0 for NetworkManager control (hotspot/client switching)
# Ensures eth0 remains isolated and managed by systemd-networkd

set -e

echo "======================================"
echo "  NetworkManager WiFi Setup (wlan0)"
echo "======================================"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root (use sudo)"
    exit 1
fi

# Configuration
WLAN_INTERFACE="wlan0"

echo "Step 1: Installing required packages..."

# Check and install NetworkManager if needed
if ! command -v nmcli &> /dev/null; then
    echo "Installing NetworkManager..."
    apt-get update
    apt-get install -y network-manager
else
    echo "NetworkManager already installed"
fi

# Install other required tools
apt-get install -y rfkill wireless-tools 2>/dev/null || true

echo "✓ Required packages installed"
echo ""

echo "Step 2: Removing wlan0 from netplan control..."

# Backup and modify cloud-init netplan config
if [ -f /etc/netplan/50-cloud-init.yaml ]; then
    cp /etc/netplan/50-cloud-init.yaml /etc/netplan/50-cloud-init.yaml.bak-$(date +%s)
    
    # Remove any wlan0 configuration
    cat > /etc/netplan/50-cloud-init.yaml <<EOF
# This file is generated from information provided by the datasource.  Changes
# to it will not persist across an instance reboot.  To disable cloud-init's
# network configuration capabilities, write a file
# /etc/cloud/cloud.cfg.d/99-disable-network-config.cfg with the following:
# network: {config: disabled}
network:
    version: 2
EOF
fi

# Apply netplan changes
netplan apply
rm -f /run/netplan/wpa-wlan0.conf 2>/dev/null || true

echo "✓ wlan0 removed from netplan control"
echo ""

echo "Step 3: Configuring NetworkManager..."

# Configure NetworkManager to NOT manage eth0 (let networkd handle it)
cat > /etc/NetworkManager/NetworkManager.conf <<EOF
[main]
plugins=keyfile

[ifupdown]
managed=true

[device]
wifi.scan-rand-mac-address=no

[keyfile]
unmanaged-devices=interface-name:lo,interface-name:eth0
EOF

# Create specific config to ensure wlan0 is managed
mkdir -p /etc/NetworkManager/conf.d
cat > /etc/NetworkManager/conf.d/20-manage-wlan0.conf <<EOF
[device-wlan0]
match-device=interface-name:wlan0
managed=true
EOF

echo "✓ NetworkManager configured to manage wlan0 only"
echo ""

echo "Step 4: Configuring wpa_supplicant..."

# Kill any standalone wpa_supplicant processes
killall wpa_supplicant 2>/dev/null || true
sleep 1

# Unmask wpa_supplicant but keep it disabled
# NetworkManager will start it via D-Bus when needed
systemctl unmask wpa_supplicant 2>/dev/null || true
systemctl disable wpa_supplicant 2>/dev/null || true
systemctl stop wpa_supplicant 2>/dev/null || true

echo "✓ wpa_supplicant configured for NetworkManager control"
echo ""

echo "Step 5: Starting NetworkManager..."

systemctl enable NetworkManager
systemctl restart NetworkManager

echo "✓ NetworkManager restarted"
echo ""

echo "Step 6: Enabling WiFi interface..."

# Unblock WiFi
rfkill unblock wifi

# Bring up the interface
ip link set ${WLAN_INTERFACE} up

echo "✓ WiFi interface enabled"
echo ""

echo "Waiting for NetworkManager to initialize..."
sleep 5

echo ""
echo "======================================"
echo "  NetworkManager Setup Complete!"
echo "======================================"
echo ""

echo "Device Status:"
echo "--------------"
nmcli device status
echo ""

echo "WiFi Radio Status:"
echo "------------------"
nmcli radio wifi
echo ""

echo "wpa_supplicant Process:"
echo "----------------------"
ps aux | grep wpa_supplicant | grep -v grep || echo "(Will start automatically when NetworkManager needs it)"
echo ""

# Check wlan0 state
WLAN_STATE=$(nmcli -t -f DEVICE,STATE device status | grep "^${WLAN_INTERFACE}:" | cut -d: -f2)

echo "Result:"
echo "-------"
if [ "$WLAN_STATE" = "disconnected" ]; then
    echo "✓ SUCCESS: ${WLAN_INTERFACE} is ready for NetworkManager control!"
    echo ""
    echo "You can now:"
    echo "  • Create WiFi hotspots"
    echo "  • Connect to WiFi networks"
    echo "  • Switch between hotspot and client mode"
    echo ""
    echo "Next steps:"
    echo "  1. Use your wifi-manager.sh script to manage hotspot/client modes"
    echo "  2. eth0 will remain stable and unaffected"
elif [ "$WLAN_STATE" = "unavailable" ]; then
    echo "⚠ WARNING: ${WLAN_INTERFACE} is 'unavailable'"
    echo ""
    echo "Troubleshooting steps:"
    echo "  sudo systemctl restart NetworkManager"
    echo "  sleep 5"
    echo "  nmcli device status"
    echo ""
    echo "If still unavailable, check logs:"
    echo "  journalctl -u NetworkManager -n 50 --no-pager | grep wlan"
else
    echo "ℹ INFO: ${WLAN_INTERFACE} state is: $WLAN_STATE"
fi

echo ""
echo "Configuration Summary:"
echo "---------------------"
echo "• wlan0: Managed by NetworkManager"
echo "• eth0: Still managed by systemd-networkd (isolated)"
echo "• wpa_supplicant: Controlled by NetworkManager via D-Bus"
echo "• WiFi Radio: Enabled"
echo ""