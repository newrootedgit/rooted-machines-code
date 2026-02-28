# Raspberry Pi BLE Provisioning with AWS IoT

This directory contains the BLE provisioning service for Raspberry Pi machines, including AWS IoT connectivity for real-time status monitoring.

## Prerequisites

- Raspberry Pi with Bluetooth
- Python 3.7+
- NetworkManager (nmcli)
- AWS Account with IoT Core enabled

## AWS Setup

### 1. Create IAM User for Pi Devices

```bash
# Create IAM user
aws iam create-user --user-name rooted-pi-iot

# Create access key
aws iam create-access-key --user-name rooted-pi-iot
# Save the AccessKeyId and SecretAccessKey - you'll need these!
```

### 2. Create IAM Policy

Create a file `pi-iot-policy.json`:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": [
        "iot:CreateThing",
        "iot:CreateKeysAndCertificate",
        "iot:AttachPolicy",
        "iot:AttachThingPrincipal",
        "iot:DescribeThing"
      ],
      "Resource": "*"
    }
  ]
}
```

Attach the policy:

```bash
aws iam put-user-policy \
  --user-name rooted-pi-iot \
  --policy-name RootedPiIoTPolicy \
  --policy-document file://pi-iot-policy.json
```

### 3. Get IoT Endpoint

```bash
aws iot describe-endpoint --endpoint-type iot:Data-ATS
# Save the endpoint address (e.g., a2jotz5yvt34r4-ats.iot.us-west-2.amazonaws.com)
```

## Deployment to Raspberry Pi

### 1. Deploy Files

```bash
./deploy-with-iot.sh <pi-ip-address>
```

### 2. Configure AWS Credentials on Pi

SSH into the Pi and edit the systemd service:

```bash
ssh rooted@<pi-ip>
sudo nano /etc/systemd/system/rooted-ble.service
```

Update these environment variables with your actual values:

```ini
Environment="AWS_REGION=us-west-2"
Environment="AWS_IOT_ENDPOINT=<your-iot-endpoint>.iot.us-west-2.amazonaws.com"
Environment="AWS_IOT_POLICY_NAME=rooted-machine-policy-prod"
Environment="AWS_ACCESS_KEY_ID=<your-access-key-id>"
Environment="AWS_SECRET_ACCESS_KEY=<your-secret-access-key>"
```

Restart the service:

```bash
sudo systemctl daemon-reload
sudo systemctl restart rooted-ble
sudo systemctl status rooted-ble
```

## How It Works

1. **BLE Advertising**: Pi advertises as a BLE peripheral with device name
2. **WiFi Provisioning**: Mobile app connects and sends WiFi credentials
3. **WiFi Connection**: Pi connects to the specified network
4. **AWS IoT Registration**: Pi auto-registers as an IoT Thing with AWS
5. **MQTT Connection**: Pi establishes persistent MQTT connection to AWS IoT
6. **Lifecycle Events**: AWS IoT detects connection/disconnection
7. **Lambda Trigger**: Lifecycle events trigger Lambda function
8. **API Update**: Lambda calls backend API to update machine status
9. **Dashboard**: Frontend displays real-time online/offline status

## Testing

### Test BLE Service

```bash
# On Pi
sudo systemctl status rooted-ble
sudo journalctl -u rooted-ble -f
```

### Test AWS IoT Connection

After provisioning WiFi, check if the Pi registered:

```bash
# From your computer
aws iot describe-thing --thing-name <device-id>
```

Check if it's connected:

```bash
# View CloudWatch logs for Lambda
aws logs tail /aws/lambda/rooted-machine-lifecycle-prod --follow
```

### Test End-to-End

1. Provision WiFi via mobile app
2. Check Pi logs: `sudo journalctl -u rooted-ble -f`
3. Verify Thing created: `aws iot list-things`
4. Check Lambda logs: `aws logs tail /aws/lambda/rooted-machine-lifecycle-prod --follow`
5. Check database: Machine should show status="online"
6. Open dashboard: Machine should display as online with WiFi network

## Troubleshooting

### Pi Not Registering with AWS IoT

```bash
# Check AWS credentials
ssh rooted@<pi-ip>
sudo systemctl status rooted-ble
sudo journalctl -u rooted-ble -n 50

# Verify credentials work
python3 -c "import boto3; print(boto3.client('iot', region_name='us-west-2').describe_endpoint(endpointType='iot:Data-ATS'))"
```

### Pi Not Connecting to MQTT

```bash
# Check certificates exist
ls -la /opt/rooted-ble/certs/

# Check certificate permissions
ls -l /opt/rooted-ble/certs/private.pem.key
# Should be: -rw------- (600)

# Verify IoT endpoint
echo $AWS_IOT_ENDPOINT
```

### Lambda Not Triggering

```bash
# Check IoT Rule
aws iot get-topic-rule --rule-name rooted_machine_lifecycle_prod

# Check Lambda permissions
aws lambda get-policy --function-name rooted-machine-lifecycle-prod

# Test Lambda directly
aws lambda invoke \
  --function-name rooted-machine-lifecycle-prod \
  --payload '{"clientId":"test-device","eventType":"connected","timestamp":"2026-01-23T14:00:00Z"}' \
  response.json
```

### API Not Receiving Calls

```bash
# Test API endpoint
curl -X POST http://localhost:8000/internal/machines/lifecycle-event \
  -H "Authorization: Bearer <your-lambda-secret-token>" \
  -H "Content-Type: application/json" \
  -d '{"deviceId":"test-device","eventType":"connected","timestamp":"2026-01-23T14:00:00Z"}'
```

## Security Notes

⚠️ **Important**: The IAM credentials approach is suitable for development and small deployments (< 20 devices). For production:

- Use AWS IoT Fleet Provisioning
- Rotate IAM credentials regularly
- Use AWS Secrets Manager for credential storage
- Implement certificate-based authentication

## Files

- `provisioner.py` - Main BLE provisioning service
- `aws_iot_registration.py` - AWS IoT registration and MQTT connection
- `setup-aws-iot.sh` - Setup script for AWS IoT on Pi
- `deploy-with-iot.sh` - Deployment script
- `requirements.txt` - Python dependencies
- `rooted-ble.service` - Systemd service file
- `device_config.json` - Device configuration (auto-generated)
- `certs/` - AWS IoT certificates (auto-generated)

## Architecture

```
Mobile App (BLE)
    ↓
Raspberry Pi (BLE Service)
    ↓ (WiFi Credentials)
NetworkManager (nmcli)
    ↓ (WiFi Connected)
AWS IoT Registration (boto3)
    ↓ (Thing + Certificates)
AWS IoT Core (MQTT)
    ↓ (Lifecycle Event)
Lambda Function
    ↓ (HTTPS POST)
Backend API
    ↓
PostgreSQL Database
    ↓
Frontend Dashboard
```
