#!/bin/bash
# Captive Portal Setup for Raspberry Pi 4
# FIXED: Uses correct dnsmasq-shared.d directory for NetworkManager

set -e

echo "======================================"
echo "  Captive Portal Setup (Fixed)"
echo "======================================"
echo ""

if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root (use sudo)"
    exit 1
fi

echo ""
echo "Captive portal will be copied to /opt/captive-portal"
echo "Wifi Manager script will also be copied there"
echo "This is where captive portal service will look for the Flask app"
echo ""
sudo cp -r /opt/rootedpi/captive-portal /opt/captive-portal
sudo cp /opt/rootedpi/wifi-setup/wifi-manager-nmcli.sh /opt/captive-portal/wifi-manager-nmcli.sh
echo "✓ Captive portal files copied to /opt/captive-portal"

HOTSPOT_INTERFACE="wlan0"
HOTSPOT_IP="10.42.0.1"
FLASK_PORT="80"

# echo "Step 1: Installing dependencies..."
# apt-get update
# apt-get install -y dnsmasq-base iptables python3-flask
# echo "✓ Dependencies installed"
# echo ""

echo "Step 2: Configuring NetworkManager for dnsmasq..."

# Configure NetworkManager to use dnsmasq
cat > /etc/NetworkManager/conf.d/30-dnsmasq.conf <<EOF
[main]
dns=dnsmasq
rc-manager=file

[connection]
wifi.dns=dnsmasq
EOF

# CRITICAL FIX: Use dnsmasq-shared.d directory (not dnsmasq.d)
# NetworkManager's dnsmasq reads from dnsmasq-shared.d when using ipv4.method=shared
mkdir -p /etc/NetworkManager/dnsmasq-shared.d

# Create captive portal DNS config in CORRECT directory
cat > /etc/NetworkManager/dnsmasq-shared.d/99-captive-portal.conf <<EOF
# Captive Portal DNS - Hijack ALL domains
# Redirect everything to captive portal
address=/#/${HOTSPOT_IP}

# Specific captive portal detection URLs
address=/connectivitycheck.gstatic.com/${HOTSPOT_IP}
address=/clients3.google.com/${HOTSPOT_IP}
address=/captive.apple.com/${HOTSPOT_IP}
address=/msftconnecttest.com/${HOTSPOT_IP}
address=/www.msftconnecttest.com/${HOTSPOT_IP}

# Logging (optional, comment out to disable)
log-queries
log-facility=/var/log/dnsmasq-captive.log
EOF

nmcli general reload

echo "✓ NetworkManager dnsmasq configured"
echo "✓ DNS config placed in: /etc/NetworkManager/dnsmasq-shared.d/"
echo ""

echo "Step 3: Configuring hotspot..."

if nmcli connection show "Rooted-Robotics-Setup" &>/dev/null; then
    nmcli connection modify "Rooted-Robotics-Setup" \
        ipv4.method shared \
        ipv4.addresses "${HOTSPOT_IP}/24" \
        connection.autoconnect yes \
        connection.autoconnect-priority -999
    echo "✓ Hotspot configured (priority: -999, will only activate if no WiFi available)"
else
    echo "Creating Rooted-Robotics-Setup hotspot..."
    nmcli device wifi hotspot \
        ifname wlan0 \
        con-name Rooted-Robotics-Setup \
        ssid Rooted-Robotics-Setup \
        password raspberry

    nmcli connection modify "Rooted-Robotics-Setup" \
        ipv4.method shared \
        ipv4.addresses "${HOTSPOT_IP}/24" \
        connection.autoconnect yes \
        connection.autoconnect-priority -999
    echo "✓ Hotspot created and configured (priority: -999, will only activate if no WiFi available)"
fi

echo ""

echo "Step 4: Setting up iptables..."

# Clear existing rules
iptables -t nat -F
iptables -F

# Redirect HTTP/HTTPS to captive portal
iptables -t nat -A PREROUTING -i ${HOTSPOT_INTERFACE} -p tcp --dport 80 -j DNAT --to-destination ${HOTSPOT_IP}:${FLASK_PORT}
iptables -t nat -A PREROUTING -i ${HOTSPOT_INTERFACE} -p tcp --dport 443 -j REDIRECT --to-port 80

# Allow DNS
iptables -A INPUT -i ${HOTSPOT_INTERFACE} -p udp --dport 53 -j ACCEPT

# Allow DHCP
iptables -A INPUT -i ${HOTSPOT_INTERFACE} -p udp --dport 67 -j ACCEPT

# Allow forwarding
iptables -A FORWARD -i ${HOTSPOT_INTERFACE} -j ACCEPT

echo "✓ iptables configured"
echo ""

echo "Step 5: Making iptables persistent..."
DEBIAN_FRONTEND=noninteractive apt-get install -y iptables-persistent
iptables-save > /etc/iptables/rules.v4
echo "✓ iptables rules saved"
echo ""

echo "Step 6: Creating Flask service..."
cat > /etc/systemd/system/captive-portal.service <<EOF
[Unit]
Description=Captive Portal Flask App
After=network.target NetworkManager.service
Wants=NetworkManager.service

[Service]
Type=simple
User=root
WorkingDirectory=/opt/captive-portal
ExecStart=/usr/bin/python3 /opt/captive-portal/captive_portal.py
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
echo "✓ Flask service created"
echo ""

echo "======================================"
echo "  Setup Complete!"
echo "======================================"
echo ""

echo "Configuration Summary:"
echo "---------------------"
echo "• Hotspot Interface: ${HOTSPOT_INTERFACE}"
echo "• Captive Portal IP: ${HOTSPOT_IP}"
echo "• Flask Port: ${FLASK_PORT}"
echo "• DNS Config: /etc/NetworkManager/dnsmasq-shared.d/99-captive-portal.conf"
echo ""

echo "Next Steps:"
echo "-----------"
echo "1. Create your Flask app at: /opt/captive-portal/captive_portal.py"
echo " By default, a sample app is already present."
echo ""
echo "2. Restart NetworkManager:"
echo "   sudo systemctl restart NetworkManager"
echo ""
echo "3. Start hotspot:"
echo "   sudo nmcli connection up Rooted-Robotics-Setup"
echo ""
echo "4. Enable and start captive portal:"
echo "   sudo systemctl enable captive-portal"
echo "   sudo systemctl start captive-portal"
echo ""

echo "Testing:"
echo "--------"
echo "• Check dnsmasq is running:"
echo "  ps aux | grep dnsmasq"
echo ""
echo "• Test DNS hijacking (when in AP mode):"
echo "  dig @${HOTSPOT_IP} google.com"
echo "  (should return ${HOTSPOT_IP})"
echo ""
echo "• Check dnsmasq logs:"
echo "  sudo tail -f /var/log/dnsmasq-captive.log"
echo ""
echo "• Test Flask app locally:"
echo "  curl http://${HOTSPOT_IP}"
echo ""
echo "• Check Flask logs:"
echo "  sudo journalctl -u captive-portal -f"
echo ""

echo "Client Testing:"
echo "---------------"
echo "1. Connect phone/laptop to 'Rooted-Robotics-Setup' WiFi (password: raspberry)"
echo "2. Captive portal should pop up automatically"
echo "3. If not, browse to: http://google.com"
echo ""

echo "✅ Setup complete! DNS config is now in the correct directory."
echo ""