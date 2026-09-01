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

// Raw level IO3 reads when the beam is BROKEN by a tray. This machine's gate is
// light-on (it drives the input HIGH while the beam is clear and releases it
// when a tray interrupts), so blocked = LOW. Swap to HIGH if the sensor, its
// output type, or its light-on/dark-on jumper is ever changed.
//
// Everything downstream is written in terms of "is a tray blocking the beam",
// so this is normalized once at the read and nowhere else — see inputState in
// loop(). Do NOT fix a polarity problem by flipping the edge test instead: the
// roller-start gate reads the same signal as a level, and inverting only the
// edge would leave the gate backwards and suppress the roller on every tray.
#define PHOTOEYE_BLOCKED_LEVEL LOW

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

/////////////////////////////////////////////////////////////////////////////
///////// User Sequence Modification Values and Motor Speeds ////////////////
/////////////////////////////////////////////////////////////////////////////
//
// sequenceActive, startTime and the six absolute *_start_time / *_end_time
// globals are gone. They belonged to the single-sequence state machine, which
// could only track one tray at a time; the delay lines carry per-edge
// timestamps instead, so several sheets can be in the machine at once.
//
// The six mod values below survive unchanged — they are the operator's trim,
// and they now shift each channel's ON and OFF edge independently. See
// ComputeChannelDelays().

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
    uint32_t hopperMotorUptimeMs; // cumulative roller run time since boot, accrued
                                  // deterministically from each commanded run window
    uint32_t traysProcessed;       // number of completed sequences since boot
};
TelemetryState t;



////////////////////////////////////////////////////////////
////////// Actuator geometry and delay lines ///////////////
////////////////////////////////////////////////////////////
//
// ARCHITECTURE — read this before touching the timing.
//
// Each actuator sits a fixed distance downstream of the photoeye, so the belt
// takes travel_i to carry any given point from the gate to that actuator. Each
// output therefore replays what the beam saw, delayed by exactly that:
//
//     output_i(t) = gate_state(t - travel_i)
//
// implemented as a per-channel FIFO of gate edges, each stamped with the time
// it happened AT THE GATE. Every edge is pushed to all three channels at once
// and each channel releases it on its own schedule.
//
// This REPLACED a single-sequence state machine that started on a photoeye
// rising edge and ran absolute timings from it. That design could not do two
// things this machine needs:
//
//   1. BACK-TO-BACK SHEETS. Only one sequence could be live, and a sequence did
//      not end until the tray had cleared misting — so the next sheet had to be
//      15.04 in behind (exactly the gate->misting distance) or it was dropped,
//      silently and untreated, at every belt speed. Here overlap is the normal
//      case: sheet two's ON edge queues while sheet one's OFF edges are still
//      draining, so irrigation can be treating one sheet while misting finishes
//      the one before it. Required spacing is zero.
//
//   2. PUNNET SHEETS. A tray is a moulded sheet of punnets, so the beam breaks
//      and clears several times per sheet. The old roller-start gate sampled
//      the beam at ONE instant and latched; when that instant fell in a gap the
//      roller was suppressed for the entire sheet. Gaps are now absorbed at the
//      actuator by CHANNEL_MIN_OFF_MS, AFTER the delay, so they never reach the
//      trigger logic and impose no spacing requirement of their own. Nothing
//      here needs to know the punnet pitch or count.
//
// Consequence worth knowing: stop times now come from the sheet's real trailing
// edge, not from an assumed tray length. A wrong tray_length can no longer make
// an actuator cut off early or late.

enum {
    CH_IRRIGATION = 0,
    CH_ROLLER     = 1,   // seed roller / hopper motor
    CH_MISTING    = 2,
    CH_COUNT      = 3
};

static const char * const CHANNEL_NAME[CH_COUNT] = {
    "irrigation", "roller", "misting"
};

// refarm Dubai, measured from the CENTRE of the laser gate along belt travel.
// True distances in metres, checkable with a tape measure.
static const float CHANNEL_DISTANCE_M[CH_COUNT] = {
    0.125817f,  //  4.953424 in — first solenoid
    0.248485f,  //  9.782867 in — roller
    0.382054f,  // 15.041477 in — final solenoid
};

