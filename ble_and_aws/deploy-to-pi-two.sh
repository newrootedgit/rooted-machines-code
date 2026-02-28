#!/bin/bash
# Deploy to Pi Two - Full setup over ethernet connection
# This script runs NetworkManager setup and configures the BLE provisioner

set -e

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values - connects via ethernet!
DEFAULT_HOSTNAME="192.168.10.1"
DEFAULT_USERNAME="ubuntu"
REMOTE_DIR="/opt/rooted-ble"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Recommended machine names (these have images in the frontend app)
RECOMMENDED_MACHINES=("HARVESTER" "SEEDER" "WASHER")

echo -e "${BLUE}============================================${NC}"
echo -e "${BLUE}  Rooted Robotics - Pi Setup (Step 2 of 2)  ${NC}"
echo -e "${BLUE}============================================${NC}"
echo ""
echo "This script completes the Pi setup over ethernet."
echo "Make sure you've run deploy-to-pi-one.sh first!"
echo ""

# =============================================================================
# STEP 1: Collect Pi Credentials (Ethernet Connection)
# =============================================================================

read -p "Enter the Raspberry Pi IP (default: ${DEFAULT_HOSTNAME}): " PI_HOST
PI_HOST=${PI_HOST:-$DEFAULT_HOSTNAME}

# Test connectivity first
echo ""
echo "Testing connectivity to ${PI_HOST}..."
if ! ping -c 2 "$PI_HOST" > /dev/null 2>&1; then
    echo -e "${RED}Unable to reach ${PI_HOST}. Please check the ethernet connection.${NC}"
    echo ""
    echo "Make sure:"
    echo "  1. You ran deploy-to-pi-one.sh first"
    echo "  2. Your computer is connected to the Pi via ethernet"
    echo "  3. Your computer has IP 192.168.10.2 (or similar)"
    exit 1
fi
echo -e "${GREEN}Connection successful${NC}"

read -p "Enter the Raspberry Pi login username (default: ${DEFAULT_USERNAME}): " PI_USER
PI_USER=${PI_USER:-$DEFAULT_USERNAME}

# Get SSH password for sudo operations
read -s -p "Enter the SSH password for ${PI_USER}@${PI_HOST}: " SSH_PASSWORD
echo ""

# # Verify SSH credentials work
# echo ""
# echo "Verifying SSH credentials..."
# SSH_RESULT=$(sshpass -p "${SSH_PASSWORD}" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "${PI_USER}@${PI_HOST}" "echo 'SSH OK'" 2>&1)
# if [ $? -ne 0 ]; then
#     echo -e "${RED}SSH authentication failed.${NC}"
#     echo "Error: ${SSH_RESULT}"
#     echo ""
#     echo "Troubleshooting:"
#     echo "  - Check that sshpass is installed: brew install hudochenkov/sshpass/sshpass"
#     echo "  - Verify you can SSH manually: ssh ${PI_USER}@${PI_HOST}"
#     echo "  - If password has special characters, try escaping them"
#     exit 1
# fi
# echo -e "${GREEN}SSH authentication successful${NC}"

# =============================================================================
# STEP 2: Machine Name Selection
# =============================================================================

echo ""
echo -e "${BLUE}Machine Name Selection${NC}"
echo "────────────────────────"
echo ""
echo "Recommended machine names (these have images in the app):"
for name in "${RECOMMENDED_MACHINES[@]}"; do
    echo -e "  ${GREEN}*${NC} ${name}"
done
echo ""
echo -e "${YELLOW}Note: Custom names will work but won't have an associated image.${NC}"
echo ""

read -p "Enter machine name: " MACHINE_NAME

if [ -z "${MACHINE_NAME}" ]; then
    echo -e "${RED}Machine name cannot be empty.${NC}"
    exit 1
fi

# Check if the name matches one of the recommended names (case-insensitive prefix match)
MACHINE_UPPER=$(echo "${MACHINE_NAME}" | tr '[:lower:]' '[:upper:]')
IS_RECOMMENDED=false
for name in "${RECOMMENDED_MACHINES[@]}"; do
    if [[ "${MACHINE_UPPER}" == "${name}"* ]]; then
        IS_RECOMMENDED=true
        break
    fi
done

if [ "${IS_RECOMMENDED}" = false ]; then
    echo ""
    echo -e "${YELLOW}Warning: '${MACHINE_NAME}' is not a recommended name.${NC}"
    echo -e "${YELLOW}This machine will not have an image displayed in the app.${NC}"
    echo ""
    read -p "Continue anyway? [y/N]: " CONTINUE
    if [[ ! "${CONTINUE}" =~ ^[Yy]$ ]]; then
        echo "Aborted."
        exit 0
    fi
fi

