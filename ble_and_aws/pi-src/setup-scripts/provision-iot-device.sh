#!/bin/bash
# AWS IoT Device Provisioning Script
# Run this on your Mac to provision a new Rooted device
#
# Usage:
#   ./provision-iot-device.sh <thing-name>                    # Interactive mode
#   ./provision-iot-device.sh <thing-name> --output-dir /tmp  # Non-interactive, output to dir

set -e

# Configuration
POLICY_NAME="RootedDevicePolicy"
AWS_REGION="${AWS_REGION:-us-west-2}"
PI_USER="${PI_USER:-rooted}"
PI_HOST="${PI_HOST:-rootedpi}"
PI_CERTS_DIR="/opt/rooted-ble/certs"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Parse arguments
THING_NAME="${1:-}"
OUTPUT_DIR=""
NON_INTERACTIVE=false

while [[ $# -gt 1 ]]; do
    case "$2" in
        --output-dir)
            OUTPUT_DIR="$3"
            NON_INTERACTIVE=true
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done

echo -e "${GREEN}=== Rooted IoT Device Provisioning ===${NC}"
echo ""

# Check for required tools
if ! command -v aws &> /dev/null; then
    echo -e "${RED}Error: AWS CLI not installed${NC}"
    exit 1
fi

# Get device name from argument or prompt
if [ -z "$THING_NAME" ]; then
    read -p "Enter device/thing name (e.g., rooted-device-001): " THING_NAME
fi

if [ -z "$THING_NAME" ]; then
    echo -e "${RED}Error: Thing name is required${NC}"
    exit 1
fi

echo -e "${YELLOW}Provisioning device: ${THING_NAME}${NC}"
echo ""

# Create temp directory for certs (or use provided output dir)
if [ -n "$OUTPUT_DIR" ]; then
    TEMP_DIR="$OUTPUT_DIR"
    mkdir -p "$TEMP_DIR"
else
    TEMP_DIR=$(mktemp -d)
fi
echo "Working in: $TEMP_DIR"

# Step 1: Create the Thing
echo -e "\n${GREEN}[1/6] Creating IoT Thing...${NC}"
if aws iot describe-thing --thing-name "$THING_NAME" --region "$AWS_REGION" 2>/dev/null; then
    echo "Thing '$THING_NAME' already exists, skipping creation"
else
    aws iot create-thing --thing-name "$THING_NAME" --region "$AWS_REGION"
    echo "Created Thing: $THING_NAME"
fi

# Step 2: Create certificate and keys
echo -e "\n${GREEN}[2/6] Creating certificates...${NC}"
CERT_OUTPUT=$(aws iot create-keys-and-certificate \
    --set-as-active \
    --certificate-pem-outfile "$TEMP_DIR/certificate.pem.crt" \
    --private-key-outfile "$TEMP_DIR/private.pem.key" \
    --public-key-outfile "$TEMP_DIR/public.pem.key" \
    --region "$AWS_REGION")

CERT_ARN=$(echo "$CERT_OUTPUT" | grep -o '"certificateArn": "[^"]*"' | cut -d'"' -f4)
CERT_ID=$(echo "$CERT_OUTPUT" | grep -o '"certificateId": "[^"]*"' | cut -d'"' -f4)

echo "Certificate ARN: $CERT_ARN"
echo "Certificate ID: $CERT_ID"

# Step 3: Create policy (if doesn't exist)
echo -e "\n${GREEN}[3/6] Creating/verifying IoT policy...${NC}"
if aws iot get-policy --policy-name "$POLICY_NAME" --region "$AWS_REGION" 2>/dev/null; then
    echo "Policy '$POLICY_NAME' already exists"
else
    aws iot create-policy \
        --policy-name "$POLICY_NAME" \
        --policy-document '{
            "Version": "2012-10-17",
            "Statement": [
                {
                    "Effect": "Allow",
                    "Action": ["iot:Connect"],
                    "Resource": "*"
                },
                {
                    "Effect": "Allow",
                    "Action": ["iot:Publish", "iot:Receive"],
                    "Resource": "*"
                },
                {
                    "Effect": "Allow",
                    "Action": ["iot:Subscribe"],
                    "Resource": "*"
                }
            ]
        }' \
        --region "$AWS_REGION"
    echo "Created policy: $POLICY_NAME"
fi

