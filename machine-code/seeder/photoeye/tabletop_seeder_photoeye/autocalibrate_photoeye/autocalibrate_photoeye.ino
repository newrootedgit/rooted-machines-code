#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include "ClearCore.h"

// ============================================================================
// autocalibrate_photoeye
//
// Variant of tabletop_seeder_photoeye that MEASURES the conveyor speed instead
// of looking it up from the operator-reported VFD dial setting.
//
// How it works: the first tray to pass the main laser gate (IO1) after boot is
// a CALIBRATION tray. Nothing fires for it — no solenoids, no roller. We simply
// time how long the beam stays broken. Because the tray length is known
// (TRAY_LENGTH_IN), that dwell time gives us the belt speed, and from there the
// travel time from the gate to the dispense head. Every subsequent tray runs
// the normal sequence using the measured timing.
//
// From the dwell we get the belt speed:
//
//     belt_in_per_sec = TRAY_LENGTH_IN * 1000 / beam_blocked_ms
//
// and every actuator's timing follows from its distance down the belt:
//
//     travel_ms_i = CHANNEL_DISTANCE_IN[i] * 1000 / belt_in_per_sec
//
// Each actuator then replays the gate signal delayed by its own travel_ms, so
// irrigation (2.5"), the seed hopper (9.5") and misting (14") switch on and off
// in sequence as each tray edge reaches them.
// ============================================================================

// No belt motor on this machine. The conveyor is driven by a separate VFD with
// 10 discrete speeds. In this variant the CSV belt_speed field is used only as
// a pre-calibration fallback and as a recalibration trigger — see below.
#define HopperMotor ConnectorM2
#define HANDLE_ALERTS (1)

int accelerationLimit = 100000; // pulses per sec^2

////////////////////////////////////////////////////////////////////////////
///////////////////////////// Pin Map ///////////////////////////////////////
////////////////////////////////////////////////////////////////////////////
// Two laser gates (inputs) and three solenoids (outputs).
// Laser gates read LOW when the beam is broken (object present = triggered).
// Solenoid outputs are driven HIGH to energize. See the polarity defines below.

#define laserGateMain    IO1  // Main sequence trigger + speed calibration source
#define laserGateTopcoat IO0  // Topcoat trigger

#define irrigationPin    IO2  // Main-sequence irrigation solenoid
#define topcoatPin       IO3  // Topcoat irrigation solenoid (follows IO0)
#define mistingPin       IO4  // Main-sequence misting solenoid

// Solenoid drive polarity. The valve drivers on this machine are ACTIVE HIGH:
// driving the output HIGH energizes the coil (valve opens), LOW releases it.
// LOW is therefore the safe/default state and what the pins hold at boot.
// Always write SOL_ON / SOL_OFF to the solenoid pins — never raw HIGH/LOW —
// so a rewire is a one-line change here.
#define SOL_ON   HIGH
#define SOL_OFF  LOW

// Laser gate polarity. The gates are ACTIVE LOW: the input reads HIGH while the
// beam is intact and drops LOW when the beam is broken (object present). Read
// them through GATE_TRIGGERED() rather than testing digitalRead() directly.
#define GATE_TRIGGERED(pin)  (digitalRead(pin) == LOW)

byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};  // MAC address
IPAddress ip(192, 168, 10, 2);                      // Static IP
IPAddress serverIp(192, 168, 10, 1);                // Server IP
#define PORT_NUM 8888
#define MAX_PACKET_LENGTH 200
unsigned char packetReceived[MAX_PACKET_LENGTH];
EthernetClient client;

bool ready_to_run_flag = false;   // =0 at power-up / reset

/////////////////////////////////////////////////////////////////////////////
/////////////////////// Belt Speed Autocalibration //////////////////////////
/////////////////////////////////////////////////////////////////////////////

// >>> MEASURE THIS ON THE MACHINE. <<<
//
// TRAY_LENGTH_IN is the dimension of the tray ALONG THE DIRECTION OF TRAVEL —
// the span that actually occludes the beam. If the trays run crosswise, this is
// the tray width, not its length.
static const float TRAY_LENGTH_IN = 20.0f;

