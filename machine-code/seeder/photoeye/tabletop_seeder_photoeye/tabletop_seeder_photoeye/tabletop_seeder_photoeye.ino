#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include "ClearCore.h"

// No belt motor on this machine. The conveyor is driven by a separate VFD with
// 10 discrete speeds; the operator dials the VFD speed and reports it to us via
// the CSV belt_speed field (see SHUTOFF_MS_BY_SPEED below).
#define HopperMotor ConnectorM2
#define HANDLE_ALERTS (1)

int accelerationLimit = 100000; // pulses per sec^2

////////////////////////////////////////////////////////////////////////////
///////////////////////////// Pin Map ///////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
// Two laser gates (inputs) and three solenoids (outputs).
// Laser gates read HIGH when the beam is broken (object present = triggered).

#define laserGateMain    IO1  // Main sequence trigger
#define laserGateTopcoat IO0  // Topcoat trigger

#define irrigationPin    IO2  // Main-sequence irrigation solenoid
#define topcoatPin       IO3  // Topcoat irrigation solenoid (follows IO0)
#define mistingPin       IO4  // Main-sequence misting solenoid

byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};  // MAC address
IPAddress ip(192, 168, 10, 2);                      // Static IP
IPAddress serverIp(192, 168, 10, 1);                // Server IP
#define PORT_NUM 8888
#define MAX_PACKET_LENGTH 200
unsigned char packetReceived[MAX_PACKET_LENGTH];
EthernetClient client;

bool ready_to_run_flag = false;   // =0 at power-up / reset

/////////////////////////////////////////////////////////////////////////////
///////// User Sequence Modification Values and Motor Speeds ////////////////
/////////////////////////////////////////////////////////////////////////////

float user_hopper_rpm = 100;

// VFD conveyor speed setting (1..10) reported by the operator via the CSV
// belt_speed field. Selects the post-trigger shutoff delay below.
int user_vfd_speed = 1;

// Shutoff delay (ms) applied AFTER the main laser gate (IO1) clears, before the
// hopper/irrigation/misting turn off. Indexed by VFD speed 1..10. This
// compensates for the travel time between the laser gate and the dispense head:
// a slower belt means a longer coast, hence a longer delay.
//
// >>> TUNE THESE ON THE MACHINE — one row per VFD speed setting. <<<
// Values below are placeholders.
const unsigned long SHUTOFF_MS_BY_SPEED[10] = {
    2000, // speed 1  (slowest)
    1800, // speed 2
    1600, // speed 3
    1400, // speed 4
    1200, // speed 5
    1000, // speed 6
    900,  // speed 7
    800,  // speed 8
    700,  // speed 9
    600,  // speed 10 (fastest)
};

unsigned long currentShutoffDelayMs() {
    int idx = user_vfd_speed;
    if (idx < 1)  idx = 1;
    if (idx > 10) idx = 10;
    return SHUTOFF_MS_BY_SPEED[idx - 1];
}

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
    uint32_t faultCountHopper;
    uint32_t udpSendFailCount;
    bool     lastHopperFault;
    uint32_t hopperMotorUptimeMs; // cumulative roller/hopper run time since boot,
                                  // accrued from each actual on..off window
    uint32_t traysProcessed;       // number of completed sequences since boot
};
TelemetryState t;

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
    // Non-blocking: command the velocity and return.
    HopperMotor.MoveVelocity(HOPPER_VELOCITY_GAIN * velocity);
    return true;
}

