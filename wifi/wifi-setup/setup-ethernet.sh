#!/bin/bash
# Ethernet Setup Script for Ubuntu 24.04 on Raspberry Pi 4
# Configures eth0 with static IP using systemd-networkd
# Isolates eth0 from NetworkManager control

set -e

echo "======================================"
echo "  Ethernet Setup (eth0)"
echo "======================================"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root (use sudo)"
    exit 1
fi

# Configuration
ETH_INTERFACE="eth0"

if [ -z "$ETH_IP" ]; then
    ETH_IP="192.168.10.1"
fi

ETH_NETMASK="24"

echo "Configuring ${ETH_INTERFACE} with static IP: ${ETH_IP}/${ETH_NETMASK}"
echo ""

echo "Step 1: Creating netplan configuration for eth0..."

# Backup existing netplan config
mkdir -p /etc/netplan/backup
if [ -f /etc/netplan/01-eth0-static.yaml ]; then
    cp /etc/netplan/01-eth0-static.yaml /etc/netplan/backup/01-eth0-static.yaml.bak-$(date +%s)
fi

# Create eth0 static IP config
cat > /etc/netplan/01-eth0-static.yaml <<EOF
network:
  version: 2
  renderer: networkd
  ethernets:
    eth0:
      dhcp4: no
      addresses:
        - ${ETH_IP}/${ETH_NETMASK}
EOF

chmod 600 /etc/netplan/01-eth0-static.yaml

echo "✓ Netplan configuration created"
echo ""

echo "Step 2: Ensuring systemd-networkd is enabled..."

systemctl unmask systemd-networkd
systemctl enable systemd-networkd
systemctl start systemd-networkd

echo "✓ systemd-networkd enabled and started"
echo ""

echo "Step 3: Applying netplan configuration..."

netplan apply

echo "✓ Netplan configuration applied"
echo ""

echo "Waiting for interface to come up..."
sleep 3

echo ""
echo "======================================"
echo "  Ethernet Setup Complete!"
echo "======================================"
echo ""

# echo "Interface Status:"
# echo "-----------------"
# ip addr show ${ETH_INTERFACE} | grep "inet "
# echo ""

# echo "Detailed Status:"
# echo "----------------"
# networkctl status ${ETH_INTERFACE} | grep -E "State|Address|Gateway" | head -10
# echo ""

# Verify the interface is up and has the correct IP
CURRENT_IP=$(ip -4 addr show ${ETH_INTERFACE} | grep -oP '(?<=inet\s)\d+(\.\d+){3}')

if [ "$CURRENT_IP" = "$ETH_IP" ]; then
    echo "✓ SUCCESS: ${ETH_INTERFACE} configured with IP ${ETH_IP}"
    echo ""
    echo "Configuration Summary:"
    echo "• Interface: ${ETH_INTERFACE}"
    echo "• IP Address: ${ETH_IP}/${ETH_NETMASK}"
    echo "• Managed by: systemd-networkd"
    echo "• Status: Stable and isolated from NetworkManager"
    echo "To connect on mac run: sudo ifconfig en10 inet 192.168.10.2 netmask 255.255.255.0 up"
    echo "Test connectivity with: "
    echo "SSH into Pi with: ssh username@192.168.10.1"
else
    echo "⚠ WARNING: Expected IP ${ETH_IP} but got ${CURRENT_IP}"
    echo "Please check the configuration"
fi

echo ""
echo "Notes:"
echo "• eth0 will persist this configuration across reboots"
echo "• eth0 is managed by systemd-networkd (not NetworkManager)"
echo "• Run setup-nmcli.sh next to configure wlan0"
echo ""