# =============================================================================
# STEP 3: Generate Device Configuration
# =============================================================================

echo ""
echo "Generating device configuration..."

# Generate UUID (try uuidgen first, fallback to Python)
if command -v uuidgen &> /dev/null; then
    DEVICE_UUID=$(uuidgen | tr '[:upper:]' '[:lower:]')
else
    DEVICE_UUID=$(python3 -c "import uuid; print(str(uuid.uuid4()))")
fi

echo -e "${GREEN}Device configuration generated:${NC}"
echo "  Device Name: ${MACHINE_NAME}"
echo "  Device ID:   ${DEVICE_UUID}"

# =============================================================================
# STEP 3.5: AWS IoT Provisioning
# =============================================================================

echo ""
echo -e "${BLUE}AWS IoT Provisioning${NC}"
echo "────────────────────────"
read -p "Provision AWS IoT for this device? [Y/n]: " PROVISION_IOT
PROVISION_IOT=${PROVISION_IOT:-Y}

IOT_TEMP_DIR="/tmp/rooted-iot-${DEVICE_UUID}"
IOT_ENDPOINT=""

if [[ "$PROVISION_IOT" =~ ^[Yy]$ ]]; then
    # Check if AWS CLI is available
    if ! command -v aws &> /dev/null; then
        echo -e "${RED}AWS CLI not installed. Skipping IoT provisioning.${NC}"
        echo "Install with: brew install awscli"
        PROVISION_IOT="n"
    else
        echo "Provisioning AWS IoT Thing: ${DEVICE_UUID}"

        # Run the provisioning script in non-interactive mode
        "${SCRIPT_DIR}/setup-scripts/provision-iot-device.sh" "${DEVICE_UUID}" --output-dir "${IOT_TEMP_DIR}"

        if [ -f "${IOT_TEMP_DIR}/.iot_endpoint" ]; then
            IOT_ENDPOINT=$(cat "${IOT_TEMP_DIR}/.iot_endpoint")
            echo -e "${GREEN}IoT provisioning complete!${NC}"
        else
            echo -e "${RED}IoT provisioning may have failed. Continuing without IoT...${NC}"
            PROVISION_IOT="n"
        fi
    fi
fi

# Create device_config.json with IoT endpoint if available
CONFIG_FILE="/tmp/device_config.json"
if [ -n "$IOT_ENDPOINT" ]; then
    echo "{
    \"device_name\": \"${MACHINE_NAME}\",
    \"device_id\": \"${DEVICE_UUID}\",
    \"aws_iot_endpoint\": \"${IOT_ENDPOINT}\",
    \"aws_region\": \"us-west-2\"
}" > "${CONFIG_FILE}"
else
    echo "{
    \"device_name\": \"${MACHINE_NAME}\",
    \"device_id\": \"${DEVICE_UUID}\"
}" > "${CONFIG_FILE}"
fi
cp "${CONFIG_FILE}" "${SCRIPT_DIR}/device_config.json"

# =============================================================================
# STEP 4: Copy Files to Pi
# =============================================================================

echo ""
echo "Copying files to Pi..."

# Ensure remote directory exists
sshpass -p "${SSH_PASSWORD}" ssh -o StrictHostKeyChecking=no "${PI_USER}@${PI_HOST}" \
    "echo '${SSH_PASSWORD}' | sudo -S mkdir -p ${REMOTE_DIR} && echo '${SSH_PASSWORD}' | sudo -S chown ${PI_USER}:${PI_USER} ${REMOTE_DIR}"

# Files to copy
FILES_TO_COPY=(
    "provisioner.py"
    "requirements.txt"
    "device_config.json"
    "rooted-ble.service"
    "rooted-iot.service"
    "rooted-ble.timer"
    "ble-wrapper.sh"
)

for file in "${FILES_TO_COPY[@]}"; do
    if [ -f "${SCRIPT_DIR}/${file}" ]; then
        echo "  Copying ${file}..."
        sshpass -p "${SSH_PASSWORD}" scp -o StrictHostKeyChecking=no \
            "${SCRIPT_DIR}/${file}" "${PI_USER}@${PI_HOST}:${REMOTE_DIR}/"
    else
        echo -e "${YELLOW}  Warning: ${file} not found, skipping...${NC}"
    fi
done

# Copy aws/ directory
echo "  Copying aws/ Python modules..."
sshpass -p "${SSH_PASSWORD}" ssh -o StrictHostKeyChecking=no "${PI_USER}@${PI_HOST}" \
    "mkdir -p ${REMOTE_DIR}/aws"
sshpass -p "${SSH_PASSWORD}" scp -o StrictHostKeyChecking=no \
    "${SCRIPT_DIR}/aws/"*.py "${PI_USER}@${PI_HOST}:${REMOTE_DIR}/aws/"

