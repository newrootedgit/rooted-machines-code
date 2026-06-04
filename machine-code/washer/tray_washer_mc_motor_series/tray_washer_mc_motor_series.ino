#include "ClearCore.h"
#include <SPI.h>

#define DriveMotor ConnectorM0

#define SPEED_POT ConnectorA9  // Potentiometer for drive speed control

#define baudRate 9600

#define HANDLE_ALERTS (0)
#define HANDLE_MOTOR_FAULTS (1)

int accelerationLimit = 50000; // pulses per sec^2
int screen2Var1Value = 0;  // Value for motor RPM
int screen3Var1Value = 0;  // Value saved for later implementation

// ---- Potentiometer speed control ----
// A10 reads the speed pot. The pot tops out at 7.0V (not the full 10V the
// ClearCore analog input can read), so map 0-7V across the full speed range.
// MAX_VELOCITY is in pulses/sec (ClearCore MoveVelocity units) -- TUNE this
// to the fastest safe wash speed for this drive.
const float   POT_MAX_VOLTAGE = 7.0;    // pot ceiling, not 10V -- be mindful
const int32_t MAX_VELOCITY    = 10000;  // pulses/sec at full pot -- TUNE
const float   POT_DEADBAND_V  = 0.1;    // below this -> motor stopped

void PrintAlerts();
void HandleAlerts();
 
void HandleMotorFaults();

void setup() {

    Serial.begin(baudRate);
    delay(2000); // Give time for Serial to initialize

    // Configure A10 as an analog input so AnalogVoltage() reads the speed pot.
    SPEED_POT.Mode(Connector::INPUT_ANALOG);

    ////////////////////////////////////////////////////////
    /////////////////// Drive Motor Set Up /////////////////
    ////////////////////////////////////////////////////////

    MotorMgr.MotorModeSet(MotorManager::MOTOR_M0M1, Connector::CPM_MODE_STEP_AND_DIR);
    DriveMotor.HlfbMode(MotorDriver::HLFB_MODE_HAS_BIPOLAR_PWM);
    DriveMotor.HlfbCarrier(MotorDriver::HLFB_CARRIER_482_HZ);
    DriveMotor.AccelMax(accelerationLimit);
    DriveMotor.VelMax(MAX_VELOCITY);
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
    // Read the speed pot on A10 and map it to a commanded velocity.
    float voltage = SPEED_POT.AnalogVoltage();

    // Clamp to the pot's usable range (tops out at ~7V) before scaling.
    if (voltage < 0) voltage = 0;
    if (voltage > POT_MAX_VOLTAGE) voltage = POT_MAX_VOLTAGE;

    int32_t velocity;
    if (voltage < POT_DEADBAND_V) {
        velocity = 0; // pot at/near minimum -> stop
    } else {
        velocity = (int32_t)((voltage / POT_MAX_VOLTAGE) * MAX_VELOCITY);
    }

    Serial.print("Pot: ");
    Serial.print(voltage);
    Serial.print("V  Vel: ");
    Serial.println(velocity);

    DriveMoveVelocity(velocity);

    delay(50); // responsive to pot changes
}

// Commands a continuous step-and-direction velocity (pulses/sec). Non-blocking:
// it sets the target and returns so loop() can keep tracking the pot. The sign
// of `velocity` sets direction -- flip it here if the drive runs backwards.
bool DriveMoveVelocity(int32_t velocity) {
    // Check if a motor fault is currently preventing motion.
    // Clear fault if configured to do so.
    if (DriveMotor.StatusReg().bit.MotorInFault) {
        if (HANDLE_MOTOR_FAULTS) {
            Serial.println("Motor fault detected. Move canceled.");
            HandleMotorFaults();
        } else {
            Serial.println("Motor fault detected. Move canceled. Enable automatic fault handling by setting HANDLE_MOTOR_FAULTS to 1.");
        }
        return false;
    }

    DriveMotor.MoveVelocity(velocity);
    return true;
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
