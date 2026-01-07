// #include "ClearCore.h"
// #include <SPI.h>
// #include <Ethernet.h>

// #define BeltMotor ConnectorM2
// #define BladeMotor ConnectorM0


// #define INPUT_A_B_FILTER 20

// #define baudRate 9600

// #define HANDLE_ALERTS (0)
// #define HANDLE_MOTOR_FAULTS (0)


#include "ClearCore.h"
#include <SPI.h>
#include <Ethernet.h>

#define BeltMotor ConnectorM2
#define BladeMotor ConnectorM0

#define inputPin1 DI6  // Blade Soft Kill Switch

#define INPUT_A_B_FILTER 20

#define baudRate 9600

#define HANDLE_ALERTS (1)
#define HANDLE_MOTOR_FAULTS (1)


// Ethernet setup
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};  // Replace with your Arduino's MAC address
IPAddress ip(192, 168, 10, 2);                      // Static IP for Arduino on the private subnet
IPAddress serverIp(192, 168, 10, 1);                // Updated IP of Raspberry Pi on the private subnet
#define PORT_NUM 8888
#define MAX_PACKET_LENGTH 100
unsigned char packetReceived[MAX_PACKET_LENGTH];
EthernetClient client;

int accelerationLimit = 100000; // pulses per sec^2
int screen2Var1Value = 0;  // Value for motor RPM
int screen3Var1Value = 0;  // Value saved for later implementation

bool MoveAtVelocity(int32_t velocity);
void PrintAlerts();
void HandleAlerts();

bool RampToVelocitySelection(int velocityIndex);
void HandleMotorFaults();

void setup() {

    pinMode(inputPin1, INPUT);

    Serial.begin(baudRate);
    delay(2000); // Give time for Serial to initialize

    ////////////////////////////////////////////////////////
    /////////////////// Belt Motor Set Up /////////////////
    ////////////////////////////////////////////////////////

    MotorMgr.MotorModeSet(MotorManager::MOTOR_M2M3, Connector::CPM_MODE_STEP_AND_DIR);
    BeltMotor.HlfbMode(MotorDriver::HLFB_MODE_HAS_BIPOLAR_PWM);
    BeltMotor.HlfbCarrier(MotorDriver::HLFB_CARRIER_482_HZ);
    BeltMotor.AccelMax(accelerationLimit);
    BeltMotor.EnableRequest(true);
    Serial.println("BeltMotor Enabled");

    uint32_t startTime = millis();
    while (BeltMotor.HlfbState() != MotorDriver::HLFB_ASSERTED &&
           !BeltMotor.StatusReg().bit.AlertsPresent &&
           millis() - startTime < 5000) {
        continue;
    }
    if (BeltMotor.StatusReg().bit.AlertsPresent) {
        if (HANDLE_ALERTS) HandleAlerts();
    } else {
        Serial.println("BeltMotor Ready");
    }

    ////////////////////////////////////////////////////////
    /////////////////// Blade Motor Set Up /////////////////
    ////////////////////////////////////////////////////////

    MotorMgr.MotorModeSet(MotorManager::MOTOR_M0M1, Connector::CPM_MODE_A_DIRECT_B_DIRECT);
    BladeMotor.HlfbMode(MotorDriver::HLFB_MODE_HAS_BIPOLAR_PWM);
    BladeMotor.HlfbCarrier(MotorDriver::HLFB_CARRIER_482_HZ);
    BladeMotor.MotorInAState(false);
    BladeMotor.MotorInBState(false);
    BladeMotor.EnableRequest(true);
    Serial.println("BladeMotor Enabled");

    startTime = millis();
    while (BladeMotor.HlfbState() != MotorDriver::HLFB_ASSERTED &&
           !BladeMotor.StatusReg().bit.MotorInFault &&
           millis() - startTime < 5000) {
        continue;
    }
    if (BladeMotor.StatusReg().bit.MotorInFault) {
        if (HANDLE_MOTOR_FAULTS) HandleMotorFaults();
    } else {
        Serial.println("BladeMotor Ready");
    }

    ////////////////////////////////////////////////////////
    /////////////////// Ethernet Connection ////////////////
    ////////////////////////////////////////////////////////

    Ethernet.begin(mac, ip); // Set static IP
    while (Ethernet.linkStatus() == LinkOFF) {
        Serial.println("Waiting for Ethernet link...");
        delay(1000);
    }

    if (client.connect(serverIp, PORT_NUM)) {
        Serial.println("Connected to server.");
    } else {
        Serial.println("Failed to connect to server.");
    }
}

