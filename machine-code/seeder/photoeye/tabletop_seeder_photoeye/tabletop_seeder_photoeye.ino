#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include "ClearCore.h"

#define BeltMotor ConnectorM0
#define HopperMotor ConnectorM2
#define HANDLE_ALERTS (1)

int accelerationLimit = 100000; // pulses per sec^2

#define inputPin1 IO3  // Tray photoeye

#define relay0Pin IO0 // Irrigation output
#define relay1Pin IO1 // Misting output

byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};  // MAC address
IPAddress ip(192, 168, 10, 2);                      // Static IP
IPAddress serverIp(192, 168, 10, 1);                // Server IP
#define PORT_NUM 8888
#define MAX_PACKET_LENGTH 200
unsigned char packetReceived[MAX_PACKET_LENGTH];
EthernetClient client;

bool ready_to_run_flag = false;   // =0 at power-up / reset
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

// Variety identity — id from CSV field 1, name from CSV field 10 (last).
// Name is bounded to 32 chars + null terminator; longer names are truncated.
int  activeVarietyId = -1;
char activeVarietyName[33] = "";

// ---- Debug logging ----
// When true, verbose per-cycle / per-packet diagnostic prints are emitted.
// Error and important state messages are always printed regardless.
// NOTE: do NOT name this `DEBUG` — the ClearCore SAM toolchain defines
// `DEBUG` as a numeric macro on the compiler command line, which would expand
// here and break the build ("expected unqualified-id before numeric constant").
const bool DEBUG_LOG = true;
#define DBG_PRINT(x)   do { if (DEBUG_LOG) Serial.print(x); } while (0)
#define DBG_PRINTLN(x) do { if (DEBUG_LOG) Serial.println(x); } while (0)

// ---- UDP telemetry ----
// Telemetry uses a SEPARATE UDP socket from the TCP control channel to avoid
// backpressure on inbound CSV commands. Fire-and-forget — never block on send.
EthernetUDP        Udp;
const uint16_t     UDP_LOCAL_PORT       = 9998;
unsigned int       remotePort           = 9999;
const uint16_t     STATUS_INTERVAL_MS   = 1000;

struct TelemetryState {
    uint32_t bootId;
    uint32_t seq;
    uint32_t lastStatusMs;
    uint32_t lastRxCmdMs;
    uint32_t faultCountBelt;
    uint32_t faultCountHopper;
    uint32_t udpSendFailCount;
    bool     lastBeltFault;
    bool     lastHopperFault;
    uint32_t beltMotorUptimeMs;   // millis() when belt started moving (0 = idle)
    uint32_t hopperMotorUptimeMs; // millis() when hopper started moving (0 = idle)
    uint32_t traysProcessed;       // number of completed sequences since boot
};
TelemetryState t;



////////////////////////////////////////////////////////////
////////////// Define sequence geometry ////////////////////
////////////////////////////////////////////////////////////

float tray_length = 0.5302; // meters

float distance_irrigation_start = 0.635-tray_length;
float distance_roller_start = 0.8382 - tray_length - 0.06; // 0.196
float distance_misting_start = 0.9398 - tray_length; //0.3302

float distance_irrigation_end = distance_irrigation_start + tray_length - 0.1;
float distance_roller_end = distance_roller_start + tray_length + 0.01;
float distance_misting_end = distance_misting_start + tray_length - 0.06;



void printSequenceTimes(float irrigation_start, float roller_start, float misting_start, float irrigation_end,  float roller_end,  float misting_end) {
    DBG_PRINTLN("Time Sequence Debug Values:");
    DBG_PRINT("Irrigation Start: "); DBG_PRINTLN(irrigation_start);
    DBG_PRINT("Roller Start: ");     DBG_PRINTLN(roller_start);
    DBG_PRINT("Misting Start: ");    DBG_PRINTLN(misting_start);
    DBG_PRINT("Irrigation End: ");   DBG_PRINTLN(irrigation_end);
    DBG_PRINT("Roller End: ");       DBG_PRINTLN(roller_end);
    DBG_PRINT("Misting End: ");      DBG_PRINTLN(misting_end);
}

