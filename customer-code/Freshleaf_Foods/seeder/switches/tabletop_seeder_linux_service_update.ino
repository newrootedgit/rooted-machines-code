#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include "ClearCore.h"

#define BeltMotor ConnectorM0
#define HopperMotor ConnectorM2
#define INPUT_A_B_FILTER 20
#define HANDLE_ALERTS (1)
#define HANDLE_MOTOR_FAULTS (1)

int accelerationLimit = 100000; // pulses per sec^2

#define inputPin1 IO3  // Belt Start trigger
#define inputPin2 IO2  // Belt Reset trigger

#define relay0Pin IO0 // Irrigation output
#define relay1Pin IO1 // Misting output

byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};  // MAC address
IPAddress ip(192, 168, 10, 2);                      // Static IP
IPAddress serverIp(192, 168, 10, 1);                // Server IP
#define PORT_NUM 8888
#define MAX_PACKET_LENGTH 200
unsigned char packetReceived[MAX_PACKET_LENGTH];
EthernetClient client;

volatile bool ready_to_run_flag = false;   // =0 at power-up / reset
bool sequenceActive = false;
unsigned long startTime = 0;
/////////////////////////////////////////////////////////////////////////////
///////// User Sequence Modification Values and Motor Speeds ////////////////
/////////////////////////////////////////////////////////////////////////////

float irrigation_start_time;
float irrigation_end_time;
float roller_start_time;
float roller_end_time;
float misting_start_time;
float misting_end_time;

float user_irrigation_start_mod_value = 0;
float user_roller_start_mod_value = 0;
float user_misting_start_mod_value = 0;
float user_irrigation_end_mod_value = 0;
float user_roller_end_mod_value = 0;
float user_misting_end_mod_value = 0;
float user_belt_rpm = 200;
float user_hopper_rpm = 100;



////////////////////////////////////////////////////////////
////////////// Define sequence geometry ////////////////////
////////////////////////////////////////////////////////////

float tray_length = 0.5302; // meters
float turn_off_manual_adjustment = 0.06;

float distance_irrigation_start = 0.635-tray_length;
float distance_roller_start = 0.8382 - tray_length - 0.06; // 0.196
float distance_misting_start = 0.9398 - tray_length; //0.3302

float distance_irrigation_end = distance_irrigation_start + tray_length - 0.1;
float distance_roller_end = distance_roller_start + tray_length + 0.01;
float distance_misting_end = distance_misting_start + tray_length - 0.06;



void printSequenceTimes(float irrigation_start, float roller_start, float misting_start, float irrigation_end,  float roller_end,  float misting_end) {
    Serial.println("Time Sequence Debug Values:");
    Serial.print("Irrigation Start: "); Serial.println(irrigation_start);
    Serial.print("Roller Start: "); Serial.println(roller_start);
    Serial.print("Misting Start: "); Serial.println(misting_start);
    Serial.print("Irrigation End: "); Serial.println(irrigation_end);
    Serial.print("Roller End: "); Serial.println(roller_end);
    Serial.print("Misting End: "); Serial.println(misting_end);
}

bool BeltMoveVelocity(int velocity) {
    velocity = -abs(velocity);
    if (BeltMotor.StatusReg().bit.AlertsPresent) {
        Serial.println("Motor alert detected.");    
        PrintAlerts();
        if(HANDLE_ALERTS){
            HandleAlerts();
        } else {
            Serial.println("Enable automatic alert handling by setting HANDLE_ALERTS to 1.");
        }
        Serial.println("Move canceled.");   
        return false;
    }
    BeltMotor.MoveVelocity(velocity);
    while (!BeltMotor.StatusReg().bit.AtTargetVelocity) {
        continue;
    }
    return true; 
}

