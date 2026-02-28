#!/bin/bash
# Deploy to Pi One - Initial setup over existing network connection
# This script installs packages and configures static ethernet

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  Rooted Robotics - Pi Setup (Step 1 of 2)  ${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""

# Ask user for Raspberry Pi hostname/IP and login username
read -p "Enter the Raspberry Pi hostname or IP (default: rootedpi): " PI_HOST
PI_HOST=${PI_HOST:-rootedpi}

# Test connectivity first
echo "Testing connectivity to ${PI_HOST}..."
if ! ping -c 2 "$PI_HOST" > /dev/null 2>&1; then
    echo -e "${RED}Unable to reach ${PI_HOST}. Please check the connection and try again.${NC}"
    exit 1
fi
echo -e "${GREEN}✓ Connection successful${NC}"

read -p "Enter the Raspberry Pi login username (default: ubuntu): " PI_USER
PI_USER=${PI_USER:-ubuntu}

# Get SSH password for sudo operations
read -s -p "Enter the SSH password for ${PI_USER}@${PI_HOST}: " SSH_PASSWORD
echo ""

# Check if sshpass is installed
if ! command -v sshpass &> /dev/null; then
    echo -e "${RED}Error: sshpass is not installed${NC}"
    echo "Install with: brew install hudochenkov/sshpass/sshpass"
    exit 1
fi

echo ""
echo -e "${GREEN}[1/3] Installing packages on Pi...${NC}"

sshpass -p "$SSH_PASSWORD" ssh -o StrictHostKeyChecking=no "${PI_USER}@${PI_HOST}" << ENDSSH
echo 'Updating package lists...'
echo '$SSH_PASSWORD' | sudo -S apt update

echo 'Installing required packages...'
echo '$SSH_PASSWORD' | sudo -S DEBIAN_FRONTEND=noninteractive apt install -y \
    network-manager \
    bluez bluetooth \
    python3 python3-pip python3.12-venv python3-dev \
    pkg-config libcairo2-dev libgirepository1.0-dev libglib2.0-dev libdbus-1-dev \
    git curl
ENDSSH

echo -e "${GREEN}[2/3] Copying setup scripts to Pi...${NC}"

# Create directories
sshpass -p "$SSH_PASSWORD" ssh -o StrictHostKeyChecking=no "${PI_USER}@${PI_HOST}" \
    "echo '$SSH_PASSWORD' | sudo -S mkdir -p /opt/rooted-ble/setup-scripts && echo '$SSH_PASSWORD' | sudo -S chown -R ${PI_USER}:${PI_USER} /opt/rooted-ble"

# Copy setup scripts
sshpass -p "$SSH_PASSWORD" scp -o StrictHostKeyChecking=no \
    "${SCRIPT_DIR}/setup-scripts/setup-ethernet.sh" \
    "${SCRIPT_DIR}/setup-scripts/setup-nm.sh" \
    "${PI_USER}@${PI_HOST}:/opt/rooted-ble/setup-scripts/"

echo -e "${GREEN}[3/3] Configuring static ethernet...${NC}"

sshpass -p "$SSH_PASSWORD" ssh -o StrictHostKeyChecking=no "${PI_USER}@${PI_HOST}" << ENDSSH
chmod +x /opt/rooted-ble/setup-scripts/*.sh
echo '$SSH_PASSWORD' | sudo -S bash /opt/rooted-ble/setup-scripts/setup-ethernet.sh
ENDSSH

echo ""
echo -e "${GREEN}======================================${NC}"
echo -e "${GREEN}  Step 1 Complete!                   ${NC}"
echo -e "${GREEN}======================================${NC}"
echo ""
echo "Ethernet configured on eth0 with IP: 192.168.10.1"
echo ""
echo -e "${YELLOW}Next Steps:${NC}"
echo "─────────────"
echo "1. Connect your computer directly to the Pi via ethernet cable"
echo ""
echo "2. Set a static IP on your computer:"
echo "   macOS:  sudo ifconfig en0 inet 192.168.10.2 netmask 255.255.255.0 up"
echo "   Linux:  sudo ip addr add 192.168.10.2/24 dev eth0"
echo ""
echo "3. Test connectivity:"
echo "   ping 192.168.10.1"
echo ""
echo "4. Run the second deployment script:"
echo "   ./deploy-to-pi-two.sh"
echo ""
