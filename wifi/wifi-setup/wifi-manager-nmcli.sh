#!/bin/bash
# WiFi Auto-Switch Manager for Captive Portal
# Automatically switches between hotspot mode and WiFi client mode
# Ubuntu 24.04 on Raspberry Pi 4

set -e

# Configuration
WLAN_INTERFACE="wlan0"
HOTSPOT_SSID="Rooted-Robotics-Setup"
HOTSPOT_IP="10.42.0.1"
TIMEOUT_SECONDS=30  # Time to wait for WiFi connection before switching to hotspot

# Function to check if connected to internet
check_internet() {
    ping -c 1 -W 2 8.8.8.8 &>/dev/null
    return $?
}

# Function to check if WiFi client connection exists and is available
check_wifi_available() {
    local conn_name="$1"
    nmcli -t -f NAME,TYPE connection show --active | grep -q "^${conn_name}:.*wireless$"
    return $?
}

# Function to create hotspot
create_hotspot() {
    if [ -z "$HOTSPOT_SSID" ]; then
        HOTSPOT_SSID="Rooted-Robotics-Setup"
    fi
    echo "Creating hotspot: $HOTSPOT_SSID"

    # Delete existing hotspot if it exists
    nmcli connection delete "$HOTSPOT_SSID" 2>/dev/null || true

    # Create new hotspot with NO password (open network)
    nmcli connection add type wifi ifname "$WLAN_INTERFACE" mode ap \
        con-name "$HOTSPOT_SSID" \
        ssid "$HOTSPOT_SSID"

    # Configure hotspot settings
    nmcli connection modify "$HOTSPOT_SSID" \
        ipv4.method shared \
        ipv4.addresses "$HOTSPOT_IP/24" \
        connection.autoconnect yes \
        connection.autoconnect-priority -999 \
        802-11-wireless.mode ap \
        802-11-wireless.band bg

    echo "✓ Hotspot created: $HOTSPOT_SSID (open network - no password)"
}

# Function to add WiFi client connection
# Adds a WiFi client profile to NetworkManager (hotspot-safe)
add_wifi_connection() {
    local ssid="$1"
    local password="$2"
    local iface="${WLAN_INTERFACE:-wlan0}"  # default to wlan0 if not set

    echo "Adding WiFi profile for SSID: $ssid"

    # Delete old connection if exists
    nmcli connection delete "$ssid" 2>/dev/null || true

    # Add new WiFi connection profile (does not activate)
    nmcli connection add type wifi ifname "$iface" con-name "$ssid" ssid "$ssid" 2>/dev/null
    nmcli connection modify "$ssid" wifi-sec.key-mgmt wpa-psk
    nmcli connection modify "$ssid" wifi-sec.psk "$password"
    nmcli connection modify "$ssid" connection.autoconnect yes connection.autoconnect-priority 10

    echo "✓ WiFi profile added: $ssid (not connected yet)"
}

# Connect to WiFi (hotspot must be down)
connect_wifi() {
    local ssid="$1"

    # Stop hotspot if active
    if nmcli connection show --active | grep -q hotspot; then
        echo "Stopping hotspot to connect to WiFi..."
        nmcli connection down hotspot
    fi

    # Activate WiFi connection
    if nmcli connection up "$ssid"; then
        echo "✓ Connected to WiFi: $ssid"
    else
        echo "❌ Failed to connect to WiFi: $ssid"
        switch_to_hotspot
    fi
}

# Function to switch to hotspot mode
switch_to_hotspot() {
    echo "Switching to hotspot mode..."
    
    # Disconnect any active WiFi connections
    nmcli device disconnect "$WLAN_INTERFACE" 2>/dev/null || true
    
    # Activate hotspot
    nmcli connection up "$HOTSPOT_SSID"
    
    echo "✓ Hotspot mode active"
    echo "  SSID: $HOTSPOT_SSID"
    echo "  IP: $HOTSPOT_IP"
    echo "  Gateway: http://$HOTSPOT_IP"
}