bool HopperMoveVelocity(int velocity) {
    velocity = abs(velocity);
    if (HopperMotor.StatusReg().bit.AlertsPresent) {
        Serial.println("Motor alert detected.");    
        PrintAlerts();
        if(HANDLE_ALERTS){
            HandleAlerts();
        } else {
            Serial.println("Enable automatic alert handling by setting HANDLE_ALERTS to 1.");
        }
        Serial.println("Move canceled.");   
        return false;
    }
    HopperMotor.MoveVelocity(3*velocity);
    while (!HopperMotor.StatusReg().bit.AtTargetVelocity) {
        continue;
    }
    return true; 
}

// void parseReceivedMessage(char *message) {
//     // Helper function to extract float values
//     auto extractValue = [](char *message, const char *key) -> float {
//         char *ptr = strstr(message, key);
//         if (ptr) {
//             return atof(ptr + strlen(key) + 2); // Skip past "Key: "
//         }
//         return 0.0;
//     };

//     auto extractBool = [](char *msg, const char *key) -> bool {
//         char *ptr = strstr(msg, key);
//         if (ptr) {
//             return atoi(ptr + strlen(key) + 2) != 0;   // treat 0/1 as false/true
//         }
//         return false;
//     };

//     ready_to_run_flag = extractBool(message, "Ready to Run");

//     user_irrigation_start_mod_value = extractValue(message, "Irrigation Delay");
//     user_roller_start_mod_value = extractValue(message, "Roller Delay");
//     user_misting_start_mod_value = extractValue(message, "Misting Delay");

//     user_irrigation_end_mod_value = extractValue(message, "Irrigation Duration");
//     user_roller_end_mod_value = extractValue(message, "Roller Duration");
//     user_misting_end_mod_value = extractValue(message, "Misting Duration");

//     user_belt_rpm = extractValue(message, "Belt Speed");
//     user_hopper_rpm = extractValue(message, "Roller Speed");

//     // at speed 10, I measured that the belt moves at 0.079 meters per second (1.19 meter in 15.06 sec)
//     // at speed 15, I measured that the belt moves at 0.132 meters per second (1.19 meter in 9.03 sec)
//     // at speed 20, I measured that the belt moves at 0.174 meters per second (1.19 meter in 6.83 sec)


//     // Rescale User Values
//     user_belt_rpm *= 500;
//     user_hopper_rpm *= 10;
//     user_irrigation_start_mod_value /= 1;
//     user_roller_start_mod_value /= 1;
//     user_misting_start_mod_value /= 1;
//     user_irrigation_end_mod_value /= 1;
//     user_roller_end_mod_value /= 1;
//     user_misting_end_mod_value /= 1;

//     // // Print the parsed values
//     // Serial.println("Parsed Data Values:");
//     // Serial.print("Irrigation Delay: "); Serial.println(user_irrigation_start_mod_value);
//     // Serial.print("Roller Delay: "); Serial.println(user_roller_start_mod_value);
//     // Serial.print("Misting Delay: "); Serial.println(user_misting_start_mod_value);
//     // Serial.print("Irrigation Duration: "); Serial.println(user_irrigation_end_mod_value);
//     // Serial.print("Roller Duration: "); Serial.println(user_roller_end_mod_value);
//     // Serial.print("Misting Duration: "); Serial.println(user_misting_end_mod_value);
//     // Serial.print("Belt Speed: "); Serial.println(user_belt_rpm);
//     // Serial.print("Roller Speed: "); Serial.println(user_hopper_rpm);
// }