void loop() {
    bool inputState1 = digitalRead(inputPin1);

    if (client.available() > 0) {
        int len = client.read(packetReceived, MAX_PACKET_LENGTH - 1);
        packetReceived[len] = '\0';

        // Print the received message
        Serial.print("Received message: ");
        Serial.println((char *)packetReceived);

        // Initialize variables to hold parsed values
        int screen5Var1Value = 1; // initialize value to 2 which corresponds to the blade being off. 1->clean, 2->off, 3->low, 4->high
        int screen3Var1Value = 0;

        // Find and extract values for screen_5_var_1 and screen_3_var_1
        char* screen5Ptr = strstr((char*)packetReceived, "screen_5_var_1: ");
        char* screen3Ptr = strstr((char*)packetReceived, "screen_3_var_1: ");
        
        if (screen5Ptr != NULL) {
            screen5Var1Value = atoi(screen5Ptr + strlen("screen_5_var_1: "));
        }
        
        if (screen3Ptr != NULL) {
            screen3Var1Value = atoi(screen3Ptr + strlen("screen_3_var_1: "));
        }

        // Debug output to verify values
        Serial.print("Parsed Screen 5 Var 1 Value: ");
        Serial.println(screen5Var1Value);
        Serial.print("Parsed Screen 3 Var 1 Value: ");
        Serial.println(screen3Var1Value);

        // Calculate blade speed and belt speed based on parsed values
        int blade_speed = screen5Var1Value;
        int belt_speed = -150 * 5 * screen3Var1Value; // scaled to menu displayin 0-20
        int belt_delay = abs(65000000 / belt_speed);

        if (inputState1){ //check soft kill switch state
          // Execute functions with calculated speeds
          RampToVelocitySelection(blade_speed);
          MoveAtVelocity(belt_speed);
        }
        if (!inputState1){ //check soft kill switch state
          // Execute functions with calculated speeds
          RampToVelocitySelection(1);
          MoveAtVelocity(0);
        }
        // Optional delay for belt movement
        delay(100);
        
    }
     else {
        // Serial.println("Server disconnected. Reconnecting...");
        client.connect(serverIp, PORT_NUM);
        delay(500);

    }        
    


}

bool MoveAtVelocity(int velocity) {
    // Check if a motor alert is currently preventing motion
  // Clear alert if configured to do so 
    if (BeltMotor.StatusReg().bit.AlertsPresent) {
    Serial.println("Motor alert detected.");    
    PrintAlerts();
    if(HANDLE_ALERTS){
      HandleAlerts();
    } else {
      Serial.println("Enable automatic alert handling by setting HANDLE_ALERTS to 1.");
    }
    Serial.println("Move canceled.");   
    Serial.println();
        return false;
    }

    // Serial.print("Moving at velocity: ");
    // Serial.println(velocity);

    // Command the velocity move
    BeltMotor.MoveVelocity(velocity);

    // Waits for the step command to ramp up/down to the commanded velocity. 
    // This time will depend on your Acceleration Limit.
    while (!BeltMotor.StatusReg().bit.AtTargetVelocity) {
        continue;
    }

    return true; 
}