void parseReceivedMessage(char *message) {
    // Expected CSV format (11 fields):
    // ready_to_run,active_variety,roller_speed,belt_speed,
    // irrigation_delay,irrigation_duration,
    // misting_delay,misting_duration,
    // roller_delay,roller_duration,variety_name
    //
    // NOTE: with the belt motor removed, the belt_speed field is repurposed to
    // carry the VFD conveyor speed setting (1..10), and the irrigation/misting/
    // roller delay & duration fields are no longer used by this firmware. We
    // still parse all 11 fields to stay aligned with the Pi's CSV framing.

    t.lastRxCmdMs = millis(); // for telemetry cmdAgeMs

    int fieldIndex = 0;
    char *token = strtok(message, ",");

    // Temporary locals to hold parsed values
    int ready_to_run_int = 0;
    int active_variety_int = 0;
    int vfd_speed_val = 1;
    float roller_speed_val = 0;
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
            case 3: // belt_speed -> repurposed as VFD speed setting (1..10)
                vfd_speed_val = atoi(token);
                break;
            case 4: // irrigation_delay  (unused)
            case 5: // irrigation_duration (unused)
            case 6: // misting_delay (unused)
            case 7: // misting_duration (unused)
            case 8: // roller_delay (unused)
            case 9: // roller_duration (unused)
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
    user_hopper_rpm = roller_speed_val; // roller speed
    user_vfd_speed  = vfd_speed_val;    // conveyor VFD speed setting (1..10)

    // Rescale hopper RPM (same logic you already had)
    user_hopper_rpm *= 10;

    // Debug prints if you want them:
    DBG_PRINTLN("Parsed CSV Data:");
    DBG_PRINT("ready_to_run_flag: "); DBG_PRINTLN(ready_to_run_flag);
    DBG_PRINT("activeVarietyId: ");   DBG_PRINTLN(activeVarietyId);
    DBG_PRINT("activeVarietyName: "); DBG_PRINTLN(activeVarietyName);
    DBG_PRINT("user_hopper_rpm: ");   DBG_PRINTLN(user_hopper_rpm);
    DBG_PRINT("user_vfd_speed: ");    DBG_PRINTLN(user_vfd_speed);
    DBG_PRINT("shutoff_delay_ms: ");  DBG_PRINTLN(currentShutoffDelayMs());
}