# Step 4: Attach policy to certificate
echo -e "\n${GREEN}[4/6] Attaching policy to certificate...${NC}"
aws iot attach-policy \
    --policy-name "$POLICY_NAME" \
    --target "$CERT_ARN" \
    --region "$AWS_REGION" 2>/dev/null || echo "Policy may already be attached"
echo "Policy attached"

# Step 5: Attach certificate to Thing
echo -e "\n${GREEN}[5/6] Attaching certificate to Thing...${NC}"
aws iot attach-thing-principal \
    --thing-name "$THING_NAME" \
    --principal "$CERT_ARN" \
    --region "$AWS_REGION"
echo "Certificate attached to Thing"

# Step 6: Download Root CA
echo -e "\n${GREEN}[6/6] Downloading Amazon Root CA...${NC}"
curl -s -o "$TEMP_DIR/AmazonRootCA1.pem" https://www.amazontrust.com/repository/AmazonRootCA1.pem
echo "Root CA downloaded"

# Get IoT endpoint
IOT_ENDPOINT=$(aws iot describe-endpoint --endpoint-type iot:Data-ATS --region "$AWS_REGION" --query 'endpointAddress' --output text)
echo -e "\n${YELLOW}IoT Endpoint: ${IOT_ENDPOINT}${NC}"

# Create device config
cat > "$TEMP_DIR/device_config.json" << EOF
{
    "device_id": "$THING_NAME",
    "device_name": "$THING_NAME",
    "aws_iot_endpoint": "$IOT_ENDPOINT",
    "aws_region": "$AWS_REGION"
}
EOF

echo -e "\n${GREEN}=== Certificates created successfully ===${NC}"
echo ""
echo "Files in $TEMP_DIR:"
ls -la "$TEMP_DIR"

# In non-interactive mode, skip the copy prompt (caller handles it)
if [ "$NON_INTERACTIVE" = true ]; then
    echo ""
    echo -e "${GREEN}=== Provisioning complete (non-interactive) ===${NC}"
    echo "Certificates saved to: $TEMP_DIR"
    echo "IoT Endpoint: $IOT_ENDPOINT"
    # Export for calling script
    echo "$IOT_ENDPOINT" > "$TEMP_DIR/.iot_endpoint"
    exit 0
fi

# Ask about copying to Pi (interactive mode only)
echo ""
read -p "Copy certificates to Pi ($PI_USER@$PI_HOST)? [y/N]: " COPY_TO_PI

if [[ "$COPY_TO_PI" =~ ^[Yy]$ ]]; then
    echo -e "\n${YELLOW}Copying to Pi...${NC}"

    # Create certs directory on Pi
    ssh "$PI_USER@$PI_HOST" "sudo mkdir -p $PI_CERTS_DIR && sudo chown $PI_USER:$PI_USER $PI_CERTS_DIR"

    # Copy certificate files
    scp "$TEMP_DIR/certificate.pem.crt" "$PI_USER@$PI_HOST:$PI_CERTS_DIR/"
    scp "$TEMP_DIR/private.pem.key" "$PI_USER@$PI_HOST:$PI_CERTS_DIR/"
    scp "$TEMP_DIR/AmazonRootCA1.pem" "$PI_USER@$PI_HOST:$PI_CERTS_DIR/"
    scp "$TEMP_DIR/device_config.json" "$PI_USER@$PI_HOST:/opt/rooted-ble/"

    # Set proper permissions on Pi
    ssh "$PI_USER@$PI_HOST" "chmod 600 $PI_CERTS_DIR/private.pem.key"

    echo -e "${GREEN}Certificates copied to Pi!${NC}"
else
    echo ""
    echo "To manually copy later, run:"
    echo "  scp $TEMP_DIR/*.pem* $PI_USER@$PI_HOST:$PI_CERTS_DIR/"
    echo "  scp $TEMP_DIR/device_config.json $PI_USER@$PI_HOST:/opt/rooted-ble/"
fi

echo ""
echo -e "${GREEN}=== Provisioning complete ===${NC}"
echo ""
echo "Summary:"
echo "  Thing Name:    $THING_NAME"
echo "  Certificate:   $CERT_ID"
echo "  IoT Endpoint:  $IOT_ENDPOINT"
echo "  Region:        $AWS_REGION"
echo ""
echo "Next steps:"
echo "  1. Verify certs are on Pi: ssh $PI_USER@$PI_HOST 'ls -la $PI_CERTS_DIR'"
echo "  2. Test connection: ssh $PI_USER@$PI_HOST 'cd /opt/rooted-ble && python aws_iot_registration.py'"