bool RampToVelocitySelection(int velocityIndex) {
    // Check if a motor fault is currently preventing motion
  // Clear fault if configured to do so 
    if (BladeMotor.StatusReg().bit.MotorInFault) {
    if(HANDLE_MOTOR_FAULTS){
      Serial.println("Motor fault detected. Move canceled.");
      HandleMotorFaults();
    } else {
      Serial.println("Motor fault detected. Move canceled. Enable automatic fault handling by setting HANDLE_MOTOR_FAULTS to 1.");
    }
        return false;
    }

    // Serial.print("Moving to Velocity Selection: ");
    // Serial.print(velocityIndex);

    switch (velocityIndex) {
        case 1:
            // Sets Input A and B for velocity 1
            BladeMotor.MotorInAState(false);
            BladeMotor.MotorInBState(false);
            Serial.println(" (Inputs A Off/B Off)");
            break;       
        case 2:
            // Sets Input A and B for velocity 2
            BladeMotor.MotorInAState(true);
            BladeMotor.MotorInBState(false);
            Serial.println(" (Inputs A On/B Off)");
            break; 
        case 3:
            // Sets Input A and B for velocity 3
            BladeMotor.MotorInAState(false);
            BladeMotor.MotorInBState(true);
            Serial.println(" (Inputs A Off/B On)");
            break;
        case 4:
            // Sets Input A and B for velocity 4
            BladeMotor.MotorInAState(true);
            BladeMotor.MotorInBState(true);
            Serial.println(" (Inputs A On/B On)");
            break;
        default:
            // If this case is reached then an incorrect velocityIndex was
            // entered
            return false;
    }

    // Ensures this delay is at least 20ms longer than the Input A, B filter
    // setting in MSP
    delay(20 + INPUT_A_B_FILTER);

    // Waits for HLFB to assert (signaling the move has successfully reached its
    // target velocity)
    Serial.println("Moving.. Waiting for HLFB");
    while (BladeMotor.HlfbState() != MotorDriver::HLFB_ASSERTED &&
      !BladeMotor.StatusReg().bit.MotorInFault) {
        continue;
    }
  // Check if a motor faulted during move
  // Clear fault if configured to do so 
    if (BladeMotor.StatusReg().bit.MotorInFault) {
    Serial.println("Motor fault detected.");    
    if(HANDLE_MOTOR_FAULTS){
      HandleMotorFaults();
    } else {
      Serial.println("Enable automatic fault handling by setting HANDLE_MOTOR_FAULTS to 1.");
    }
    Serial.println("Motion may not have completed as expected. Proceed with caution.");
    Serial.println();
    return false;
    } else {
    Serial.println("Move Done");
    return true;
  }
}

void PrintAlerts(){
  // report status of alerts
  Serial.println("Alerts present: ");
  if(BeltMotor.AlertReg().bit.MotionCanceledInAlert){
    Serial.println("    MotionCanceledInAlert "); }
  if(BeltMotor.AlertReg().bit.MotionCanceledPositiveLimit){
    Serial.println("    MotionCanceledPositiveLimit "); }
  if(BeltMotor.AlertReg().bit.MotionCanceledNegativeLimit){
    Serial.println("    MotionCanceledNegativeLimit "); }
  if(BeltMotor.AlertReg().bit.MotionCanceledSensorEStop){
    Serial.println("    MotionCanceledSensorEStop "); }
  if(BeltMotor.AlertReg().bit.MotionCanceledMotorDisabled){
    Serial.println("    MotionCanceledMotorDisabled "); }
  if(BeltMotor.AlertReg().bit.MotorFaulted){
    Serial.println("    MotorFaulted ");
  }
}

void HandleAlerts(){
  if(BeltMotor.AlertReg().bit.MotorFaulted){
    // if a motor fault is present, clear it by cycling enable
    Serial.println("Faults present. Cycling enable signal to motor to clear faults.");
    BeltMotor.EnableRequest(false);
    Delay_ms(10);
    BeltMotor.EnableRequest(true);
  }
  // clear alerts
  Serial.println("Clearing alerts.");
  BeltMotor.ClearAlerts();
}

void HandleMotorFaults(){
  Serial.println("Handling fault: clearing faults by cycling enable signal to motor.");
  BladeMotor.EnableRequest(false);
  Delay_ms(10);
  BladeMotor.EnableRequest(true);
  Delay_ms(100);
}