bool BeltMoveVelocity(int velocity) {
    velocity = -abs(velocity);
    if (BeltMotor.StatusReg().bit.AlertsPresent) {
        Serial.println("Motor alert detected.");
        PrintAlerts();
        SendEvent("FAULT_BELT", "belt");
        t.faultCountBelt++;
        if(HANDLE_ALERTS){
            HandleAlerts();
        } else {
            Serial.println("Enable automatic alert handling by setting HANDLE_ALERTS to 1.");
        }
        Serial.println("Move canceled.");
        return false;
    }
    BeltMotor.MoveVelocity(velocity);
    // Track motor-running uptime for telemetry: 0 commanded → idle.
    if (velocity == 0) {
        t.beltMotorUptimeMs = 0;
    } else if (t.beltMotorUptimeMs == 0) {
        t.beltMotorUptimeMs = millis();
    }
    while (!BeltMotor.StatusReg().bit.AtTargetVelocity) {
        continue;
    }
    return true;
}

// Empirical scale: roller mechanism needs ~3x the commanded velocity to
// produce the desired seed-roller surface speed (tuned on bench).
static const int HOPPER_VELOCITY_GAIN = 3;

bool HopperMoveVelocity(int velocity) {
    velocity = abs(velocity);
    if (HopperMotor.StatusReg().bit.AlertsPresent) {
        Serial.println("Motor alert detected.");
        PrintAlerts();
        SendEvent("FAULT_ROLLER", "roller");
        t.faultCountHopper++;
        if(HANDLE_ALERTS){
            HandleAlerts();
        } else {
            Serial.println("Enable automatic alert handling by setting HANDLE_ALERTS to 1.");
        }
        Serial.println("Move canceled.");
        return false;
    }
    // Non-blocking: command the velocity and return. Busy-waiting on
    // AtTargetVelocity here would stall loop() and skew sequence timing,
    // especially when ramping down to 0 at roller-end.
    HopperMotor.MoveVelocity(HOPPER_VELOCITY_GAIN * velocity);
    if (velocity == 0) {
        t.hopperMotorUptimeMs = 0;
    } else if (t.hopperMotorUptimeMs == 0) {
        t.hopperMotorUptimeMs = millis();
    }
    return true;
}