void parseReceivedMessage(char *message) {
    // Expected CSV format:
    // ready_to_run,active_variety,roller_speed,belt_speed,
    // irrigation_delay,irrigation_duration,
    // misting_delay,misting_duration,
    // roller_delay,roller_duration

    int fieldIndex = 0;
    char *token = strtok(message, ",");

    // Temporary locals to hold parsed values
    int ready_to_run_int = 0;
    float belt_speed_val = 0;
    float roller_speed_val = 0;
    float irrigation_delay_val = 0;
    float irrigation_duration_val = 0;
    float misting_delay_val = 0;
    float misting_duration_val = 0;
    float roller_delay_val = 0;
    float roller_duration_val = 0;

    while (token != NULL && fieldIndex < 10) {
        switch (fieldIndex) {
            case 0: // ready_to_run
                ready_to_run_int = atoi(token);
                break;
            case 1: // active_variety (we can ignore for now or store if needed)
                // int active_variety = atoi(token);
                break;
            case 2: // roller_speed
                roller_speed_val = atof(token);
                break;
            case 3: // belt_speed
                belt_speed_val = atof(token);
                break;
            case 4: // irrigation_delay
                irrigation_delay_val = atof(token);
                break;
            case 5: // irrigation_duration
                irrigation_duration_val = atof(token);
                break;
            case 6: // misting_delay
                misting_delay_val = atof(token);
                break;
            case 7: // misting_duration
                misting_duration_val = atof(token);
                break;
            case 8: // roller_delay
                roller_delay_val = atof(token);
                break;
            case 9: // roller_duration
                roller_duration_val = atof(token);
                break;
        }

        fieldIndex++;
        token = strtok(NULL, ",");
    }

    // Map parsed values to your global variables

    ready_to_run_flag = (ready_to_run_int != 0);

    // speeds
    user_belt_rpm   = belt_speed_val;   // belt speed
    user_hopper_rpm = roller_speed_val; // roller speed

    // delays
    user_irrigation_start_mod_value = irrigation_delay_val;
    user_roller_start_mod_value     = roller_delay_val;
    user_misting_start_mod_value    = misting_delay_val;

    // durations
    user_irrigation_end_mod_value = irrigation_duration_val;
    user_roller_end_mod_value     = roller_duration_val;
    user_misting_end_mod_value    = misting_duration_val;

    // Rescale User Values (same logic you already had)
    user_belt_rpm *= 500;
    user_hopper_rpm *= 10;
    user_irrigation_start_mod_value /= 1;
    user_roller_start_mod_value     /= 1;
    user_misting_start_mod_value    /= 1;
    user_irrigation_end_mod_value   /= 1;
    user_roller_end_mod_value       /= 1;
    user_misting_end_mod_value      /= 1;

    // Debug prints if you want them:
    
    Serial.println("Parsed CSV Data:");
    Serial.print("ready_to_run_flag: "); Serial.println(ready_to_run_flag);
    Serial.print("user_belt_rpm: "); Serial.println(user_belt_rpm);
    Serial.print("user_hopper_rpm: "); Serial.println(user_hopper_rpm);
    Serial.print("Irrig delay: "); Serial.println(user_irrigation_start_mod_value);
    Serial.print("Roller delay: "); Serial.println(user_roller_start_mod_value);
    Serial.print("Misting delay: "); Serial.println(user_misting_start_mod_value);
    Serial.print("Irrig dur: "); Serial.println(user_irrigation_end_mod_value);
    Serial.print("Roller dur: "); Serial.println(user_roller_end_mod_value);
    Serial.print("Misting dur: "); Serial.println(user_misting_end_mod_value);
    
}

void PrintAlerts() {
    Serial.println("Alerts present: ");
    if(BeltMotor.AlertReg().bit.MotionCanceledInAlert){
        Serial.println("    BeltMotor: MotionCanceledInAlert "); }
    if(BeltMotor.AlertReg().bit.MotionCanceledPositiveLimit){
        Serial.println("    BeltMotor: MotionCanceledPositiveLimit "); }
    if(BeltMotor.AlertReg().bit.MotionCanceledNegativeLimit){
        Serial.println("    BeltMotor: MotionCanceledNegativeLimit "); }
    if(BeltMotor.AlertReg().bit.MotionCanceledSensorEStop){
        Serial.println("    BeltMotor: MotionCanceledSensorEStop "); }
    if(BeltMotor.AlertReg().bit.MotionCanceledMotorDisabled){
        Serial.println("    BeltMotor: MotionCanceledMotorDisabled "); }
    if(BeltMotor.AlertReg().bit.MotorFaulted){
        Serial.println("    BeltMotor: MotorFaulted "); }
    if(HopperMotor.AlertReg().bit.MotionCanceledInAlert){
        Serial.println("    HopperMotor: MotionCanceledInAlert "); }
    if(HopperMotor.AlertReg().bit.MotionCanceledPositiveLimit){
        Serial.println("    HopperMotor: MotionCanceledPositiveLimit "); }
    if(HopperMotor.AlertReg().bit.MotionCanceledNegativeLimit){
        Serial.println("    HopperMotor: MotionCanceledNegativeLimit "); }
    if(HopperMotor.AlertReg().bit.MotionCanceledSensorEStop){
        Serial.println("    HopperMotor: MotionCanceledSensorEStop "); }
    if(HopperMotor.AlertReg().bit.MotionCanceledMotorDisabled){
        Serial.println("    HopperMotor: MotionCanceledMotorDisabled "); }
    if(HopperMotor.AlertReg().bit.MotorFaulted){
        Serial.println("    HopperMotor: MotorFaulted "); }
}