/////////////////////////////////////////////////////////////////////////////
///////////////////////// Actuator Geometry /////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
// Each actuator sits a different distance downstream of the main laser gate, so
// each one gets its own delay. Measured along the belt from the IO1 beam:
//
//     irrigation bar   2.5 in
//     seed hopper      9.5 in
//     misting bar     14.0 in
//
// A tray's leading edge reaches each actuator at gate_time + distance/speed,
// and its trailing edge likewise. Each channel therefore replays the gate
// signal on its own schedule, and the three switch in sequence as the tray
// travels: irrigation first, then seed, then mist.

enum {
    CH_IRRIGATION = 0,
    CH_HOPPER     = 1,   // seed roller
    CH_MISTING    = 2,
    CH_COUNT      = 3
};

static const char * const CHANNEL_NAME[CH_COUNT] = {
    "irrigation", "hopper", "misting"
};

// Distance from the IO1 beam to each actuator, in inches along the belt.
static const float CHANNEL_DISTANCE_IN[CH_COUNT] = {
    2.5f,   // CH_IRRIGATION
    9.5f,   // CH_HOPPER
    14.0f,  // CH_MISTING
};

// Per-channel fixed lead (ms) subtracted from the computed travel time, to
// compensate for actuator response lag — solenoid pull-in, or the roller's
// ramp to speed under AccelMax. Fire this many ms EARLY so the actuator is
// actually doing its job by the time the tray edge arrives underneath it.
//
// >>> TUNE ON THE MACHINE. <<< Start at 0 and add lead if you see the leading
// edge of the tray under-treated. The roller usually needs the most.
static const uint32_t CHANNEL_LEAD_MS[CH_COUNT] = {
    0,   // CH_IRRIGATION — solenoid, tens of ms at most
    0,   // CH_HOPPER      — motor ramp; likely the largest of the three
    0,   // CH_MISTING     — solenoid
};

// Shortest off-window worth actuating, per channel (see MIN_OFF_MS notes below).
static const uint32_t CHANNEL_MIN_OFF_MS[CH_COUNT] = {
    120,  // CH_IRRIGATION
    250,  // CH_HOPPER — motor: stopping and restarting is the most expensive
    120,  // CH_MISTING
};

// Sanity window for a calibration measurement, in ms. A dwell outside this
// range is rejected as not-a-tray (hand through the beam, a jam parked in the
// gate, sensor flicker) and calibration retries on the next tray.
static const uint32_t CAL_MIN_BLOCK_MS = 250;
static const uint32_t CAL_MAX_BLOCK_MS = 15000;

// Absolute clamp on any derived travel time, in ms. Belt-and-suspenders in case
// the geometry constants are edited to something nonsensical. There is no lower
// clamp: the irrigation bar at 2.5" on a fast belt legitimately lands near zero.
static const uint32_t TRAVEL_MAX_MS = 30000;

enum CalState {
    CAL_WAITING   = 0,  // armed, waiting for the calibration tray to arrive
    CAL_MEASURING = 1,  // beam currently broken by the calibration tray
    CAL_DONE      = 2,  // measured; normal sequencing is live
};

CalState  calState            = CAL_WAITING;
uint32_t  calBlockStartMs     = 0;   // millis() when the beam broke
uint32_t  calMeasuredBlockMs  = 0;   // dwell time of the calibration tray
float     calBeltInPerSec     = 0.0f;// derived belt speed, for telemetry/logs
uint32_t  calRejectCount      = 0;   // measurements thrown out by the sanity window