# Function to try connecting to saved WiFi networks
try_wifi_connections() {
    echo "Scanning for known WiFi networks..."
    
    # Get list of saved WiFi connections (excluding hotspot)
    local wifi_connections=$(nmcli -t -f NAME,TYPE connection show | \
        grep ":802-11-wireless$" | \
        cut -d: -f1 | \
        grep -v "^$HOTSPOT_SSID$")
    
    if [ -z "$wifi_connections" ]; then
        echo "No saved WiFi networks found"
        return 1
    fi
    
    # Try each connection
    for conn in $wifi_connections; do
        echo "Trying connection: $conn"
        
        if nmcli connection up "$conn" 2>/dev/null; then
            echo "Connected to: $conn"
            
            # Wait and check for internet
            sleep 5
            if check_internet; then
                echo "✓ Internet connection verified"
                return 0
            else
                echo "⚠ Connected but no internet access"
                nmcli connection down "$conn" 2>/dev/null || true
            fi
        fi
    done
    
    echo "Could not connect to any saved WiFi network"
    return 1
}

check_cloud() {
    # Check connectivity to a cloud service (example: google.com)
    # Haven't setup AWS IoT or similar, so using google.com as a placeholder
    curl -s --max-time 5 https://www.google.com > /dev/null
    return $?
}

# Function to monitor and auto-switch
auto_switch_daemon() {
    echo "Starting WiFi auto-switch daemon..."
    echo "Press Ctrl+C to stop"
    trap 'echo; echo "Daemon stopped"; exit 0' INT TERM

    local wait_interval=10

    while true; do
        # If we already have internet, verify cloud
        if check_internet; then
            echo "✓ Internet connection active"
            if check_cloud; then
                echo "✓ Cloud connectivity verified"
                return 0
            else
                echo "⚠ No cloud connectivity, re-evaluating..."
            fi
        else
            echo "⚠ No internet connection, trying WiFi connections..."
            # Try to connect to known WiFi networks; if that fails, start hotspot
            if try_wifi_connections; then
                echo "✓ Connected to a saved WiFi network"
                # give a moment for the connection to stabilise before re-checking
                sleep 5
                if check_internet; then
                    echo "✓ Internet available after connecting to WiFi"
                    # Loop will check cloud on next iteration
                else
                    echo "⚠ Still no internet after connecting to WiFi; disconnecting and re-evaluating..."
                    nmcli connection down "$(nmcli -t -f NAME connection show --active | head -n1)" 2>/dev/null || true
                fi
            else
                echo "Could not connect to saved WiFi networks; starting hotspot as fallback..."
                switch_to_hotspot
            fi
        fi

        # Check whether hotspot is active and report status
        ACTIVE=$(nmcli -t -f NAME,TYPE connection show --active 2>/dev/null || true)
        if echo "$ACTIVE" | grep -q "^$HOTSPOT_SSID:"; then
            echo "Hotspot already active: $HOTSPOT_SSID"
        fi

        sleep "$wait_interval"
    done
}

# Search for all wifi's in nearby area
search_wifi_net() { 
    # echo "Scanning for nearby WiFi networks..."
    nmcli device wifi rescan
    sleep 2 
    nmcli -t -f SSID device wifi list | sed '/^$/d'
}