void parseReceivedMessage(char *message) {
    // Expected CSV format (11 fields):
    // ready_to_run,active_variety,roller_speed,belt_speed,
    // irrigation_delay,irrigation_duration,
    // misting_delay,misting_duration,
    // roller_delay,roller_duration,variety_name

    t.lastRxCmdMs = millis(); // for telemetry cmdAgeMs

    int fieldIndex = 0;
    char *token = strtok(message, ",");

    // Temporary locals to hold parsed values
    int ready_to_run_int = 0;
    int active_variety_int = 0;
    float belt_speed_val = 0;
    float roller_speed_val = 0;
    float irrigation_delay_val = 0;
    float irrigation_duration_val = 0;
    float misting_delay_val = 0;
    float misting_duration_val = 0;
    float roller_delay_val = 0;
    float roller_duration_val = 0;
    char variety_name_buf[33] = "";

    while (token != NULL && fieldIndex < 11) {
        switch (fieldIndex) {
            case 0: // ready_to_run
                ready_to_run_int = atoi(token);
                break;
            case 1: // active_variety (variety id)
                active_variety_int = atoi(token);
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
            case 10: // variety_name (last field; bounded copy)
                strncpy(variety_name_buf, token, sizeof(variety_name_buf) - 1);
                variety_name_buf[sizeof(variety_name_buf) - 1] = '\0';
                // Defensive: scrub anything that would break our CSV/UDP framing.
                // Pi side should already sanitize, but cheap insurance.
                for (char *p = variety_name_buf; *p; ++p) {
                    if (*p == ',' || *p == '\n' || *p == '\r') *p = '_';
                }
                break;
        }

        fieldIndex++;
        token = strtok(NULL, ",");
    }

    // Map parsed values to your global variables

    ready_to_run_flag = (ready_to_run_int != 0);

    // variety identity
    activeVarietyId = active_variety_int;
    strncpy(activeVarietyName, variety_name_buf, sizeof(activeVarietyName) - 1);
    activeVarietyName[sizeof(activeVarietyName) - 1] = '\0';

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
    
    DBG_PRINTLN("Parsed CSV Data:");
    DBG_PRINT("ready_to_run_flag: "); DBG_PRINTLN(ready_to_run_flag);
    DBG_PRINT("activeVarietyId: ");   DBG_PRINTLN(activeVarietyId);
    DBG_PRINT("activeVarietyName: "); DBG_PRINTLN(activeVarietyName);
    DBG_PRINT("user_belt_rpm: ");     DBG_PRINTLN(user_belt_rpm);
    DBG_PRINT("user_hopper_rpm: ");   DBG_PRINTLN(user_hopper_rpm);
    DBG_PRINT("Irrig delay: ");       DBG_PRINTLN(user_irrigation_start_mod_value);
    DBG_PRINT("Roller delay: ");      DBG_PRINTLN(user_roller_start_mod_value);
    DBG_PRINT("Misting delay: ");     DBG_PRINTLN(user_misting_start_mod_value);
    DBG_PRINT("Irrig dur: ");         DBG_PRINTLN(user_irrigation_end_mod_value);
    DBG_PRINT("Roller dur: ");        DBG_PRINTLN(user_roller_end_mod_value);
    DBG_PRINT("Misting dur: ");       DBG_PRINTLN(user_misting_end_mod_value);
    
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

//------------------------------------------------------------------------------
// Telemetry — UDP fire-and-forget. NEVER block on send. Runs on a separate
// UDP socket from the TCP control channel to avoid TCP backpressure stalling
// inbound command reads.
//------------------------------------------------------------------------------

void SendStatusUpdate() {
    if (Ethernet.linkStatus() != LinkON) {
        return;
    }
    char telemetryBuffer[256];
    uint32_t uptimeMs = millis();
    uint32_t cmdAgeMs = uptimeMs - t.lastRxCmdMs;
    uint32_t seq = ++t.seq;

    uint32_t beltUptime   = t.beltMotorUptimeMs   ? (uptimeMs - t.beltMotorUptimeMs)   : 0;
    uint32_t hopperUptime = t.hopperMotorUptimeMs ? (uptimeMs - t.hopperMotorUptimeMs) : 0;

    // Format (schema_ver 2): STATUS_UPDATE,2,bootId,seq,uptimeMs,
    //   belt_motor_uptime_ms,roller_motor_uptime_ms,cmdAgeMs,udpFails,
    //   trays_processed,varietyId,varietyName
    // hopperUptime maps to roller_motor_uptime_ms (hopper = roller).
    // varietyName is LAST so any snprintf truncation chops the name, not
    // the structured numeric tail.
    snprintf(telemetryBuffer, sizeof(telemetryBuffer),
             "STATUS_UPDATE,2,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%d,%s",
             (unsigned long)t.bootId,
             (unsigned long)seq,
             (unsigned long)uptimeMs,
             (unsigned long)beltUptime,
             (unsigned long)hopperUptime,
             (unsigned long)cmdAgeMs,
             (unsigned long)t.udpSendFailCount,
             (unsigned long)t.traysProcessed,
             activeVarietyId,
             activeVarietyName);

    Udp.beginPacket(serverIp, remotePort);
    Udp.write((const uint8_t *)telemetryBuffer, strlen(telemetryBuffer));
    if (!Udp.endPacket()) {
        t.udpSendFailCount++;
    }
}

// Lightweight fault ping — emit once on the fault edge (0->1), not every
// cycle. Receiver joins with the nearest STATUS_UPDATE on (bootId, uptimeMs)
// for full state at event time. eventCode must start with "FAULT_"; motor
// attributes the fault ("belt" or "roller").
void SendEvent(const char *eventCode, const char *motor) {
    if (Ethernet.linkStatus() != LinkON) {
        return;
    }
    char telemetryBuffer[128];
    uint32_t uptimeMs = millis();
    uint32_t seq = ++t.seq;

    // Format (schema_ver 2): EVENT,2,bootId,seq,uptimeMs,eventCode,eventValue,motor
    snprintf(telemetryBuffer, sizeof(telemetryBuffer),
             "EVENT,2,%lu,%lu,%lu,%s,%d,%s",
             (unsigned long)t.bootId,
             (unsigned long)seq,
             (unsigned long)uptimeMs,
             eventCode,
             1,
             motor);

    Udp.beginPacket(serverIp, remotePort);
    Udp.write((const uint8_t *)telemetryBuffer, strlen(telemetryBuffer));
    if (!Udp.endPacket()) {
        t.udpSendFailCount++;
    }
}

void setup() {
    pinMode(relay0Pin, OUTPUT);
    pinMode(relay1Pin, OUTPUT);
    pinMode(inputPin1, INPUT);


    Serial.begin(9600);

    ////////////////////////////////////////////////////////
    /////////////////// Ethernet Connection ////////////////
    ////////////////////////////////////////////////////////

    Ethernet.begin(mac, ip); // Set static IP
    while (Ethernet.linkStatus() == LinkOFF) {
        Serial.println("Waiting for Ethernet link...");
        delay(1000);
    }

    Serial.println("[line-framed-v2] firmware boot");
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

    uint32_t enableStartTime = millis();
    while (BeltMotor.HlfbState() != MotorDriver::HLFB_ASSERTED &&
            !BeltMotor.StatusReg().bit.AlertsPresent &&
            millis() - enableStartTime < 5000) {
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

    enableStartTime = millis();
    while (HopperMotor.HlfbState() != MotorDriver::HLFB_ASSERTED &&
            !HopperMotor.StatusReg().bit.AlertsPresent &&
            millis() - enableStartTime < 5000) {
        continue;
    }
    if (HopperMotor.StatusReg().bit.AlertsPresent) {
        if (HANDLE_ALERTS) HandleAlerts();
    } else {
        Serial.println("HopperMotor Ready");
    }

    ////////////////////////////////////////////////////////
    /////////////// Telemetry Initialization ///////////////
    ////////////////////////////////////////////////////////
    t.bootId              = millis() ^ 0x5EED5EED;
    t.seq                 = 0;
    t.lastStatusMs        = 0;
    t.lastRxCmdMs         = millis();
    t.faultCountBelt      = 0;
    t.faultCountHopper    = 0;
    t.udpSendFailCount    = 0;
    t.lastBeltFault       = false;
    t.lastHopperFault     = false;
    t.beltMotorUptimeMs   = 0;
    t.hopperMotorUptimeMs = 0;
    t.traysProcessed       = 0;

    Udp.begin(UDP_LOCAL_PORT);
    Serial.println("UDP telemetry initialized");
}

void loop() {
  // Non-blocking reconnect: only attempt when the socket is actually down,
  // and rate-limit attempts so we don't stall the motion sequence.
  static unsigned long lastReconnectAttempt = 0;
  if (!client.connected()) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt >= 2000) {
          lastReconnectAttempt = now;
          Serial.println("Server disconnected. Attempting reconnect...");
          client.stop();
          client.connect(serverIp, PORT_NUM);
      }
  } else {
      // The Pi server holds the connection open and pushes a newline-terminated
      // CSV whenever state changes, plus a periodic heartbeat (~10s). Frame on
      // '\n' so multi-packet or split reads don't corrupt the CSV field split,
      // and skip-parse if the snapshot matches the last one we parsed — that
      // makes the heartbeat free and gives us defense-in-depth against any
      // future server that pushes redundant updates.
      static char   lineBuf[MAX_PACKET_LENGTH];
      static char   lastLine[MAX_PACKET_LENGTH];  // zero-init by static
      static size_t lineLen = 0;
      while (client.available() > 0) {
          int c = client.read();
          if (c < 0) break;
          if (c == '\r') continue;                       // tolerate CRLF
          if (c == '\n') {
              if (lineLen > 0) {
                  lineBuf[lineLen] = '\0';
                  if (strcmp(lineBuf, lastLine) != 0) {
                      // Cache BEFORE parse — parseReceivedMessage uses strtok
                      // which mutates lineBuf in place.
                      memcpy(lastLine, lineBuf, lineLen + 1);
                      parseReceivedMessage(lineBuf);
                  }
              }
              lineLen = 0;
          } else if (lineLen < sizeof(lineBuf) - 1) {
              lineBuf[lineLen++] = (char)c;
          } else {
              // Overflow without a delimiter — drop the partial line and
              // resync on the next '\n'. Should never happen with the current
              // ~80-byte payload.
              lineLen = 0;
          }
      }
  }

  ////////////////////////////////////////////////////////////
  //////////////// Belt Run Gate //////////////////////////////
  ////////////////////////////////////////////////////////////
  // Belt runs continuously while ready_to_run_flag is true. Only re-issue
  // the velocity command when something actually changed (flag flip or
  // live RPM update) so we don't spam the motor every tick.
  {
      static bool  lastBeltRunning   = false;
      static float lastCommandedRpm  = 0;
      bool wantBeltRunning = ready_to_run_flag && (user_belt_rpm > 0);

      if (wantBeltRunning) {
          if (!lastBeltRunning || user_belt_rpm != lastCommandedRpm) {
              BeltMoveVelocity(user_belt_rpm);
              lastCommandedRpm = user_belt_rpm;
              lastBeltRunning  = true;
          }
      } else if (lastBeltRunning) {
          BeltMoveVelocity(0);
          lastCommandedRpm = 0;
          lastBeltRunning  = false;
      }
  }

  ////////////////////////////////////////////////////////////
  //////////////// Calculate Motor Speed //////////////////////
  ////////////////////////////////////////////////////////////

  // NOTE: The belt_speed expression below is dimensionally wrong (missing a
  // 2*pi to convert rev/s to rad/s), so the resulting "belt_speed" is off
  // by ~6.28x from true m/s. We intentionally keep it as-is: every
  // distance_* constant and every "- N*motor_rps" fudge in the sequence
  // timings below was tuned empirically on the bench against THIS value.
  // Don't "fix" the math without re-tuning the entire sequence on the
  // physical machine.
  float motor_rps = user_belt_rpm / 60.0f; // revolutions per second
  float pulley_radius = 0.0102f; // meters
  float gear_ratio = 0.1f;       // 10:1 gearbox
  float belt_speed = motor_rps * gear_ratio * pulley_radius;

  ////////////////////////////////////////////////////////////
  ////////// Calculate Default Sequence Times ////////////////
  ////////////////////////////////////////////////////////////

  float irrigation_start_time = 0, roller_start_time = 0, misting_start_time = 0;
  float irrigation_end_time = 0, roller_end_time = 0, misting_end_time = 0;
  bool belt_speed_valid = (belt_speed > 0.01f);
  if (belt_speed_valid) {
      irrigation_start_time = (distance_irrigation_start / belt_speed) * 1000 + user_irrigation_start_mod_value*100;
      roller_start_time     = (distance_roller_start     / belt_speed) * 1000 + user_roller_start_mod_value*100;
      misting_start_time    = (distance_misting_start    / belt_speed) * 1000 + user_misting_start_mod_value*100 - 1*motor_rps;

      irrigation_end_time = (distance_irrigation_end / belt_speed) * 1000 + user_irrigation_end_mod_value*100 - 5*motor_rps;
      roller_end_time     = (distance_roller_end     / belt_speed) * 1000 + user_roller_end_mod_value*100     - 2.6*motor_rps;
      misting_end_time    = (distance_misting_end    / belt_speed) * 1000 + user_misting_end_mod_value*100    - motor_rps;
  }

  ////////////////////////////////////////////////////////////
  /////////////// Sequence Execution Logic ///////////////////
  ////////////////////////////////////////////////////////////

  bool inputState = digitalRead(inputPin1);
  static bool lastPhotoeyeState = false;

  // Rising-edge only: a tray that's still parked in front of the photoeye
  // when the previous sequence ends must not immediately re-fire — wait for
  // it to clear and a new tray to arrive. Refuse to start without a valid
  // belt speed (clamped/zero speed produces nonsense timings).
  if (inputState && !lastPhotoeyeState && !sequenceActive && ready_to_run_flag && belt_speed_valid) {
      startTime = millis();
      sequenceActive = true;
      DBG_PRINTLN("Photoeye rising edge: Starting event sequence.");
  }
  lastPhotoeyeState = inputState;

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
          sequenceActive = false;
          t.traysProcessed++;
      }
  }

  // Periodic telemetry — fire-and-forget, time-guarded so a slow socket
  // surfaces as a warning instead of silently skewing the motion loop.
  {
      unsigned long now = millis();
      if (now - t.lastStatusMs >= STATUS_INTERVAL_MS) {
          t.lastStatusMs = now;
          uint32_t udpStart = millis();
          SendStatusUpdate();
          uint32_t udpDuration = millis() - udpStart;
          if (udpDuration > 20) {
              Serial.print("WARN telemetry blocked for ");
              Serial.print(udpDuration);
              Serial.println("ms");
          }
      }
  }

  delay(10);
}