/////////////////////////////////////////////////////////////////////////////
/////////////////////// Gate -> Actuator Delay Lines ////////////////////////
/////////////////////////////////////////////////////////////////////////////
// Each actuator sits CHANNEL_DISTANCE_IN downstream of the laser gate, so the
// belt takes travel_ms to carry any given point from one to the other. Each
// output must therefore replay what the beam saw, delayed by exactly that:
//
//     output_i(t) = gate_state(t - travel_ms_i)
//
// We implement that as a per-channel FIFO of gate edges, stamped with the time
// they happened AT THE GATE. Every edge is pushed to all three channels at once
// and each channel releases it on its own schedule. Independent queues (rather
// than one queue with three cursors) keep each channel's short-gap suppression
// self-contained, and the memory cost is trivial.
//
// This replaces the original "turn on immediately, coast off after a delay"
// scheme, which had two defects: it opened the valves before the tray reached
// the actuator, and — because a new trigger cancelled the pending shutoff — it
// erased any inter-tray gap shorter than the coast time, dumping seed and water
// into the space between two closely spaced trays.
struct GateEvent {
    uint32_t atMs;     // millis() when this edge occurred at the gate
    bool     blocked;  // gate state AFTER the edge (true = beam broken)
};

// 16 edges is 8 tray on/off pairs in flight at once — far more than the 14 in
// between the gate and the farthest actuator can physically hold.
static const uint8_t GATE_FIFO_LEN = 16;

struct DelayLine {
    GateEvent fifo[GATE_FIFO_LEN];
    uint8_t   head;
    uint8_t   tail;
    uint8_t   count;
    uint32_t  drops;      // edges lost to overflow; should stay 0
    uint32_t  travelMs;   // gate -> this actuator, after lead compensation
    bool      headState;  // delayed gate state as seen by this actuator
};

DelayLine channels[CH_COUNT];

// Shortest off-window worth actuating — see CHANNEL_MIN_OFF_MS above for the
// per-channel values. If two trays are separated by a gap that would put an
// actuator down for less than its threshold, we hold it ON through the gap.
// Two reasons: a solenoid has a finite pull-in/drop-out time (typically
// 10-30 ms each way) so a very short pulse is not physically real, and the
// roller has to decelerate and re-accelerate under AccelMax. Chattering either
// to chase a 1/2" gap costs more than the seed it saves.
//
// At a given belt speed the threshold corresponds to a minimum gap width:
//     gap_in = min_off_ms * belt_in_per_s / 1000

/////////////////////////////////////////////////////////////////////////////
///////// User Sequence Modification Values and Motor Speeds ////////////////
/////////////////////////////////////////////////////////////////////////////

float user_hopper_rpm = 100;

// VFD conveyor speed setting (1..10) reported by the operator. Two roles here:
//   1. Fallback shutoff timing before the first successful calibration.
//   2. Change detection — if the operator re-dials the VFD, the belt speed we
//      measured is stale, so we drop back to calibrating on the next tray.
int user_vfd_speed = 1;

// Fallback belt speed (inches/sec) by VFD dial setting, used ONLY until the
// first successful calibration. Once calibrated, the measured speed supersedes
// this entirely. Expressing the fallback as a SPEED rather than as a delay is
// what lets the three actuator distances share one table.
//
// >>> TUNE THESE ON THE MACHINE — one row per VFD speed setting. <<<
// Values below are placeholders.
const float BELT_IN_PER_SEC_BY_SPEED[10] = {
    4.0f,  // speed 1  (slowest)
    5.0f,  // speed 2
    6.0f,  // speed 3
    7.0f,  // speed 4
    8.0f,  // speed 5
    10.0f, // speed 6
    12.0f, // speed 7
    14.0f, // speed 8
    16.0f, // speed 9
    18.0f, // speed 10 (fastest)
};

// Belt speed in inches/sec — measured if calibrated, dial-table fallback if not.
float currentBeltInPerSec() {
    if (calState == CAL_DONE && calBeltInPerSec > 0.0f) {
        return calBeltInPerSec;
    }
    int idx = user_vfd_speed;
    if (idx < 1)  idx = 1;
    if (idx > 10) idx = 10;
    return BELT_IN_PER_SEC_BY_SPEED[idx - 1];
}