# Main setup function
setup() {
    echo "=== WiFi Manager Setup ==="
    echo ""
    
    # Check if running as root
    if [ "$EUID" -ne 0 ]; then 
        echo "Please run as root (use sudo)"
        exit 1
    fi
    
    # Check if NetworkManager is installed
    if ! command -v nmcli &> /dev/null; then
        echo "NetworkManager not found. Installing..."
        apt-get update
        apt-get install -y network-manager
        echo "✓ NetworkManager installed"
    fi
    
    echo "Step 1: Ensuring NetworkManager manages $WLAN_INTERFACE..."
    
    # Remove any conflicting netplan configurations
    if [ -f /etc/netplan/50-cloud-init.yaml ]; then
        echo "Backing up and modifying netplan..."
        cp /etc/netplan/50-cloud-init.yaml /etc/netplan/50-cloud-init.yaml.bak-$(date +%s)
        
        # Set renderer to NetworkManager and let it handle everything
        cat > /etc/netplan/50-cloud-init.yaml <<EOF
network:
  version: 2
  renderer: NetworkManager
EOF
        netplan generate
        netplan apply
    fi
    
    # Override system-wide NetworkManager config
    rm -f /usr/lib/NetworkManager/conf.d/10-globally-managed-devices.conf
    
    # Make sure NetworkManager is managing wlan0
    cat > /etc/NetworkManager/conf.d/10-globally-managed-devices.conf <<EOF
[keyfile]
unmanaged-devices=none

[device]
wifi.scan-rand-mac-address=no
EOF
    
    # Disable and mask systemd-networkd completely
    systemctl disable --now systemd-networkd.service 2>/dev/null || true
    systemctl disable --now systemd-networkd.socket 2>/dev/null || true
    systemctl mask systemd-networkd.service 2>/dev/null || true
    
    echo "Restarting NetworkManager..."
    systemctl restart NetworkManager
    sleep 5
    
    # Set wlan0 to managed if it isn't
    nmcli device set "$WLAN_INTERFACE" managed yes 2>/dev/null || true
    sleep 2
    
    echo "Step 2: Creating hotspot configuration..."
    create_hotspot
    
    echo ""
    echo "=== Setup Complete ==="
    echo ""
    echo "Current WiFi connections:"
    nmcli connection show | grep -E "NAME|802-11-wireless"
    echo ""
}

# Command handling
case "${1:-}" in
    setup)
        setup
        ;;
    add-wifi)
        if [ -z "$2" ] || [ -z "$3" ]; then
            echo "Usage: $0 add-wifi SSID PASSWORD"
            exit 1
        fi
        add_wifi_connection "$2" "$3"
        ;;
    connect-wifi)
        if [ -z "$2" ]; then
            echo "Usage: $0 connect-wifi SSID"
            exit 1
        fi
        connect_wifi "$2"
        ;;
    start-hotspot)
        switch_to_hotspot
        ;;
    add-hotspot) 
        if [ -z "$2" ]; then
            echo "Usage: $0 add-hotspot SSID"
            exit 1
        fi
        create_hotspot
        ;;
    try-wifi)
        try_wifi_connections
        ;;
    search-wifi)
        search_wifi_net
        ;;
    auto-switch)
        auto_switch_daemon
        ;;
    status)
        echo "=== WiFi Status ==="
        echo ""
        echo "Interface status:"
        nmcli device status | grep -E "DEVICE|$WLAN_INTERFACE"
        echo ""
        echo "Active connection:"
        nmcli connection show --active | grep -E "NAME|802-11-wireless"
        echo ""
        echo "Internet connectivity:"
        if check_internet; then
            echo "✓ Connected to internet"
        else
            echo "✗ No internet connection"
        fi
        ;;
    *)
        echo "WiFi Auto-Switch Manager"
        echo ""
        echo "Usage: $0 COMMAND"
        echo ""
        echo "Commands:"
        echo "  setup                    - Initial setup (create hotspot config)"
        echo "  add-wifi SSID PASSWORD   - Add a WiFi network"
        echo "  start-hotspot           - Manually start hotspot mode"
        echo "  try-wifi                - Try connecting to saved WiFi networks"
        echo "  daemon                  - Start auto-switch daemon (monitors and switches)"
        echo "  status                  - Show current WiFi status"
        echo ""
        echo "Examples:"
        echo "  sudo $0 setup"
        echo "  sudo $0 add-wifi \"MyWiFi\" \"mypassword\""
        echo "  sudo $0 daemon"
        echo ""
        echo "Typical workflow:"
        echo "  1. Run 'setup' to initialize"
        echo "  2. Run 'daemon' to start auto-switching"
        echo "  3. When no WiFi available, connects users to '$HOTSPOT_SSID' hotspot"
        echo "  4. Use captive portal to call 'add-wifi' with credentials"
        echo "  5. Script auto-switches to WiFi client mode"
        ;;
esac