void PrintAlerts() {
    Serial.println("Alerts present: ");
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
    if(HopperMotor.AlertReg().bit.MotorFaulted){
        Serial.println("HopperMotor faults detected. Resetting...");
        HopperMotor.EnableRequest(false);
        delay(10);
        HopperMotor.EnableRequest(true);
    }
    Serial.println("Clearing alerts.");
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

    uint32_t hopperUptime = t.hopperMotorUptimeMs; // cumulative hopper run time

    // Format (schema_ver 3): STATUS_UPDATE,3,bootId,seq,uptimeMs,
    //   roller_motor_uptime_ms,cmdAgeMs,udpFails,
    //   trays_processed,varietyId,varietyName
    // NOTE: schema bumped 2 -> 3 because the belt_motor_uptime_ms field was
    // removed (no belt motor). The Pi-side telemetry parser must be updated to
    // match this layout.
    // varietyName is LAST so any snprintf truncation chops the name, not
    // the structured numeric tail.
    snprintf(telemetryBuffer, sizeof(telemetryBuffer),
             "STATUS_UPDATE,3,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%d,%s",
             (unsigned long)t.bootId,
             (unsigned long)seq,
             (unsigned long)uptimeMs,
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
// attributes the fault ("roller").
void SendEvent(const char *eventCode, const char *motor) {
    if (Ethernet.linkStatus() != LinkON) {
        return;
    }
    char telemetryBuffer[128];
    uint32_t uptimeMs = millis();
    uint32_t seq = ++t.seq;

    // Format (schema_ver 3): EVENT,3,bootId,seq,uptimeMs,eventCode,eventValue,motor
    snprintf(telemetryBuffer, sizeof(telemetryBuffer),
             "EVENT,3,%lu,%lu,%lu,%s,%d,%s",
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
    // Laser gates: inputs (HIGH = beam broken / triggered).
    pinMode(laserGateMain,    INPUT);
    pinMode(laserGateTopcoat, INPUT);

    // Solenoids: outputs, start de-energized.
    pinMode(irrigationPin, OUTPUT);
    pinMode(topcoatPin,    OUTPUT);
    pinMode(mistingPin,    OUTPUT);
    digitalWrite(irrigationPin, LOW);
    digitalWrite(topcoatPin,    LOW);
    digitalWrite(mistingPin,    LOW);

    Serial.begin(9600);

    ////////////////////////////////////////////////////////
    /////////////////// Ethernet Connection ////////////////
    ////////////////////////////////////////////////////////

    Ethernet.begin(mac, ip); // Set static IP
    while (Ethernet.linkStatus() == LinkOFF) {
        Serial.println("Waiting for Ethernet link...");
        delay(1000);
    }

    /////////////////////////////////////////////////////////
    /////////////        Motor Set Up           /////////////
    /////////////////////////////////////////////////////////
    MotorMgr.MotorModeSet(MotorManager::MOTOR_ALL, Connector::CPM_MODE_STEP_AND_DIR);

    // Hopper Motor Setup
    HopperMotor.HlfbMode(MotorDriver::HLFB_MODE_HAS_BIPOLAR_PWM);
    HopperMotor.HlfbCarrier(MotorDriver::HLFB_CARRIER_482_HZ);
    HopperMotor.AccelMax(accelerationLimit);
    HopperMotor.EnableRequest(true);
    Serial.println("HopperMotor Enabled");

    uint32_t enableStartTime = millis();
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
    t.faultCountHopper    = 0;
    t.udpSendFailCount    = 0;
    t.lastHopperFault     = false;
    t.hopperMotorUptimeMs = 0;
    t.traysProcessed      = 0;

    Udp.begin(UDP_LOCAL_PORT);
    Serial.println("UDP telemetry initialized");
}

void loop() {
  // Non-blocking reconnect: only attempt when the socket is actually down,
  // and rate-limit attempts so we don't stall the control loop.
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
  ////////////////// Read Laser Gates /////////////////////////
  ////////////////////////////////////////////////////////////
  // HIGH = beam broken = triggered.
  bool mainGate    = digitalRead(laserGateMain);
  bool topcoatGate = digitalRead(laserGateTopcoat);

  ////////////////////////////////////////////////////////////
  /////////// Topcoat Irrigation (IO0 -> IO3) /////////////////
  ////////////////////////////////////////////////////////////
  // Pure level-follow, no delays: solenoid mirrors the gate. Gated by
  // ready_to_run_flag so nothing energizes when the machine isn't armed.
  digitalWrite(topcoatPin, (ready_to_run_flag && topcoatGate) ? HIGH : LOW);

  ////////////////////////////////////////////////////////////
  ///////// Main Sequence (IO1 -> hopper + IO2 + IO4) /////////
  ////////////////////////////////////////////////////////////
  // While the main gate is triggered, the hopper/irrigation/misting run. When
  // the gate clears, a shutoff timer (selected by VFD speed) keeps them running
  // for the coast-down before turning everything off together.
  {
      static bool          mainOutputsOn   = false;
      static bool          shutoffPending  = false;
      static unsigned long shutoffStartMs  = 0;
      static uint32_t      hopperOnStartMs = 0;

      // Force everything off if the machine gets disarmed mid-run.
      if (!ready_to_run_flag) {
          if (mainOutputsOn) {
              digitalWrite(irrigationPin, LOW);
              digitalWrite(mistingPin,    LOW);
              HopperMoveVelocity(0);
              if (hopperOnStartMs) {
                  t.hopperMotorUptimeMs += millis() - hopperOnStartMs;
                  hopperOnStartMs = 0;
              }
              mainOutputsOn = false;
          }
          shutoffPending = false;
      } else {
          if (mainGate) {
              // Triggered: ensure outputs are on, cancel any pending shutoff.
              shutoffPending = false;
              if (!mainOutputsOn) {
                  digitalWrite(irrigationPin, HIGH);
                  digitalWrite(mistingPin,    HIGH);
                  HopperMoveVelocity(user_hopper_rpm);
                  hopperOnStartMs = millis();
                  mainOutputsOn = true;
                  DBG_PRINTLN("Main gate triggered: sequence ON.");
              }
          } else {
              // Gate clear. If outputs are on and we haven't already started the
              // shutoff countdown, start it now.
              if (mainOutputsOn && !shutoffPending) {
                  shutoffPending = true;
                  shutoffStartMs = millis();
                  DBG_PRINT("Main gate cleared: shutoff timer started (ms): ");
                  DBG_PRINTLN(currentShutoffDelayMs());
              }
              if (shutoffPending &&
                  (millis() - shutoffStartMs >= currentShutoffDelayMs())) {
                  digitalWrite(irrigationPin, LOW);
                  digitalWrite(mistingPin,    LOW);
                  HopperMoveVelocity(0);
                  if (hopperOnStartMs) {
                      t.hopperMotorUptimeMs += millis() - hopperOnStartMs;
                      hopperOnStartMs = 0;
                  }
                  mainOutputsOn  = false;
                  shutoffPending = false;
                  t.traysProcessed++;
                  DBG_PRINTLN("Shutoff timer elapsed: sequence OFF.");
              }
          }
      }
  }

  // Periodic telemetry — fire-and-forget, time-guarded so a slow socket
  // surfaces as a warning instead of silently skewing the control loop.
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