void HandleAlerts() {
    if(BeltMotor.AlertReg().bit.MotorFaulted){
        Serial.println("BeltMotor faults detected. Resetting...");
        BeltMotor.EnableRequest(false);
        delay(10);
        BeltMotor.EnableRequest(true);
    }
    if(HopperMotor.AlertReg().bit.MotorFaulted){
        Serial.println("HopperMotor faults detected. Resetting...");
        HopperMotor.EnableRequest(false);
        delay(10);
        HopperMotor.EnableRequest(true);
    }
    Serial.println("Clearing alerts.");
    BeltMotor.ClearAlerts();
    HopperMotor.ClearAlerts();
}

void setup() {
    pinMode(relay0Pin, OUTPUT);
    pinMode(relay1Pin, OUTPUT);
    pinMode(inputPin1, INPUT);
    pinMode(inputPin2, INPUT);


    Serial.begin(9600);

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

  /////////////////////////////////////////////////////////
  /////////////        Motor Set Up           ///////////// 
  /////////////////////////////////////////////////////////
  MotorMgr.MotorModeSet(MotorManager::MOTOR_ALL, Connector::CPM_MODE_STEP_AND_DIR);
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

  // Hopper Motor Setup
  HopperMotor.HlfbMode(MotorDriver::HLFB_MODE_HAS_BIPOLAR_PWM);
  HopperMotor.HlfbCarrier(MotorDriver::HLFB_CARRIER_482_HZ);
  HopperMotor.AccelMax(accelerationLimit);
  HopperMotor.EnableRequest(true);
  Serial.println("HopperMotor Enabled");

  startTime = millis();
  while (HopperMotor.HlfbState() != MotorDriver::HLFB_ASSERTED &&
          !HopperMotor.StatusReg().bit.AlertsPresent &&
          millis() - startTime < 5000) {
      continue;
  }
  if (HopperMotor.StatusReg().bit.AlertsPresent) {
      if (HANDLE_ALERTS) HandleAlerts();
  } else {
      Serial.println("HopperMotor Ready");
  }
}