// Shortest OFF window worth actuating, per channel. This is what makes punnet
// sheets work: if two blocked spans are separated by less than this, the
// actuator is held ON straight through rather than chattering. A solenoid has a
// finite pull-in and drop-out time so a shorter pulse is not physically real,
// and the roller would have to decelerate and re-accelerate under AccelMax.
//
// It also sets the boundary between "gap between punnets" (bridge it) and "gap
// between sheets" (a real OFF). At belt setting 15 a 0.42 in punnet gap is
// ~61 ms, far under the roller's 250 ms, while two separate sheets are an
// unambiguous OFF at any spacing the infeed can produce.
static const uint32_t CHANNEL_MIN_OFF_MS[CH_COUNT] = {
    120,  // CH_IRRIGATION
    250,  // CH_ROLLER — motor: stopping and restarting is the most expensive
    120,  // CH_MISTING
};

// Shortest ON pulse worth producing, per channel. The floor the operator's stop
// trim is clamped against: a pulse is dwell + (travelOff - travelOn), so a stop
// trim more negative than the sheet's own dwell would close the actuator before
// it opened.
static const uint32_t CHANNEL_MIN_ON_MS[CH_COUNT] = {
    120,  // CH_IRRIGATION
    250,  // CH_ROLLER
    120,  // CH_MISTING
};

// Length of one sheet along travel. NO LONGER sets when anything stops — the
// real trailing edge does that. Its only remaining job is to say how long a
// sheet holds the beam, which is the budget a negative stop trim is measured
// against in ComputeChannelDelays().
float tray_length = 0.34925; // 13.75 in — moulded punnet sheet, whole string

// Absolute ceiling on any derived travel time. Belt-and-suspenders against a
// nonsensical belt_speed or distance edit; no real sequence approaches it.
static const float TRAVEL_MAX_MS = 30000.0f;

/////////////////////////////////////////////////////////////////////////////
/////////////////////// Gate -> Actuator delay lines ////////////////////////
/////////////////////////////////////////////////////////////////////////////

struct GateEvent {
    uint32_t atMs;     // millis() when this edge occurred at the gate
    bool     blocked;  // gate state AFTER the edge (true = beam broken)
};

// 16 edges is 8 on/off pairs in flight at once. A punnet sheet pushes one pair
// per punnet, so this holds two full sheets of four in the worst case plus the
// spacing between them — comfortably more than the 15 in between the gate and
// the farthest actuator can physically contain.
static const uint8_t GATE_FIFO_LEN = 16;

struct DelayLine {
    GateEvent fifo[GATE_FIFO_LEN];
    uint8_t   head;
    uint8_t   tail;
    uint8_t   count;
    uint32_t  drops;        // edges lost to overflow; should stay 0
    // The two edges are held for different durations: that difference is what
    // lengthens or shortens the pulse relative to the sheet's own dwell, and is
    // where the operator's start/stop trim lands.
    uint32_t  travelOnMs;   // gate rising  -> actuator ON
    uint32_t  travelOffMs;  // gate falling -> actuator OFF
    bool      headState;    // delayed gate state as seen by this actuator
};

DelayLine channels[CH_COUNT];

void DelayLinePush(DelayLine &d, uint32_t atMs, bool blocked) {
    if (d.count >= GATE_FIFO_LEN) {
        d.drops++;   // never expected; surfaced in telemetry rather than hidden
        return;
    }
    d.fifo[d.tail].atMs    = atMs;
    d.fifo[d.tail].blocked = blocked;
    d.tail  = (uint8_t)((d.tail + 1) % GATE_FIFO_LEN);
    d.count++;
}

void DelayLinePop(DelayLine &d) {
    if (d.count == 0) return;
    d.head = (uint8_t)((d.head + 1) % GATE_FIFO_LEN);
    d.count--;
}