# Copy IoT certificates if provisioning was done
if [[ "$PROVISION_IOT" =~ ^[Yy]$ ]] && [ -d "${IOT_TEMP_DIR}" ]; then
    echo "  Creating certs directory..."
    sshpass -p "${SSH_PASSWORD}" ssh -o StrictHostKeyChecking=no "${PI_USER}@${PI_HOST}" \
        "echo '${SSH_PASSWORD}' | sudo -S mkdir -p ${REMOTE_DIR}/certs && echo '${SSH_PASSWORD}' | sudo -S chown ${PI_USER}:${PI_USER} ${REMOTE_DIR}/certs"

    echo "  Copying IoT certificates..."
    sshpass -p "${SSH_PASSWORD}" scp -o StrictHostKeyChecking=no \
        "${IOT_TEMP_DIR}/certificate.pem.crt" \
        "${IOT_TEMP_DIR}/private.pem.key" \
        "${IOT_TEMP_DIR}/AmazonRootCA1.pem" \
        "${PI_USER}@${PI_HOST}:${REMOTE_DIR}/certs/"

    # Set proper permissions on private key
    sshpass -p "${SSH_PASSWORD}" ssh -o StrictHostKeyChecking=no "${PI_USER}@${PI_HOST}" \
        "chmod 600 ${REMOTE_DIR}/certs/private.pem.key"
fi

echo -e "${GREEN}Files copied successfully${NC}"

# =============================================================================
# STEP 5: Setup on Pi (NetworkManager, venv, dependencies, systemd)
# =============================================================================

echo ""
echo "Setting up on Pi..."

# Copy setup-nm.sh script
echo "  Copying NetworkManager setup script..."
sshpass -p "${SSH_PASSWORD}" scp -o StrictHostKeyChecking=no \
    "${SCRIPT_DIR}/setup-scripts/setup-nm.sh" "${PI_USER}@${PI_HOST}:${REMOTE_DIR}/"

sshpass -p "${SSH_PASSWORD}" ssh -o StrictHostKeyChecking=no "${PI_USER}@${PI_HOST}" << REMOTE_SCRIPT
set -e

REMOTE_DIR="${REMOTE_DIR}"

# Make scripts executable
chmod +x \${REMOTE_DIR}/ble-wrapper.sh
chmod +x \${REMOTE_DIR}/setup-nm.sh

# # =============================================================================
# # Run NetworkManager setup
# # =============================================================================
# echo "  Configuring NetworkManager..."
# echo "${SSH_PASSWORD}" | sudo -S bash \${REMOTE_DIR}/setup-nm.sh

# # =============================================================================
# # Reconnect to WiFi after NetworkManager takes over
# # =============================================================================
# echo "  Reconnecting to WiFi..."
# echo "${SSH_PASSWORD}" | sudo -S nmcli device wifi connect "urbanfarms" password "RoboticFarming"
# sleep 3

# =============================================================================
# Create Python virtual environment and install dependencies
# =============================================================================
echo "  Creating Python virtual environment..."
if [ ! -d "\${REMOTE_DIR}/.venv" ]; then
    python3 -m venv \${REMOTE_DIR}/.venv
fi

echo "  Installing Python dependencies (this may take a minute)..."
\${REMOTE_DIR}/.venv/bin/pip install --upgrade pip --quiet
\${REMOTE_DIR}/.venv/bin/pip install -r \${REMOTE_DIR}/requirements.txt --quiet

# =============================================================================
# Set Bluetooth adapter name
# =============================================================================
echo "  Setting Bluetooth adapter name to '${MACHINE_NAME}'..."

# Check if bluetooth config exists, create if not
if [ ! -f /etc/bluetooth/main.conf ]; then
    echo "  Creating /etc/bluetooth/main.conf..."
    echo "${SSH_PASSWORD}" | sudo -S mkdir -p /etc/bluetooth
    echo "${SSH_PASSWORD}" | sudo -S tee /etc/bluetooth/main.conf > /dev/null << BTCONF
[General]
Name = ${MACHINE_NAME}
DiscoverableTimeout = 0
PairableTimeout = 0

[Policy]
AutoEnable=true
BTCONF
else
    # File exists, update the Name setting
    echo "${SSH_PASSWORD}" | sudo -S sed -i "s/^#Name = .*/Name = ${MACHINE_NAME}/" /etc/bluetooth/main.conf
    echo "${SSH_PASSWORD}" | sudo -S sed -i "s/^Name = .*/Name = ${MACHINE_NAME}/" /etc/bluetooth/main.conf
    # Add Name if it doesn't exist in [General] section
    if ! grep -q "^Name = " /etc/bluetooth/main.conf; then
        echo "${SSH_PASSWORD}" | sudo -S sed -i '/^\[General\]/a Name = ${MACHINE_NAME}' /etc/bluetooth/main.conf
    fi