void loop() {
  // Read input state
  if (client.available() > 0) {
      int len = client.read(packetReceived, MAX_PACKET_LENGTH - 1);
      packetReceived[len] = '\0';

      // Parse values from the received message
      parseReceivedMessage((char *)packetReceived);
  } else {
      Serial.println("Server disconnected. Reconnecting...");
      client.connect(serverIp, PORT_NUM);
      delay(500);
  }

  float belt_speed = 0.01;

  ////////////////////////////////////////////////////////////
  //////////////// Calculate Motor Speed //////////////////////
  ////////////////////////////////////////////////////////////

  // float motor_rps = 200*user_belt_rpm / 60.0; // revolutions per second
  float motor_rps = user_belt_rpm/60 ; // revolutions per second

  float motor_rps_radians = motor_rps; // radians per second
  float pulley_radius = 0.0102; // meters
  float gear_ratio = 0.1; // 10:1 gearbox
  belt_speed = motor_rps_radians * gear_ratio * pulley_radius; // meters per second
  if (belt_speed < 0.01) {   // or user_belt_rpm == 0
    belt_speed = 0.01;     // tiny number to avoid NaN
  }
  Serial.println(belt_speed, 5);



  ////////////////////////////////////////////////////////////
  ////////// Calculate Default Sequence Times ////////////////
  ////////////////////////////////////////////////////////////

  float irrigation_start_time = (distance_irrigation_start / belt_speed) * 1000 + user_irrigation_start_mod_value*100;
  float roller_start_time = (distance_roller_start / belt_speed) * 1000 + user_roller_start_mod_value*100;
  float misting_start_time = (distance_misting_start / belt_speed) * 1000 + user_misting_start_mod_value*100 -  1*motor_rps;

  float irrigation_end_time = (distance_irrigation_end / belt_speed) * 1000 + user_irrigation_end_mod_value*100 - 5*motor_rps ;
  float roller_end_time = (distance_roller_end / belt_speed) * 1000 + user_roller_end_mod_value*100 - 2.6*motor_rps;
  float misting_end_time = (distance_misting_end / belt_speed) * 1000 + user_misting_end_mod_value*100 - motor_rps;

  ////////////////////////////////////////////////////////////
  /////////////// Sequence Execution Logic ///////////////////
  ////////////////////////////////////////////////////////////

  bool inputState = digitalRead(inputPin1);

  if (inputState && !sequenceActive && ready_to_run_flag) {
      // If DI-6 is triggered and sequence is not already running, start stopwatch
      startTime = millis();
      sequenceActive = true;
      Serial.println("DI-6 triggered: Starting event sequence.");

      // Run belt motor immediately
      BeltMoveVelocity(user_belt_rpm);  // Example values: 100 RPM
  }

  if (sequenceActive) {
      unsigned long elapsedTime = millis() - startTime;
      unsigned long irrigationStartMs = irrigation_start_time;
      unsigned long irrigationEndMs = irrigation_end_time;
      unsigned long rollerStartMs = roller_start_time;
      unsigned long rollerEndMs = roller_end_time;
      unsigned long mistingStartMs = misting_start_time;
      unsigned long mistingEndMs = misting_end_time;

      // printSequenceTimes(irrigationStartMs, rollerStartMs, mistingStartMs, irrigationEndMs, rollerEndMs, mistingEndMs);

      if (elapsedTime >= irrigationStartMs && elapsedTime < irrigationEndMs) {
          digitalWrite(relay0Pin, HIGH);
          // Serial.println("Irrigation ON");
      } else if (elapsedTime >= irrigationEndMs) {
          digitalWrite(relay0Pin, LOW);
          // Serial.println("Irrigation OFF");
      }

      if (elapsedTime >= rollerStartMs && elapsedTime < rollerEndMs) {
          // Serial.println("Roller ON (Function Call Would Happen Here)");
          HopperMoveVelocity(user_hopper_rpm);  // Example values: 100 RPM

      } else if (elapsedTime >= rollerEndMs) {
          // Serial.println("Roller OFF");
          HopperMoveVelocity(0);  // Example values: 100 RPM

      }

      if (elapsedTime >= mistingStartMs && elapsedTime < mistingEndMs) {
          digitalWrite(relay1Pin, HIGH);
          // Serial.println("Misting ON");
      } else if (elapsedTime >= mistingEndMs) {
          digitalWrite(relay1Pin, LOW);
          // Serial.println("Misting OFF");
      }

      if (elapsedTime >= irrigationEndMs && elapsedTime >= rollerEndMs && elapsedTime >= mistingEndMs) {
          Serial.println("Waiting for DI-7 trigger to stop belt...");
          BeltMoveVelocity(14000);

          while (!digitalRead(inputPin2)) {

              delay(10);
          }
          // Serial.println(int(75/belt_speed));
          delay(500);
          BeltMoveVelocity(0);
          sequenceActive = false;
          // Serial.println("Sequence completed. Waiting for next DI-6 trigger.");
      }
  }
  delay(10);  
}