// Release whatever has aged past this channel's own travel time, absorbing any
// OFF window too short to be worth actuating.
void DelayLineAdvance(int ch, uint32_t nowMs) {
    DelayLine     &d        = channels[ch];
    const uint32_t minOffMs = CHANNEL_MIN_OFF_MS[ch];

    while (d.count > 0) {
        const bool     edgeBlocked = d.fifo[d.head].blocked;
        const uint32_t edgeAtMs    = d.fifo[d.head].atMs;

        // ON edges wait travelOnMs, OFF edges travelOffMs. The queue stays
        // strictly in order, so an OFF can never overtake the ON it belongs to
        // even when travelOffMs is the smaller of the two — it just becomes due
        // immediately after, which would be a zero-length pulse. That is what
        // CHANNEL_MIN_ON_MS prevents in ComputeChannelDelays().
        const uint32_t dueAfterMs = edgeBlocked ? d.travelOnMs : d.travelOffMs;
        if ((nowMs - edgeAtMs) < dueAfterMs) break;

        // Peek ahead: is this an OFF window too short to be worth actuating —
        // i.e. a gap between punnets rather than the end of a sheet?
        if (!edgeBlocked && d.count >= 2) {
            const uint8_t nextIdx = (uint8_t)((d.head + 1) % GATE_FIFO_LEN);
            if (d.fifo[nextIdx].blocked &&
                (d.fifo[nextIdx].atMs - edgeAtMs) < minOffMs) {
                // Swallow both edges — hold ON straight through the gap.
                DelayLinePop(d);
                DelayLinePop(d);
                continue;
            }
        }

        d.headState = edgeBlocked;
        DelayLinePop(d);
    }
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
    // NOTE: roller run time is NOT tracked here. Because the roller runs in
    // short bursts between telemetry samples, reading elapsed-since-start almost
    // always caught it idle. Instead we accrue the deterministic run window
    // (rollerEnd - rollerStart) once per sequence where the roller commits.
    HopperMotor.MoveVelocity(HOPPER_VELOCITY_GAIN * velocity);
    return true;
}

////////////////////////////////////////////////////////////
/////////////// Belt speed and channel timing //////////////
////////////////////////////////////////////////////////////

// Belt travel in metres per second per unit of the operator's belt speed
// setting (screen 3, range 0-20). Measured on this machine: time to travel
// 10 inches, 3-6 runs at each of seven settings from 5 to 20, least squares
// through the origin. 0.45396 in/s per unit, R^2 = 0.998.
static const float BELT_M_PER_S_PER_UNIT = 0.011531f;

// user_belt_rpm is the operator setting already multiplied by 500 at parse time
// — a pulse rate, not the setting — so the 500 comes back out here.
float currentBeltSpeed() {
    return (user_belt_rpm / 500.0f) * BELT_M_PER_S_PER_UNIT;
}

bool beltSpeedValid() {
    return currentBeltSpeed() > 0.01f;
}

// How long one sheet holds the beam at the current belt speed. This is the
// budget a negative stop trim is measured against: a pulse is dwell +
// (travelOff - travelOn), so a stop trim more negative than the dwell would
// close the actuator before it opened.
uint32_t expectedDwellMs() {
    float v = currentBeltSpeed();
    if (v < 0.001f) v = 0.001f;
    return (uint32_t)((tray_length / v) * 1000.0f + 0.5f);
}

// Pure geometry: gate -> actuator, BEFORE the operator's trim.
uint32_t channelTravelMs(int ch) {
    float v = currentBeltSpeed();
    if (v < 0.001f) v = 0.001f;
    float ms = (CHANNEL_DISTANCE_M[ch] / v) * 1000.0f;
    if (ms > TRAVEL_MAX_MS) ms = TRAVEL_MAX_MS;
    return (uint32_t)(ms + 0.5f);
}

// Why an edge ended up somewhere other than base + trim. Recorded per edge so
// the operator is told which control was overruled and by which rule, rather
// than a blanket "something was clamped" covering all six numbers.
//
// Rules are applied in order and a later one can move an edge an earlier one
// already moved, so this records the LAST rule to change it — the one actually
// holding the value where it is.
enum ClampReason {
    CLAMP_NONE = 0,
    CLAMP_NEGATIVE,   // rule 1
    CLAMP_ORDER,      // rule 2
    CLAMP_MIN_ON,     // rule 3
    CLAMP_CEILING,    // rule 4
};

const char *ClampReasonText(uint8_t reason) {
    switch (reason) {
        case CLAMP_NEGATIVE: return "cannot act before the gate edge that triggered it";
        case CLAMP_ORDER:    return "would fire out of physical sequence";
        case CLAMP_MIN_ON:   return "pulse shorter than this channel's minimum";
        case CLAMP_CEILING:  return "beyond the travel-time ceiling";
        default:             return "";
    }
}