fi

# Set PRETTY_HOSTNAME to override BlueZ hostname plugin
echo "  Setting PRETTY_HOSTNAME for Bluetooth..."
echo "${SSH_PASSWORD}" | sudo -S bash -c "echo 'PRETTY_HOSTNAME=${MACHINE_NAME}' > /etc/machine-info"

# Restart bluetooth service to apply the name change
echo "${SSH_PASSWORD}" | sudo -S systemctl restart bluetooth

# =============================================================================
# Fix file permissions for preset lock file
# =============================================================================
echo "  Fixing preset file permissions..."
echo "${SSH_PASSWORD}" | sudo -S mkdir -p /home/rooted/te-cli
echo "${SSH_PASSWORD}" | sudo -S chown -R ${PI_USER}:${PI_USER} /home/rooted/te-cli
# Remove stale lock file if owned by root
if [ -f /home/rooted/te-cli/TE_Variable_Values.json.lock ]; then
    echo "${SSH_PASSWORD}" | sudo -S rm -f /home/rooted/te-cli/TE_Variable_Values.json.lock
fi

# =============================================================================
# Install and enable systemd services
# =============================================================================
echo "  Installing systemd service files..."
echo "${SSH_PASSWORD}" | sudo -S cp \${REMOTE_DIR}/rooted-ble.service /etc/systemd/system/
echo "${SSH_PASSWORD}" | sudo -S cp \${REMOTE_DIR}/rooted-ble.timer /etc/systemd/system/

# Install IoT service if it exists
if [ -f "\${REMOTE_DIR}/rooted-iot.service" ]; then
    echo "  Installing AWS IoT service..."
    echo "${SSH_PASSWORD}" | sudo -S cp \${REMOTE_DIR}/rooted-iot.service /etc/systemd/system/
fi

# Reload systemd
echo "  Reloading systemd..."
echo "${SSH_PASSWORD}" | sudo -S systemctl daemon-reload

# Enable the timer (will start the service on boot)
echo "  Enabling BLE timer..."
echo "${SSH_PASSWORD}" | sudo -S systemctl enable rooted-ble.timer

# Start the timer now (optional - for immediate testing)
echo "  Starting BLE timer..."
echo "${SSH_PASSWORD}" | sudo -S systemctl start rooted-ble.timer

# Enable and start IoT service if certificates exist
if [ -f "\${REMOTE_DIR}/certs/certificate.pem.crt" ]; then
    echo "  Enabling AWS IoT service..."
    echo "${SSH_PASSWORD}" | sudo -S systemctl enable rooted-iot.service
    echo "  Starting AWS IoT service..."
    echo "${SSH_PASSWORD}" | sudo -S systemctl start rooted-iot.service
fi

echo "  Setup complete!"
REMOTE_SCRIPT

# =============================================================================
# STEP 6: Cleanup and Summary
# =============================================================================

# Remove local device_config.json and temp IoT files
rm -f "${CONFIG_FILE}" "${SCRIPT_DIR}/device_config.json" 2>/dev/null || true
rm -rf "${IOT_TEMP_DIR}" 2>/dev/null || true

echo ""
echo -e "${GREEN}=========================================${NC}"
echo -e "${GREEN}  Deployment Complete!                  ${NC}"
echo -e "${GREEN}=========================================${NC}"
echo ""
echo "Summary:"
echo "  Pi Host:      ${PI_HOST}"
echo "  Machine Name: ${MACHINE_NAME}"
echo "  Device ID:    ${DEVICE_UUID}"
echo "  Install Path: ${REMOTE_DIR}"
if [ -n "$IOT_ENDPOINT" ]; then
echo "  IoT Endpoint: ${IOT_ENDPOINT}"
echo "  IoT Status:   Provisioned"
else
echo "  IoT Status:   Not provisioned"
fi
echo ""
echo "Services:"
echo "  - rooted-ble.timer   : BLE provisioning (runs on boot)"
if [ -n "$IOT_ENDPOINT" ]; then
echo "  - rooted-iot.service : AWS IoT connection (always running)"
fi
echo ""
echo "Useful commands on the Pi:"
echo "  sudo systemctl status rooted-ble.timer   # Check BLE timer"
echo "  sudo systemctl status rooted-ble.service # Check BLE service"
if [ -n "$IOT_ENDPOINT" ]; then
echo "  sudo systemctl status rooted-iot.service # Check IoT service"
echo "  sudo journalctl -u rooted-iot -f         # Watch IoT logs"
fi
echo ""
echo -e "${YELLOW}Important: Save the Device ID above - you'll need it to identify this machine.${NC}"
echo ""
echo "You can now disconnect the ethernet cable."
