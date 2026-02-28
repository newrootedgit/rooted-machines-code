#!/bin/bash
# Setup script for AWS IoT on Raspberry Pi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CERTS_DIR="$SCRIPT_DIR/certs"

# Create certs directory
mkdir -p "$CERTS_DIR"

# Download Amazon Root CA certificate
echo "Downloading Amazon Root CA certificate..."
curl -o "$CERTS_DIR/AmazonRootCA1.pem" https://www.amazontrust.com/repository/AmazonRootCA1.pem

# Set proper permissions
chmod 644 "$CERTS_DIR/AmazonRootCA1.pem"

echo "AWS IoT setup complete!"
echo ""
echo "Next steps:"
echo "1. Set environment variables in /etc/environment or systemd service:"
echo "   AWS_REGION=us-west-2"
echo "   AWS_IOT_ENDPOINT=a2jotz5yvt34r4-ats.iot.us-west-2.amazonaws.com"
echo "   AWS_IOT_POLICY_NAME=rooted-machine-policy-prod"
echo "   AWS_ACCESS_KEY_ID=<your-key>"
echo "   AWS_SECRET_ACCESS_KEY=<your-secret>"
echo ""
echo "2. Install Python dependencies:"
echo "   pip3 install -r requirements.txt"