// Apply the operator's trim to the geometric base and clamp the result into
// what the machine can physically do.
//
// WHY CLAMP RATHER THAN REJECT. Whether a trim is achievable depends on belt
// speed and sheet length, and the HMI knows neither — it cannot validate these
// at entry. Rejecting here would leave the operator with a saved value that
// silently does nothing, which is the failure this exists to remove. Railing
// behaves like every other physical control: keep turning and you reach the
// stop, and the caller says so.
//
// Plain arrays rather than a struct out-param on purpose: the Arduino build
// inserts generated prototypes above the first function definition in the file,
// so a user type named in a signature has to be declared before that point.
// Builtin types sidestep the ordering entirely.
//
// The four rules, in order. Each is applied AFTER the previous so a later rule
// cannot reintroduce a violation of an earlier one:
//
//   1. No delay is negative. The earliest anything can happen is the instant
//      the gate edge occurs; we cannot act before the sheet we are reacting to.
//   2. The physical order holds: irrigation, then roller, then misting. They
//      sit at 4.95, 9.78 and 15.04 in and a sheet reaches them in that order.
//   3. Every pulse stays at least CHANNEL_MIN_ON_MS long, measured against the
//      dwell. This is the rule that catches "stop trim longer than the sheet".
//   4. Nothing exceeds TRAVEL_MAX_MS.
//
// NOTE there is deliberately no "must start while the sheet is still over the
// photoeye" rule. That was needed only by the old roller-start gate, which
// sampled the beam once and latched. Nothing samples the beam now — each edge
// carries its own timestamp through the FIFO — so a trim that pushes an
// actuator past the trailing edge simply produces a later pulse, which is what
// the operator asked for.
bool ComputeChannelDelays(uint32_t outOn[], uint32_t outOff[],
                          int32_t askedOn[], int32_t askedOff[],
                          uint8_t reasonOn[], uint8_t reasonOff[]) {
    const int64_t dwell = (int64_t)expectedDwellMs();
    bool clamped = false;

    const float trimOnMs[CH_COUNT] = {
        user_irrigation_start_mod_value * 100.0f,
        user_roller_start_mod_value     * 100.0f,
        user_misting_start_mod_value    * 100.0f,
    };
    const float trimOffMs[CH_COUNT] = {
        user_irrigation_end_mod_value * 100.0f,
        user_roller_end_mod_value     * 100.0f,
        user_misting_end_mod_value    * 100.0f,
    };

    int64_t on[CH_COUNT], off[CH_COUNT];

    for (int ch = 0; ch < CH_COUNT; ch++) {
        const int64_t base = (int64_t)channelTravelMs(ch);
        on[ch]  = base + (int64_t)trimOnMs[ch];
        off[ch] = base + (int64_t)trimOffMs[ch];

        askedOn[ch]   = (int32_t)on[ch];
        askedOff[ch]  = (int32_t)off[ch];
        reasonOn[ch]  = CLAMP_NONE;
        reasonOff[ch] = CLAMP_NONE;

        // Rule 1 — nothing before the edge that caused it.
        if (on[ch]  < 0) { on[ch]  = 0; reasonOn[ch]  = CLAMP_NEGATIVE; clamped = true; }
        if (off[ch] < 0) { off[ch] = 0; reasonOff[ch] = CLAMP_NEGATIVE; clamped = true; }
    }

    // Rule 2 — keep the physical sequence. Raise a later channel to its
    // predecessor rather than lowering the earlier one, so a trim can always
    // delay a stage but never drag the stage ahead of it backwards.
    for (int ch = 1; ch < CH_COUNT; ch++) {
        if (on[ch]  < on[ch - 1])  { on[ch]  = on[ch - 1];  reasonOn[ch]  = CLAMP_ORDER; clamped = true; }
        if (off[ch] < off[ch - 1]) { off[ch] = off[ch - 1]; reasonOff[ch] = CLAMP_ORDER; clamped = true; }
    }

    for (int ch = 0; ch < CH_COUNT; ch++) {
        // Rule 3 — a real pulse. Length is dwell + (off - on), so the floor on
        // off is on - dwell + minimum. Applied after rule 2 because raising an
        // ON edge there shortens that channel's pulse.
        const int64_t minOff = on[ch] - dwell + (int64_t)CHANNEL_MIN_ON_MS[ch];
        if (off[ch] < minOff) { off[ch] = minOff; reasonOff[ch] = CLAMP_MIN_ON;  clamped = true; }
        if (off[ch] < 0)      { off[ch] = 0;      reasonOff[ch] = CLAMP_NEGATIVE; clamped = true; }

        // Rule 4 — absolute ceiling.
        if (on[ch]  > (int64_t)TRAVEL_MAX_MS) { on[ch]  = (int64_t)TRAVEL_MAX_MS; reasonOn[ch]  = CLAMP_CEILING; clamped = true; }
        if (off[ch] > (int64_t)TRAVEL_MAX_MS) { off[ch] = (int64_t)TRAVEL_MAX_MS; reasonOff[ch] = CLAMP_CEILING; clamped = true; }

        outOn[ch]  = (uint32_t)on[ch];
        outOff[ch] = (uint32_t)off[ch];
    }

    return clamped;
}