// Travel time in ms from the laser gate to actuator `ch`, after subtracting
// that channel's response-lag lead. This is the delay applied to BOTH edges of
// the gate signal for that channel.
uint32_t channelTravelMs(int ch) {
    float speed = currentBeltInPerSec();
    if (speed < 0.1f) speed = 0.1f;   // guard against a divide-by-zero table edit

    float    ms     = (CHANNEL_DISTANCE_IN[ch] * 1000.0f) / speed;
    uint32_t travel = (uint32_t)(ms + 0.5f);

    // Fire early by the channel's lead to cover actuator response time.
    uint32_t lead = CHANNEL_LEAD_MS[ch];
    travel = (lead >= travel) ? 0 : (travel - lead);

    if (travel > TRAVEL_MAX_MS) travel = TRAVEL_MAX_MS;
    return travel;
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

//------------------------------------------------------------------------------
// Calibration helpers
//------------------------------------------------------------------------------

// Throw away any existing calibration and re-arm the measurement. Called at
// boot and whenever the operator changes the VFD dial.
void ResetCalibration(const char *reason) {
    calState           = CAL_WAITING;
    calBlockStartMs    = 0;
    calMeasuredBlockMs = 0;
    calBeltInPerSec    = 0.0f;
    Serial.print("Calibration reset (");
    Serial.print(reason);
    Serial.println("). Next tray is a calibration pass — no dispense.");
}

// Convert a measured beam-break dwell into a belt speed, from which every
// channel's travel time follows. Returns false (and leaves state untouched) if
// the dwell is implausible.
bool ApplyCalibration(uint32_t blockedMs) {
    if (blockedMs < CAL_MIN_BLOCK_MS || blockedMs > CAL_MAX_BLOCK_MS) {
        calRejectCount++;
        Serial.print("Calibration REJECTED: beam blocked ");
        Serial.print(blockedMs);
        Serial.print(" ms, outside [");
        Serial.print(CAL_MIN_BLOCK_MS);
        Serial.print(", ");
        Serial.print(CAL_MAX_BLOCK_MS);
        Serial.println("] ms. Will retry on the next tray.");
        return false;
    }

    calMeasuredBlockMs = blockedMs;
    calBeltInPerSec    = (TRAY_LENGTH_IN * 1000.0f) / (float)blockedMs;
    calState           = CAL_DONE;

    Serial.println("=== Belt speed calibrated ===");
    Serial.print("  tray dwell:  "); Serial.print(calMeasuredBlockMs); Serial.println(" ms");
    Serial.print("  tray length: "); Serial.print(TRAY_LENGTH_IN);     Serial.println(" in");
    Serial.print("  belt speed:  "); Serial.print(calBeltInPerSec);    Serial.println(" in/s");
    Serial.println("  channel delays (gate -> actuator, lead applied):");
    for (int ch = 0; ch < CH_COUNT; ch++) {
        Serial.print("    ");
        Serial.print(CHANNEL_NAME[ch]);
        Serial.print(" @ ");
        Serial.print(CHANNEL_DISTANCE_IN[ch]);
        Serial.print(" in -> ");
        Serial.print(channelTravelMs(ch));
        Serial.println(" ms");
    }
    Serial.println("=== Normal sequencing active ===");
    return true;
}

//------------------------------------------------------------------------------
// Gate -> head delay line
//------------------------------------------------------------------------------

void DelayLineClearAll() {
    for (int ch = 0; ch < CH_COUNT; ch++) {
        channels[ch].head      = 0;
        channels[ch].tail      = 0;
        channels[ch].count     = 0;
        channels[ch].headState = false;
    }
}

// Queue one gate edge onto every channel. All three see the identical event
// stream; they differ only in how long they hold each edge before applying it.
void DelayLinePushAll(uint32_t atMs, bool blocked) {
    for (int ch = 0; ch < CH_COUNT; ch++) {
        DelayLine &d = channels[ch];
        if (d.count >= GATE_FIFO_LEN) {
            // Should be unreachable — the belt cannot hold 8 trays inside 14".
            // Count it and drop the OLDEST edge so we stay in sync with recent
            // reality rather than replaying stale history.
            d.drops++;
            d.head = (uint8_t)((d.head + 1) % GATE_FIFO_LEN);
            d.count--;
            Serial.print("WARN delay line overflow on ");
            Serial.print(CHANNEL_NAME[ch]);
            Serial.println(" — dropped an edge.");
        }
        d.fifo[d.tail].atMs    = atMs;
        d.fifo[d.tail].blocked = blocked;
        d.tail = (uint8_t)((d.tail + 1) % GATE_FIFO_LEN);
        d.count++;
    }
}

static void DelayLinePop(DelayLine &d) {
    d.head = (uint8_t)((d.head + 1) % GATE_FIFO_LEN);
    d.count--;
}

// Advance one channel's delay line, updating its headState to the gate state
// that should now be applied at that actuator.
//
// Short-gap suppression needs no extra hardware: an edge sits in the queue for
// travelMs before it is due, so by the time an OFF edge matures the following ON
// edge is usually already queued behind it. If that pair is closer together than
// the channel's minOffMs, we consume both and stay on through the gap. (This
// works whenever travelMs > minOffMs, which holds comfortably for all three
// actuators at any realistic belt speed.)
void DelayLineAdvance(int ch, uint32_t nowMs) {
    DelayLine     &d        = channels[ch];
    const uint32_t travelMs = d.travelMs;
    const uint32_t minOffMs = CHANNEL_MIN_OFF_MS[ch];

    while (d.count > 0 && (nowMs - d.fifo[d.head].atMs) >= travelMs) {

        bool     edgeBlocked = d.fifo[d.head].blocked;
        uint32_t edgeAtMs    = d.fifo[d.head].atMs;

        // Peek ahead: is this a gap too short to be worth actuating?
        if (!edgeBlocked && d.count >= 2) {
            uint8_t nextIdx = (uint8_t)((d.head + 1) % GATE_FIFO_LEN);
            if (d.fifo[nextIdx].blocked &&
                (d.fifo[nextIdx].atMs - edgeAtMs) < minOffMs) {
                // Swallow both edges — stay on straight through the gap.
                DBG_PRINT("Gap of ");
                DBG_PRINT(d.fifo[nextIdx].atMs - edgeAtMs);
                DBG_PRINT(" ms below threshold on ");
                DBG_PRINT(CHANNEL_NAME[ch]);
                DBG_PRINTLN(" — holding ON.");
                DelayLinePop(d);
                DelayLinePop(d);
                continue;
            }
        }

        d.headState = edgeBlocked;
        DelayLinePop(d);
    }
}

void parseReceivedMessage(char *message) {
    // Expected CSV format (11 fields):
    // ready_to_run,active_variety,roller_speed,belt_speed,
    // irrigation_delay,irrigation_duration,
    // misting_delay,misting_duration,
    // roller_delay,roller_duration,variety_name
    //
    // NOTE: with the belt motor removed, the belt_speed field carries the VFD
    // conveyor speed setting (1..10), and the irrigation/misting/roller delay &
    // duration fields are not used by this firmware. We still parse all 11
    // fields to stay aligned with the Pi's CSV framing.

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
            case 3: // belt_speed -> VFD speed setting (1..10)
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

    // A change to the VFD dial invalidates the measured belt speed. Drop back
    // to calibrating so the next tray re-measures instead of dispensing on
    // stale timing.
    if (vfd_speed_val != user_vfd_speed && calState == CAL_DONE) {
        ResetCalibration("VFD speed changed");
    }
    user_vfd_speed = vfd_speed_val;

    // Rescale hopper RPM (same logic you already had)
    user_hopper_rpm *= 10;

    // Debug prints if you want them:
    DBG_PRINTLN("Parsed CSV Data:");
    DBG_PRINT("ready_to_run_flag: "); DBG_PRINTLN(ready_to_run_flag);
    DBG_PRINT("activeVarietyId: ");   DBG_PRINTLN(activeVarietyId);
    DBG_PRINT("activeVarietyName: "); DBG_PRINTLN(activeVarietyName);
    DBG_PRINT("user_hopper_rpm: ");   DBG_PRINTLN(user_hopper_rpm);
    DBG_PRINT("user_vfd_speed: ");    DBG_PRINTLN(user_vfd_speed);
    DBG_PRINT("cal_state: ");         DBG_PRINTLN((int)calState);
    DBG_PRINT("belt_in_per_sec: ");   DBG_PRINTLN(currentBeltInPerSec());
    DBG_PRINT("travel_ms irr/hop/mist: ");
    DBG_PRINT(channelTravelMs(CH_IRRIGATION)); DBG_PRINT("/");
    DBG_PRINT(channelTravelMs(CH_HOPPER));     DBG_PRINT("/");
    DBG_PRINTLN(channelTravelMs(CH_MISTING));
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

    // Belt speed is sent as HUNDREDTHS of an inch/sec (integer) rather than a
    // float — avoids depending on %f support in the toolchain's snprintf and
    // keeps the wire format integer-only. Divide by 100 on the Pi side.
    uint32_t beltSpeedX100 = (uint32_t)((calBeltInPerSec * 100.0f) + 0.5f);

    // Format (schema_ver 5): STATUS_UPDATE,5,bootId,seq,uptimeMs,
    //   roller_motor_uptime_ms,cmdAgeMs,udpFails,trays_processed,
    //   cal_state,cal_block_ms,belt_speed_x100,cal_rejects,
    //   travel_irrigation_ms,travel_hopper_ms,travel_misting_ms,
    //   varietyId,varietyName
    // NOTE: schema bumped 4 -> 5 — the single shutoff_ms field became one
    // travel time per actuator. The Pi-side telemetry parser must be updated to
    // match this layout.
    // varietyName is LAST so any snprintf truncation chops the name, not
    // the structured numeric tail.
    snprintf(telemetryBuffer, sizeof(telemetryBuffer),
             "STATUS_UPDATE,5,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%d,%lu,%lu,%lu,%lu,%lu,%lu,%d,%s",
             (unsigned long)t.bootId,
             (unsigned long)seq,
             (unsigned long)uptimeMs,
             (unsigned long)hopperUptime,
             (unsigned long)cmdAgeMs,
             (unsigned long)t.udpSendFailCount,
             (unsigned long)t.traysProcessed,
             (int)calState,
             (unsigned long)calMeasuredBlockMs,
             (unsigned long)beltSpeedX100,
             (unsigned long)calRejectCount,
             (unsigned long)channelTravelMs(CH_IRRIGATION),
             (unsigned long)channelTravelMs(CH_HOPPER),
             (unsigned long)channelTravelMs(CH_MISTING),
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

    // Format (schema_ver 5): EVENT,5,bootId,seq,uptimeMs,eventCode,eventValue,motor
    snprintf(telemetryBuffer, sizeof(telemetryBuffer),
             "EVENT,5,%lu,%lu,%lu,%s,%d,%s",
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
    // Laser gates: inputs (LOW = beam broken / triggered).
    pinMode(laserGateMain,    INPUT);
    pinMode(laserGateTopcoat, INPUT);

    // Solenoids: outputs, start de-energized.
    pinMode(irrigationPin, OUTPUT);
    pinMode(topcoatPin,    OUTPUT);
    pinMode(mistingPin,    OUTPUT);
    digitalWrite(irrigationPin, SOL_OFF);
    digitalWrite(topcoatPin,    SOL_OFF);
    digitalWrite(mistingPin,    SOL_OFF);

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

    for (int ch = 0; ch < CH_COUNT; ch++) {
        channels[ch].drops    = 0;
        channels[ch].travelMs = 0;
    }
    DelayLineClearAll();

    ResetCalibration("boot");
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
  // Active low: the pin sits HIGH with the beam intact and goes LOW when the
  // beam is broken. mainGate/topcoatGate are true when the beam IS broken.
  bool mainGate    = GATE_TRIGGERED(laserGateMain);
  bool topcoatGate = GATE_TRIGGERED(laserGateTopcoat);

  ////////////////////////////////////////////////////////////
  ////////////// Arm / Disarm Gate Inhibit ////////////////////
  ////////////////////////////////////////////////////////////
  // OFF is the default state of this machine. A laser gate may only energize a
  // solenoid while ready_to_run is active, AND only after that gate has been
  // seen CLEAR at least once since arming. Without the second condition, a beam
  // that happens to be blocked at the moment the operator arms (tray sitting in
  // the gate, misaligned emitter, hand in the beam) fires the valves instantly
  // on the arm edge. Requiring a fresh clear->blocked edge means only a real
  // tray passing through can trigger a cycle — and, here, only a whole tray can
  // be used as a calibration sample.
  static bool prevReady          = false;
  static bool mainGateInhibit    = false;
  static bool topcoatGateInhibit = false;

  if (ready_to_run_flag && !prevReady) {
      // Arm edge: whatever the gates read right now does not count as a trigger.
      mainGateInhibit    = mainGate;
      topcoatGateInhibit = topcoatGate;
      if (mainGateInhibit || topcoatGateInhibit) {
          DBG_PRINTLN("Armed with a gate already blocked: ignoring until it clears.");
      }
  }
  prevReady = ready_to_run_flag;

  // A gate is released from inhibit as soon as it reads clear.
  if (!mainGate)    mainGateInhibit    = false;
  if (!topcoatGate) topcoatGateInhibit = false;

  bool mainTriggered    = ready_to_run_flag && mainGate    && !mainGateInhibit;
  bool topcoatTriggered = ready_to_run_flag && topcoatGate && !topcoatGateInhibit;

  ////////////////////////////////////////////////////////////
  //////////////// Belt Speed Calibration Pass ////////////////
  ////////////////////////////////////////////////////////////
  // The first qualifying tray after boot (or after a VFD change) is measured,
  // not dispensed on. We time the full clear->blocked->clear span of IO1.
  //
  // Only a COMPLETE dwell counts. If the machine is disarmed mid-measurement we
  // abort rather than record a truncated span, because a disarm collapses
  // mainTriggered to false and would otherwise look exactly like the tray's
  // trailing edge.
  {
      static bool prevMainTrigCal = false;

      if (calState != CAL_DONE) {
          if (!ready_to_run_flag) {
              if (calState == CAL_MEASURING) {
                  Serial.println("Calibration aborted: disarmed mid-tray.");
                  calState = CAL_WAITING;
              }
          } else if (mainTriggered && !prevMainTrigCal) {
              // Leading edge of the calibration tray.
              calBlockStartMs = millis();
              calState        = CAL_MEASURING;
              DBG_PRINTLN("Calibration tray entered the gate — timing (no dispense).");
          } else if (!mainTriggered && prevMainTrigCal && calState == CAL_MEASURING) {
              // Trailing edge — we have a full dwell. On rejection ApplyCalibration
              // leaves us in CAL_WAITING so the next tray gets another attempt.
              uint32_t blockedMs = millis() - calBlockStartMs;
              if (!ApplyCalibration(blockedMs)) {
                  calState = CAL_WAITING;
              }
          }
      }

      prevMainTrigCal = mainTriggered;
  }

  // While uncalibrated, NOTHING actuates: no valves, no roller. This is what
  // makes the first tray a pure measurement pass.
  bool calibrating     = (calState != CAL_DONE);
  bool sequenceTrigger = mainTriggered    && !calibrating;
  bool topcoatOn       = topcoatTriggered && !calibrating;

  ////////////////////////////////////////////////////////////
  ///////// Main Sequence (IO1 -> hopper + IO2 + IO4) /////////
  ////////////////////////////////////////////////////////////
  // Each actuator replays the gate, delayed by its own gate->actuator travel
  // time. Every edge — leading and trailing alike — goes through the same delay
  // line, so what each actuator does is a faithful copy of what the beam saw,
  // shifted downstream by that actuator's distance. Trays that touch produce no
  // gate edge and stay on continuously; trays with a real gap produce a real
  // off-window in the right place for each actuator in turn, unless that window
  // is shorter than the channel's minimum.
  bool irrigationOn = false;
  bool mistingOn    = false;
  {
      static bool     prevGate        = false;  // last gate state pushed to the lines
      static bool     hopperRunning   = false;
      static uint32_t hopperOnStartMs = 0;

      // Force everything off if the machine gets disarmed mid-run, or if a VFD
      // change dropped us back into calibration. This clears ALL latched state
      // unconditionally — not just on the falling edge — so the machine can
      // never be left holding queued edges or a stale "outputs are on" flag
      // that would resurface later.
      if (!ready_to_run_flag || calibrating) {
          if (hopperRunning) {
              HopperMoveVelocity(0);
              if (hopperOnStartMs) {
                  t.hopperMotorUptimeMs += millis() - hopperOnStartMs;
                  hopperOnStartMs = 0;
              }
              DBG_PRINTLN("Disarmed or recalibrating: sequence forced OFF.");
          }
          DelayLineClearAll();
          prevGate      = false;
          hopperRunning = false;
      } else {
          uint32_t nowMs = millis();

          // Refresh each channel's travel time. Recomputed every pass so a
          // recalibration takes effect immediately, and so edges already in
          // flight are released on the newest speed estimate.
          for (int ch = 0; ch < CH_COUNT; ch++) {
              channels[ch].travelMs = channelTravelMs(ch);
          }

          // Stamp every gate edge as it happens and queue it on all channels.
          if (sequenceTrigger != prevGate) {
              DelayLinePushAll(nowMs, sequenceTrigger);
              prevGate = sequenceTrigger;

              // One tray = one leading edge at the gate.
              if (sequenceTrigger) t.traysProcessed++;

              DBG_PRINT(sequenceTrigger ? "Gate BLOCKED" : "Gate CLEAR");
              DBG_PRINT(" queued; applies at irrigation/hopper/misting (ms): ");
              DBG_PRINT(channels[CH_IRRIGATION].travelMs); DBG_PRINT("/");
              DBG_PRINT(channels[CH_HOPPER].travelMs);     DBG_PRINT("/");
              DBG_PRINTLN(channels[CH_MISTING].travelMs);
          }

          // Release whatever has aged past each channel's own travel time.
          for (int ch = 0; ch < CH_COUNT; ch++) {
              DelayLineAdvance(ch, nowMs);
          }

          irrigationOn = channels[CH_IRRIGATION].headState;
          mistingOn    = channels[CH_MISTING].headState;

          // The roller is a motor, not a valve, so it is edge-driven off its
          // own channel rather than written every pass.
          bool hopperWanted = channels[CH_HOPPER].headState;
          if (hopperWanted && !hopperRunning) {
              HopperMoveVelocity(user_hopper_rpm);
              hopperOnStartMs = nowMs;
              hopperRunning   = true;
              DBG_PRINTLN("Tray at hopper: roller ON.");
          } else if (!hopperWanted && hopperRunning) {
              HopperMoveVelocity(0);
              if (hopperOnStartMs) {
                  t.hopperMotorUptimeMs += nowMs - hopperOnStartMs;
                  hopperOnStartMs = 0;
              }
              hopperRunning = false;
              DBG_PRINTLN("Tray past hopper: roller OFF.");
          }
      }
  }

  ////////////////////////////////////////////////////////////
  ///////////////// Drive Solenoid Outputs ////////////////////
  ////////////////////////////////////////////////////////////
  // Single write point for all three valves, re-asserted EVERY pass from the
  // state computed above rather than only on transitions. Outputs therefore
  // cannot be left latched in the wrong state by any path through the logic.
  // Disarmed OR uncalibrated forces all three OFF here on every iteration.
  if (!ready_to_run_flag || calibrating) {
      topcoatOn    = false;
      irrigationOn = false;
      mistingOn    = false;
  }
  digitalWrite(topcoatPin,    topcoatOn    ? SOL_ON : SOL_OFF);
  digitalWrite(irrigationPin, irrigationOn ? SOL_ON : SOL_OFF);
  digitalWrite(mistingPin,    mistingOn    ? SOL_ON : SOL_OFF);

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

  // Loop cadence. This also sets the resolution of the calibration measurement
  // and of the shutoff timer — at 10 ms, a dwell reading carries roughly +/-10 ms
  // of quantization, which on a 2000 ms tray is well under 1%. Drop to 1 if you
  // need finer timing.
  delay(10);
}
