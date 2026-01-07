#include "ClearCore.h"
#include <SPI.h>

#define DriveMotor ConnectorM0

#define inputPin1 DI6  // Drive Speed 1
#define inputPin2 DI7  // Drive Speed 2

#define INPUT_A_B_FILTER 20

#define baudRate 9600

#define HANDLE_ALERTS (0)
#define HANDLE_MOTOR_FAULTS (1)

int accelerationLimit = 50000; // pulses per sec^2
int screen2Var1Value = 0;  // Value for motor RPM
int screen3Var1Value = 0;  // Value saved for later implementation

void PrintAlerts();
void HandleAlerts();

void HandleMotorFaults();

void setup() {

    Serial.begin(baudRate);
    delay(2000); // Give time for Serial to initialize

    pinMode(inputPin1, INPUT);
    pinMode(inputPin2, INPUT);

    ////////////////////////////////////////////////////////
    /////////////////// Drive Motor Set Up /////////////////
    ////////////////////////////////////////////////////////

    MotorMgr.MotorModeSet(MotorManager::MOTOR_M0M1, Connector::CPM_MODE_A_DIRECT_B_DIRECT);
    DriveMotor.HlfbMode(MotorDriver::HLFB_MODE_HAS_BIPOLAR_PWM);
    DriveMotor.HlfbCarrier(MotorDriver::HLFB_CARRIER_482_HZ);
    DriveMotor.MotorInAState(false);
    DriveMotor.MotorInBState(false);
    DriveMotor.EnableRequest(true);
    Serial.println("DriveMotor Enabled");

    int startTime = millis();
    while (DriveMotor.HlfbState() != MotorDriver::HLFB_ASSERTED &&
           !DriveMotor.StatusReg().bit.MotorInFault &&
           millis() - startTime < 5000) {
        continue;
    }
    if (DriveMotor.StatusReg().bit.MotorInFault) {
        if (HANDLE_MOTOR_FAULTS) HandleMotorFaults();
    } else {
        Serial.println("Drive Motor Ready");
    }
}

void loop() {
    bool inputState1 = digitalRead(inputPin1);
    bool inputState2 = digitalRead(inputPin2);

    Serial.print(inputState1);
    Serial.print(inputState2);

    if (!inputState1 && !inputState2){ //check soft kill switch state
      // Execute functions with calculated speeds
      RampToVelocitySelection(3);
    }
    else if (inputState1){ //check soft kill switch state
      // Execute functions with calculated speeds
      RampToVelocitySelection(2);
    }
    else if (inputState2){ //check soft kill switch state
      // Execute functions with calculated speeds
      RampToVelocitySelection(4);
    }
    delay(1000);     

}

bool RampToVelocitySelection(int velocityIndex) {
    // Check if a motor fault is currently preventing motion
  // Clear fault if configured to do so 
    if (DriveMotor.StatusReg().bit.MotorInFault) {
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
            DriveMotor.MotorInAState(false);
            DriveMotor.MotorInBState(false);
            Serial.println(" (Inputs A Off/B Off)");
            break;       
        case 2:
            // Sets Input A and B for velocity 2
            DriveMotor.MotorInAState(true);
            DriveMotor.MotorInBState(false);
            Serial.println(" (Inputs A On/B Off)");
            break; 
        case 3:
            // Sets Input A and B for velocity 3
            DriveMotor.MotorInAState(false);
            DriveMotor.MotorInBState(true);
            Serial.println(" (Inputs A Off/B On)");
            break;
        case 4:
            // Sets Input A and B for velocity 4
            DriveMotor.MotorInAState(true);
            DriveMotor.MotorInBState(true);
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
    while (DriveMotor.HlfbState() != MotorDriver::HLFB_ASSERTED &&
      !DriveMotor.StatusReg().bit.MotorInFault) {
        continue;
    }
  // Check if a motor faulted during move
  // Clear fault if configured to do so 
    if (DriveMotor.StatusReg().bit.MotorInFault) {
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
  if(DriveMotor.AlertReg().bit.MotionCanceledInAlert){
    Serial.println("    MotionCanceledInAlert "); }
  if(DriveMotor.AlertReg().bit.MotionCanceledPositiveLimit){
    Serial.println("    MotionCanceledPositiveLimit "); }
  if(DriveMotor.AlertReg().bit.MotionCanceledNegativeLimit){
    Serial.println("    MotionCanceledNegativeLimit "); }
  if(DriveMotor.AlertReg().bit.MotionCanceledSensorEStop){
    Serial.println("    MotionCanceledSensorEStop "); }
  if(DriveMotor.AlertReg().bit.MotionCanceledMotorDisabled){
    Serial.println("    MotionCanceledMotorDisabled "); }
  if(DriveMotor.AlertReg().bit.MotorFaulted){
    Serial.println("    MotorFaulted ");
  }
}

void HandleAlerts(){
  if(DriveMotor.AlertReg().bit.MotorFaulted){
    // if a motor fault is present, clear it by cycling enable
    Serial.println("Faults present. Cycling enable signal to motor to clear faults.");
    DriveMotor.EnableRequest(false);
    Delay_ms(10);
    DriveMotor.EnableRequest(true);
  }
  // clear alerts
  Serial.println("Clearing alerts.");
  DriveMotor.ClearAlerts();
}

void HandleMotorFaults(){
  Serial.println("Handling fault: clearing faults by cycling enable signal to motor.");
  DriveMotor.EnableRequest(false);
  Delay_ms(10);
  DriveMotor.EnableRequest(true);
  Delay_ms(100);
}