// Recompute both edges for every channel and store them on the delay lines.
//
// Reports only when the numbers change. This runs every pass of the main loop,
// so an unconditional print would bury the log; but a silent clamp repeats the
// failure this exists to fix, where a control appears to work and does not.
void RefreshChannelDelays() {
    static bool     haveLast = false;
    static uint32_t lastOn[CH_COUNT], lastOff[CH_COUNT];
    static uint8_t  lastReasonOn[CH_COUNT]  = { CLAMP_NONE, CLAMP_NONE, CLAMP_NONE };
    static uint8_t  lastReasonOff[CH_COUNT] = { CLAMP_NONE, CLAMP_NONE, CLAMP_NONE };

    uint32_t on[CH_COUNT], off[CH_COUNT];
    int32_t  askedOn[CH_COUNT], askedOff[CH_COUNT];
    uint8_t  reasonOn[CH_COUNT], reasonOff[CH_COUNT];
    const bool clamped = ComputeChannelDelays(on, off, askedOn, askedOff,
                                              reasonOn, reasonOff);

    bool changed = !haveLast;
    for (int ch = 0; ch < CH_COUNT && !changed; ch++) {
        if (on[ch] != lastOn[ch] || off[ch] != lastOff[ch])   changed = true;
        if (reasonOn[ch]  != lastReasonOn[ch])                changed = true;
        if (reasonOff[ch] != lastReasonOff[ch])               changed = true;
    }

    for (int ch = 0; ch < CH_COUNT; ch++) {
        channels[ch].travelOnMs  = on[ch];
        channels[ch].travelOffMs = off[ch];
        lastOn[ch]        = on[ch];
        lastOff[ch]       = off[ch];
        lastReasonOn[ch]  = reasonOn[ch];
        lastReasonOff[ch] = reasonOff[ch];
    }
    haveLast = true;

    if (changed) {
        Serial.print("Channel timing (sheet dwell ");
        Serial.print(expectedDwellMs());
        Serial.println(" ms):");
        for (int ch = 0; ch < CH_COUNT; ch++) {
            Serial.print("  ");
            Serial.print(CHANNEL_NAME[ch]);
            Serial.print(": base ");    Serial.print(channelTravelMs(ch));
            Serial.print(" ms -> on "); Serial.print(on[ch]);
            Serial.print(" ms, off ");  Serial.print(off[ch]);
            Serial.println(" ms");
        }
        if (clamped) {
            for (int ch = 0; ch < CH_COUNT; ch++) {
                if (reasonOn[ch] != CLAMP_NONE) {
                    Serial.print("  CLAMPED ");   Serial.print(CHANNEL_NAME[ch]);
                    Serial.print(" ON: asked ");  Serial.print(askedOn[ch]);
                    Serial.print(" ms -> ");      Serial.print(on[ch]);
                    Serial.print(" ms (");        Serial.print(ClampReasonText(reasonOn[ch]));
                    Serial.println(")");
                }
                if (reasonOff[ch] != CLAMP_NONE) {
                    Serial.print("  CLAMPED ");   Serial.print(CHANNEL_NAME[ch]);
                    Serial.print(" OFF: asked "); Serial.print(askedOff[ch]);
                    Serial.print(" ms -> ");      Serial.print(off[ch]);
                    Serial.print(" ms (");        Serial.print(ClampReasonText(reasonOff[ch]));
                    Serial.println(")");
                }
            }
        }
    }
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

    uint32_t beltUptime   = t.beltMotorUptimeMs ? (uptimeMs - t.beltMotorUptimeMs) : 0;
    uint32_t hopperUptime = t.hopperMotorUptimeMs; // cumulative deterministic roller run time

    // Format (schema_ver 2): STATUS_UPDATE,2,bootId,seq,uptimeMs,
    //   belt_motor_uptime_ms,roller_motor_uptime_ms,cmdAgeMs,udpFails,
    //   trays_processed,varietyId,varietyName
    // hopperUptime maps to roller_motor_uptime_ms (hopper = roller). Unlike the
    // belt (continuous, reported as current run duration), the roller value is
    // a cumulative total of commanded run windows since boot.
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

    // Serial.println("[line-framed-v2] firmware boot");
    // if (client.connect(serverIp, PORT_NUM)) {
    //     Serial.println("Connected to server.");
    // } else {
    //     Serial.println("Failed to connect to server.");
    // }

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
  //////////////// Gate sampling and dispatch /////////////////
  ////////////////////////////////////////////////////////////
  //
  // Everything below is edge-driven. There is no sequence state, no "currently
  // processing a tray" flag, and nothing samples the beam at a chosen instant.
  // Each gate edge is timestamped once, pushed to all three channels, and each
  // channel releases it after its own travel time. That is what lets sheets run
  // back to back: two sheets simply put more edges in the queues.

  RefreshChannelDelays();

  const uint32_t nowMs = millis();

  // Polarity normalized at the single point the pin is read. TRUE means the
  // beam is broken. Punnet gaps are NOT filtered here — they are absorbed per
  // channel in DelayLineAdvance() by CHANNEL_MIN_OFF_MS, after the delay, which
  // is what keeps them from imposing any spacing requirement between sheets.
  const bool gateBlocked = (digitalRead(inputPin1) == PHOTOEYE_BLOCKED_LEVEL);

  static bool lastGateBlocked = false;
  static bool haveGateState   = false;
  if (!haveGateState) {
      lastGateBlocked = gateBlocked;
      haveGateState   = true;
  } else if (gateBlocked != lastGateBlocked) {
      // Queue the edge on every channel at once. Refuse while disarmed or
      // without a usable belt speed: travel times would be meaningless, and an
      // edge queued now would fire minutes later when the belt restarts.
      if (ready_to_run_flag && beltSpeedValid()) {
          for (int ch = 0; ch < CH_COUNT; ch++) {
              DelayLinePush(channels[ch], nowMs, gateBlocked);
          }
      }
      lastGateBlocked = gateBlocked;
  }

  // Release whatever has aged past each channel's own travel time.
  for (int ch = 0; ch < CH_COUNT; ch++) {
      DelayLineAdvance(ch, nowMs);
  }

  // Disarmed: drop every output and discard queued edges, so re-arming does not
  // replay a sheet that has long since left the machine.
  if (!ready_to_run_flag) {
      for (int ch = 0; ch < CH_COUNT; ch++) {
          channels[ch].head = channels[ch].tail = channels[ch].count = 0;
          channels[ch].headState = false;
      }
  }

  ////////////////////////////////////////////////////////////
  //////////////////// Drive the actuators ////////////////////
  ////////////////////////////////////////////////////////////

  // Solenoids are level devices: write them unconditionally every pass.
  digitalWrite(relay0Pin, channels[CH_IRRIGATION].headState ? HIGH : LOW);
  digitalWrite(relay1Pin, channels[CH_MISTING].headState    ? HIGH : LOW);

  // The roller is a motor, not a valve, so it is edge-driven off its own
  // channel rather than commanded every pass.
  {
      static bool     rollerRunning = false;
      static uint32_t rollerOnAtMs  = 0;
      const bool rollerWanted = channels[CH_ROLLER].headState;

      if (rollerWanted && !rollerRunning) {
          HopperMoveVelocity(user_hopper_rpm);
          rollerRunning = true;
          rollerOnAtMs  = nowMs;

          // One roller ON transition == one sheet. The punnet gaps have already
          // been swallowed by CHANNEL_MIN_OFF_MS, so this counts sheets rather
          // than punnets without needing to know how many punnets a sheet has.
          t.traysProcessed++;
          DBG_PRINTLN("Sheet at roller: ON.");
      } else if (!rollerWanted && rollerRunning) {
          HopperMoveVelocity(0);
          rollerRunning = false;
          if (rollerOnAtMs) {
              t.hopperMotorUptimeMs += nowMs - rollerOnAtMs;
              rollerOnAtMs = 0;
          }
          DBG_PRINTLN("Sheet past roller: OFF.");
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


