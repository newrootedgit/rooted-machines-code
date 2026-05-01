#include "ClearCore.h"
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>

#define BladeMotor ConnectorM0

#define LinearRailMotor ConnectorM2
#define BeltMotor ConnectorM3

// Pins that support digital interrupts: DI-6, DI-7, DI-8, A-9, A-10, A-11, A-12
#define UpperLimitPin DI7  // DI7 -> Upper Limit Switch (supports interrupts)
#define LowerLimitPin DI8  // DI8 -> Lower Limit Switch (supports interrupts)
#define TrayDetectionPin IO3  // IO3 -> Tray Detection Limit Switch


#define KillSwitchPin DI6  // Blade Soft Kill Switch

// Solenoid outputs (ClearCore IO connectors)
#define TopSolenoid ConnectorIO2
#define BotSolenoid ConnectorIO1

// Solenoid timing configuration
#define BOT_SOLENOID_DURATION_MS 500   // Bottom solenoid on duration
#define FIRST_PULSE_DELAY_MS 1000      // Delay after bottom off before first top pulse
#define FIRST_PULSE_DURATION_MS 250    // First top pulse duration
#define SECOND_PULSE_DELAY_MS 250      // Delay after tray clears before second top pulse
#define SECOND_PULSE_DURATION_MS 500   // Second top pulse duration

#define INPUT_A_B_FILTER 20

#define baudRate 9600

#define HANDLE_ALERTS (1)
#define HANDLE_MOTOR_FAULTS (1)

// Motor wait timeouts (milliseconds)
#define BELT_VELOCITY_TIMEOUT_MS  5000
#define BLADE_HLFB_TIMEOUT_MS     5000

// Ethernet setup
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
IPAddress ip(192, 168, 10, 2);
IPAddress serverIp(192, 168, 10, 1);
#define PORT_NUM 8888
#define MAX_PACKET_LENGTH 100
unsigned char packetReceived[MAX_PACKET_LENGTH];
EthernetClient client;

int accelerationLimit = 100000;
int screen2Var1Value = 0;
int screen3Var1Value = 0;

//UDP Ethernet setup
EthernetUDP Udp;
unsigned int remotePort = 9999;
const uint16_t UDP_LOCAL_PORT = 9998;
const uint16_t STATUS_INTERVAL_MS = 1000;

struct TelemetryState {
    uint32_t bootId;
    uint32_t seq;
    uint32_t lastStatusMs;
    uint32_t lastRxCmdMs;
    uint32_t faultCountBelt;
    uint32_t faultCountBlade;
    uint32_t udpSendFailCount;
    bool     lastBeltFault;
    bool     lastBladeFault;
    bool     lastKillSwitchState;
    uint32_t beltMotorUptimeMs;
    uint32_t bladeMotorUptimeMs;
};
TelemetryState t;

// Linear rail configuration
#define STEPS_PER_MM 397
#define LINEAR_RAIL_MIN_POS -100
#define LINEAR_RAIL_MAX_POS -25001
#define LINEAR_RAIL_VELOCITY 1000
#define LINEAR_RAIL_ACCEL 2000

int32_t linearRailPositions[3] = {-15000, -20000, -24000};
int currentPositionIndex = 0;

// Variables from TCP server
int readyToRun = 0;
int activeVariety = -1;
int bladeSpeed = 0;
int beltSpeed = 0;
int bladeHeight = 0;
int airKnifeMode = 0;

// Auto-polling configuration
#define TCP_POLL_INTERVAL_MS 1000
unsigned long lastTcpPollTime = 0;
int lastBladeHeight = -1;
int lastBeltSpeed = -1;
int lastBladeSpeed = -1;

// Tracks whether the blade motor is actively spinning.
// Used to guard RampToVelocitySelection(0) calls — in CPM_MODE_A_DIRECT_B_DIRECT,
// HLFB does not assert when both inputs are false, so calling stop on an already-stopped
// motor causes the HLFB wait to hang/timeout.
bool bladeMotorRunning = false;

#define BELT_SPEED_SCALE 600

// Tray detection and solenoid state machine
enum TrayState {
    TRAY_IDLE,
    TRAY_BOT_SOLENOID,
    TRAY_WAIT_FIRST_PULSE,
    TRAY_FIRST_TOP_PULSE,
    TRAY_WAIT_TRAY_CLEAR,
    TRAY_WAIT_SECOND_PULSE,
    TRAY_SECOND_TOP_PULSE
};
TrayState trayState = TRAY_IDLE;
unsigned long trayStateStartTime = 0;
bool lastTrayDetected = false;
uint32_t trayCount = 0;

// Interrupt flags for limit switches
volatile bool lowerLimitHit = false;
volatile bool upperLimitHit = false;

#define DEBOUNCE_MS 200
volatile unsigned long lastLowerTrigger = 0;
volatile unsigned long lastUpperTrigger = 0;

bool MoveAtVelocity(int32_t velocity);
void PrintAlerts();
void HandleAlerts();
bool RampToVelocitySelection(int velocityIndex);
void HandleMotorFaults();
bool LinearRailMoveAbsolute(int32_t position);
void LinearRailCyclePositions();
void PrintLinearRailAlerts();
void HandleLinearRailAlerts();
bool ReadFromTCPServer();
void PrintTCPVariables();
void SendEvent(const char *eventCode, int32_t value);
void SendStatusUpdate();

void OnLowerLimitHit() {
    unsigned long now = millis();
    if (now - lastLowerTrigger > DEBOUNCE_MS) {
        lowerLimitHit = true;
        lastLowerTrigger = now;
    }
}

void OnUpperLimitHit() {
    unsigned long now = millis();
    if (now - lastUpperTrigger > DEBOUNCE_MS) {
        upperLimitHit = true;
        lastUpperTrigger = now;
    }
}

void setup() {

    pinMode(UpperLimitPin, INPUT);
    pinMode(LowerLimitPin, INPUT);
    pinMode(TrayDetectionPin, INPUT);
    pinMode(KillSwitchPin, INPUT);

    TopSolenoid.Mode(Connector::OUTPUT_DIGITAL);
    BotSolenoid.Mode(Connector::OUTPUT_DIGITAL);
    TopSolenoid.State(false);
    BotSolenoid.State(false);

    Serial.begin(baudRate);
    delay(2000);

    Serial.println("=== ClearCore Starting ===");
    Serial.println("Solenoids configured (IO1=Top, IO2=Bot)");
    Serial.println("Limit switch interrupts DISABLED");
    Serial.println("Setup complete. Entering loop...");

    ////////////////////////////////////////////////////////
    /////////////////// Linear Rail Motor Set Up (M2) //////
    ////////////////////////////////////////////////////////

    MotorMgr.MotorInputClocking(MotorManager::CLOCK_RATE_NORMAL);
    MotorMgr.MotorModeSet(MotorManager::MOTOR_M2M3, Connector::CPM_MODE_STEP_AND_DIR);
    LinearRailMotor.HlfbMode(MotorDriver::HLFB_MODE_HAS_BIPOLAR_PWM);
    LinearRailMotor.HlfbCarrier(MotorDriver::HLFB_CARRIER_482_HZ);
    LinearRailMotor.VelMax(LINEAR_RAIL_VELOCITY);
    LinearRailMotor.AccelMax(LINEAR_RAIL_ACCEL);
    LinearRailMotor.EnableRequest(true);
    Serial.println("LinearRailMotor Enabled");

    Serial.println("Waiting for LinearRailMotor HLFB...");
    uint32_t startTime = millis();
    while (LinearRailMotor.HlfbState() != MotorDriver::HLFB_ASSERTED &&
           !LinearRailMotor.StatusReg().bit.AlertsPresent &&
           millis() - startTime < 5000) {
        continue;
    }
    if (LinearRailMotor.StatusReg().bit.AlertsPresent) {
        Serial.println("LinearRailMotor alert detected.");
        PrintLinearRailAlerts();
        if (HANDLE_ALERTS) HandleLinearRailAlerts();
    } else {
        Serial.println("LinearRailMotor Ready");
    }

    Serial.println("");
    Serial.println("Enter a position in mm (e.g. 37 for 37mm from home):");

    ////////////////////////////////////////////////////////
    /////////////////// Belt Motor Set Up (M3) /////////////
    ////////////////////////////////////////////////////////

    BeltMotor.HlfbMode(MotorDriver::HLFB_MODE_HAS_BIPOLAR_PWM);
    BeltMotor.HlfbCarrier(MotorDriver::HLFB_CARRIER_482_HZ);
    BeltMotor.VelMax(10000);
    BeltMotor.AccelMax(20000);
    BeltMotor.EnableRequest(true);
    Serial.println("BeltMotor Enabled");

    Serial.println("Waiting for BeltMotor HLFB...");
    startTime = millis();
    while (BeltMotor.HlfbState() != MotorDriver::HLFB_ASSERTED &&
           !BeltMotor.StatusReg().bit.AlertsPresent &&
           millis() - startTime < 5000) {
        continue;
    }
    if (BeltMotor.StatusReg().bit.AlertsPresent) {
        Serial.println("BeltMotor alert detected.");
        PrintAlerts();
        if (HANDLE_ALERTS) HandleAlerts();
    } else {
        Serial.println("BeltMotor Ready");
    }

    ////////////////////////////////////////////////////////////////
    /////////////////// Blade Motor ////////////////////////////////
    ////////////////////////////////////////////////////////////////

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

    Ethernet.begin(mac, ip);
    Serial.println("Ethernet initialized");

    uint32_t ethTimeout = millis();
    while (Ethernet.linkStatus() == LinkOFF && millis() - ethTimeout < 5000) {
        Serial.println("Waiting for Ethernet link...");
        delay(1000);
    }

    if (Ethernet.linkStatus() == LinkON) {
        Serial.println("Ethernet link established");
    } else {
        Serial.println("Ethernet link timeout - continuing without connection");
    }

    ////////////////////////////////////////////////////////
    /////////////////// Telemetry Initialization ////////////
    ////////////////////////////////////////////////////////

    t.bootId            = millis() ^ 0x5A17C0DE;
    t.seq               = 0;
    t.lastStatusMs      = 0;
    t.lastRxCmdMs       = millis();
    t.faultCountBelt    = 0;
    t.faultCountBlade   = 0;
    t.udpSendFailCount  = 0;
    t.lastBeltFault     = false;
    t.lastBladeFault    = false;
    t.lastKillSwitchState = digitalRead(KillSwitchPin);
    t.beltMotorUptimeMs = 0;
    t.bladeMotorUptimeMs = 0;

    Udp.begin(UDP_LOCAL_PORT);
    Serial.println("UDP telemetry initialized");

    // -------------------------------------------------------
    // [DBG] Log initial state of all tracking variables
    // -------------------------------------------------------
    Serial.println("[DBG] === Initial State ===");
    Serial.print("[DBG] bladeMotorRunning="); Serial.println(bladeMotorRunning);
    Serial.print("[DBG] lastBladeHeight=");   Serial.println(lastBladeHeight);
    Serial.print("[DBG] lastBeltSpeed=");     Serial.println(lastBeltSpeed);
    Serial.print("[DBG] lastBladeSpeed=");    Serial.println(lastBladeSpeed);
    Serial.print("[DBG] killSwitch on boot=");Serial.println(digitalRead(KillSwitchPin) ? "ACTIVE" : "INACTIVE");
    Serial.println("[DBG] ======================");
}

void loop() {
    // Serial.print("I am at the top of the loop!");
    uint32_t currentTime = millis();
    bool beltFault  = BeltMotor.StatusReg().bit.AlertsPresent;
    bool bladeFault = BladeMotor.StatusReg().bit.MotorInFault;

    // Read kill switch state (HIGH = activated, motors can run)
    bool killSwitchActive = digitalRead(KillSwitchPin);
    // Detect kill switch state change
    if (killSwitchActive != t.lastKillSwitchState) {
        Serial.print("Kill switch changed: ");
        Serial.println(killSwitchActive ? "ACTIVE (motors enabled)" : "INACTIVE (motors stopped)");

        if (killSwitchActive) {
            // Kill switch activated - reset last speeds so motors will be commanded on next poll.
            // NOTE: lastBladeHeight is intentionally NOT reset here — the height hasn't changed
            // and resetting it would trigger a redundant (and potentially problematic) rail move.
            lastBeltSpeed = -1;
            lastBladeSpeed = -1;
            Serial.println("Motors ready - will apply current settings");

            // -------------------------------------------------------
            // [DBG] Log state at kill switch ON moment
            // -------------------------------------------------------
            Serial.print("[DBG] KS->ON: bladeMotorRunning="); Serial.println(bladeMotorRunning);
            Serial.print("[DBG] KS->ON: lastBladeHeight=");   Serial.println(lastBladeHeight);
            Serial.print("[DBG] KS->ON: bladeHeight(TCP)=");  Serial.println(bladeHeight);
            Serial.print("[DBG] KS->ON: heights match? ");    Serial.println((bladeHeight == lastBladeHeight) ? "YES - no rail move" : "NO - rail move will fire");

        } else {
            // Kill switch deactivated - stop belt and blade motors immediately.
            // -------------------------------------------------------
            // [DBG] Log state at kill switch OFF moment
            // -------------------------------------------------------
            Serial.print("[DBG] KS->OFF: bladeMotorRunning="); Serial.println(bladeMotorRunning);

            MoveAtVelocity(0);  // Stop belt — always safe

            // Guard blade stop: only call if blade is actually spinning.
            // RampToVelocitySelection(0) on an already-stopped blade in
            // CPM_MODE_A_DIRECT_B_DIRECT will hang on the HLFB wait.
            if (bladeMotorRunning) {
                Serial.println("[DBG] KS->OFF: blade was running, stopping it now");
                RampToVelocitySelection(0);
            } else {
                Serial.println("[DBG] KS->OFF: blade already stopped, skipping RampToVelocitySelection(0)");
            }

            Serial.println("Belt and blade motors stopped");
        }
        t.lastKillSwitchState = killSwitchActive;
    }

    // Tray detection and solenoid control state machine
    bool trayDetected = digitalRead(TrayDetectionPin);

    switch (trayState) {
        case TRAY_IDLE:
            if (trayDetected && !lastTrayDetected) {
              trayCount++;
              if (airKnifeMode > 0) {
                Serial.print("Tray switch: ACTIVE - airknife mode ");
                Serial.println(airKnifeMode);

                if (airKnifeMode == 1 || airKnifeMode == 3) {
                    BotSolenoid.State(true);
                    trayStateStartTime = millis();
                    trayState = TRAY_BOT_SOLENOID;
                    Serial.println("  -> Bottom solenoid ON");
                } else if (airKnifeMode == 2) {
                    trayStateStartTime = millis();
                    trayState = TRAY_WAIT_FIRST_PULSE;
                    Serial.println("  -> Waiting 1s for first top pulse");
                }
              }
            }
            break;

        case TRAY_BOT_SOLENOID:
            if (millis() - trayStateStartTime >= BOT_SOLENOID_DURATION_MS) {
                BotSolenoid.State(false);
                Serial.println("  -> Bottom solenoid OFF");
                if (airKnifeMode == 1) {
                    trayState = TRAY_WAIT_TRAY_CLEAR;
                    Serial.println("  -> Waiting for tray to clear");
                } else if (airKnifeMode == 3) {
                    trayStateStartTime = millis();
                    trayState = TRAY_WAIT_FIRST_PULSE;
                    Serial.println("  -> Waiting 1s for first top pulse");
                }
            }
            break;

        case TRAY_WAIT_FIRST_PULSE:
            if (millis() - trayStateStartTime >= FIRST_PULSE_DELAY_MS) {
                TopSolenoid.State(true);
                trayStateStartTime = millis();
                trayState = TRAY_FIRST_TOP_PULSE;
                Serial.println("  -> First top pulse ON (250ms)");
            }
            break;

        case TRAY_FIRST_TOP_PULSE:
            if (millis() - trayStateStartTime >= FIRST_PULSE_DURATION_MS) {
                TopSolenoid.State(false);
                trayState = TRAY_WAIT_TRAY_CLEAR;
                Serial.println("  -> First top pulse OFF, waiting for tray to clear");
            }
            break;

        case TRAY_WAIT_TRAY_CLEAR:
            if (!trayDetected && lastTrayDetected) {
                trayStateStartTime = millis();
                if (airKnifeMode == 1) {
                    trayState = TRAY_IDLE;
                    Serial.println("  -> Tray cleared, sequence complete");
                } else {
                    trayState = TRAY_WAIT_SECOND_PULSE;
                    Serial.println("  -> Tray cleared, waiting 500ms for second pulse");
                }
            }
            break;

        case TRAY_WAIT_SECOND_PULSE:
            if (millis() - trayStateStartTime >= SECOND_PULSE_DELAY_MS) {
                TopSolenoid.State(true);
                trayStateStartTime = millis();
                trayState = TRAY_SECOND_TOP_PULSE;
                Serial.println("  -> Second top pulse ON (500ms)");
            }
            break;

        case TRAY_SECOND_TOP_PULSE:
            if (millis() - trayStateStartTime >= SECOND_PULSE_DURATION_MS) {
                TopSolenoid.State(false);
                trayState = TRAY_IDLE;
                Serial.println("  -> Second top pulse OFF, sequence complete");
            }
            break;
    }
    lastTrayDetected = trayDetected;

    // Auto-poll TCP server at regular intervals
    if (millis() - lastTcpPollTime >= TCP_POLL_INTERVAL_MS) {
        lastTcpPollTime = millis();

        if (ReadFromTCPServer()) {
            t.lastRxCmdMs = currentTime;

            if (readyToRun) {
                // Check if blade height changed - pause motors during rail move
                if (bladeHeight != lastBladeHeight && bladeHeight >= 0) {

                    // -------------------------------------------------------
                    // [DBG] Log full state at the moment height change fires
                    // -------------------------------------------------------
                    Serial.print("[DBG] killSwitchActive="); Serial.println(killSwitchActive);
                    Serial.println("[DBG] === Height change block entered ===");
                    Serial.print("[DBG] bladeHeight(TCP)=");    Serial.println(bladeHeight);
                    Serial.print("[DBG] lastBladeHeight=");     Serial.println(lastBladeHeight);
                    Serial.print("[DBG] bladeMotorRunning=");   Serial.println(bladeMotorRunning);
                    Serial.print("[DBG] killSwitchActive=");    Serial.println(killSwitchActive);
                    Serial.print("[DBG] HLFB state (0=deasserted,1=asserted,2=has_measurement)=");
                    Serial.println((int)BladeMotor.HlfbState());
                    Serial.print("[DBG] MotorInFault=");        Serial.println(BladeMotor.StatusReg().bit.MotorInFault);
                    Serial.print("[DBG] InA=");                 Serial.print(BladeMotor.MotorInAState());
                    Serial.print(" InB=");                      Serial.println(BladeMotor.MotorInBState());

                    if (killSwitchActive) {
                        Serial.println("Pausing belt and blade for rail move...");
                        MoveAtVelocity(0);  // Stop belt — always safe

                        // -------------------------------------------------------
                        // [DBG] Log the guard decision
                        // -------------------------------------------------------
                        Serial.print("[DBG] Guard check: bladeMotorRunning="); Serial.println(bladeMotorRunning);
                        if (bladeMotorRunning) {
                            Serial.println("[DBG] Guard PASSED — calling RampToVelocitySelection(0)");
                            RampToVelocitySelection(0);
                        } else {
                            Serial.println("[DBG] Guard BLOCKED — blade already stopped, skipping RampToVelocitySelection(0)");
                        }
                    }

                    // Convert blade height (mm) to steps and move linear rail
                    int32_t targetPositionSteps = -bladeHeight * STEPS_PER_MM;

                    // -------------------------------------------------------
                    // [DBG] Log before rail move
                    // -------------------------------------------------------
                    Serial.print("[DBG] Starting rail move to ");
                    Serial.print(bladeHeight);
                    Serial.print(" mm = ");
                    Serial.print(targetPositionSteps);
                    Serial.println(" steps");

                    bool railMoveSuccess = LinearRailMoveAbsolute(targetPositionSteps);
                    killSwitchActive = digitalRead(KillSwitchPin);

                    // -------------------------------------------------------
                    // [DBG] Log rail move result
                    // -------------------------------------------------------
                    Serial.print("[DBG] Rail move result: ");
                    Serial.println(railMoveSuccess ? "SUCCESS" : "FAILED");

                    if (railMoveSuccess) {
                        lastBladeHeight = bladeHeight;
                    } else {
                        Serial.println("Rail move failed - will retry on next poll cycle.");
                    }

                    if (killSwitchActive) {
                        Serial.println("Rail move complete. Resuming belt and blade...");

                        int32_t beltVelocity = -beltSpeed * BELT_SPEED_SCALE;

                        // -------------------------------------------------------
                        // [DBG] Log resume values
                        // -------------------------------------------------------
                        Serial.print("[DBG] Resuming belt at velocity="); Serial.println(beltVelocity);
                        Serial.print("[DBG] Resuming blade at speed index="); Serial.println(bladeSpeed);

                        MoveAtVelocity(beltVelocity);
                        RampToVelocitySelection(bladeSpeed);
                        Serial.println("Motors resumed");
                    }

                    Serial.println("[DBG] === Height change block done ===");
                }

                // Only run belt and blade motors if kill switch is active
                if (killSwitchActive) {
                    if (beltSpeed != lastBeltSpeed) {
                        Serial.print("Belt speed changed: ");
                        Serial.print(lastBeltSpeed);
                        Serial.print(" -> ");
                        Serial.println(beltSpeed);

                        int32_t beltVelocity = -beltSpeed * BELT_SPEED_SCALE;
                        MoveAtVelocity(beltVelocity);
                        lastBeltSpeed = beltSpeed;
                    }

                    if (bladeSpeed != lastBladeSpeed) {
                        Serial.print("Blade speed changed: ");
                        Serial.print(lastBladeSpeed);
                        Serial.print(" -> ");
                        Serial.println(bladeSpeed);

                        RampToVelocitySelection(bladeSpeed);
                        lastBladeSpeed = bladeSpeed;
                    }
                }
            }
        }
    }

    // Check for serial input
    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n');
        input.trim();

        if (input.equalsIgnoreCase("tcp") || input.equalsIgnoreCase("read")) {
            Serial.println("Reading from TCP server...");
            if (ReadFromTCPServer()) {
                PrintTCPVariables();
            }
        } else if (input.equalsIgnoreCase("vars")) {
            PrintTCPVariables();
        } else if (input.equalsIgnoreCase("dbg")) {
            // -------------------------------------------------------
            // [DBG] On-demand state dump — type "dbg" in Serial Monitor
            // -------------------------------------------------------
            Serial.println("[DBG] === On-demand state dump ===");
            Serial.print("[DBG] bladeMotorRunning=");   Serial.println(bladeMotorRunning);
            Serial.print("[DBG] killSwitch=");          Serial.println(digitalRead(KillSwitchPin) ? "ACTIVE" : "INACTIVE");
            Serial.print("[DBG] bladeHeight(TCP)=");    Serial.println(bladeHeight);
            Serial.print("[DBG] lastBladeHeight=");     Serial.println(lastBladeHeight);
            Serial.print("[DBG] bladeSpeed(TCP)=");     Serial.println(bladeSpeed);
            Serial.print("[DBG] lastBladeSpeed=");      Serial.println(lastBladeSpeed);
            Serial.print("[DBG] beltSpeed(TCP)=");      Serial.println(beltSpeed);
            Serial.print("[DBG] lastBeltSpeed=");       Serial.println(lastBeltSpeed);
            Serial.print("[DBG] readyToRun=");          Serial.println(readyToRun);
            Serial.print("[DBG] HLFB state=");          Serial.println((int)BladeMotor.HlfbState());
            Serial.print("[DBG] MotorInFault=");        Serial.println(BladeMotor.StatusReg().bit.MotorInFault);
            Serial.print("[DBG] InA=");                 Serial.print(BladeMotor.MotorInAState());
            Serial.print(" InB=");                      Serial.println(BladeMotor.MotorInBState());
            Serial.println("[DBG] =================================");
        } else if (input.equalsIgnoreCase("help")) {
            Serial.println("Commands:");
            Serial.println("  tcp/read - Read from TCP server");
            Serial.println("  vars     - Print current TCP variables");
            Serial.println("  dbg      - Print full debug state dump");
            Serial.println("  b0-b3    - Set blade speed (0=off, 1-3=speeds)");
            Serial.println("  belt <n> - Set belt velocity (e.g. belt 500)");
            Serial.println("  rail <n> - Move linear rail to position in mm");
            Serial.print("  Valid rail range: 0 to ");
            Serial.print(abs(LINEAR_RAIL_MAX_POS) / STEPS_PER_MM);
            Serial.println(" mm");
        } else if (input.startsWith("b") && input.length() == 2) {
            int speed = input.substring(1).toInt();
            if (speed >= 0 && speed <= 3) {
                Serial.print("Setting blade speed to: ");
                Serial.println(speed);
                RampToVelocitySelection(speed);
            } else {
                Serial.println("Invalid blade speed. Use b0, b1, b2, or b3.");
            }
        } else if (input.startsWith("belt ")) {
            int32_t velocity = input.substring(5).toInt();
            Serial.print("Setting belt velocity to: ");
            Serial.println(velocity);
            MoveAtVelocity(velocity);
        } else if (input.startsWith("rail ")) {
            int32_t targetPositionMm = input.substring(5).toInt();
            int32_t targetPositionSteps = -targetPositionMm * STEPS_PER_MM;
            Serial.print("Moving rail to: ");
            Serial.print(targetPositionMm);
            Serial.print(" mm (");
            Serial.print(targetPositionSteps);
            Serial.println(" steps)");
            LinearRailMoveAbsolute(targetPositionSteps);
        } else {
            int32_t targetPositionMm = input.toInt();
            int32_t targetPositionSteps = -targetPositionMm * STEPS_PER_MM;
            Serial.print("Received position: ");
            Serial.print(targetPositionMm);
            Serial.print(" mm (");
            Serial.print(targetPositionSteps);
            Serial.println(" steps)");
            LinearRailMoveAbsolute(targetPositionSteps);
        }
    }

  if (currentTime - t.lastStatusMs >= STATUS_INTERVAL_MS) {
      t.lastStatusMs = currentTime;
      uint32_t udpStart = millis();
      SendStatusUpdate();
      uint32_t udpDuration = millis() - udpStart;
      if (udpDuration > 20) {
          Serial.print("[DBG] WARNING: SendStatusUpdate blocked for ");
          Serial.print(udpDuration);
          Serial.println("ms");
      }
  }
}

bool MoveAtVelocity(int32_t velocity) {
    if (BeltMotor.StatusReg().bit.AlertsPresent) {
        Serial.println("Motor alert detected.");
        PrintAlerts();
        if (HANDLE_ALERTS) {
            HandleAlerts();
        } else {
            Serial.println("Enable automatic alert handling by setting HANDLE_ALERTS to 1.");
        }
        Serial.println("Move canceled.");
        Serial.println();
        return false;
    }

    BeltMotor.MoveVelocity(velocity);

    if (velocity == 0) {
        t.beltMotorUptimeMs = 0;
    } else if (t.beltMotorUptimeMs == 0) {
        t.beltMotorUptimeMs = millis();
    }

    uint32_t timeout = millis();
    while (!BeltMotor.StatusReg().bit.AtTargetVelocity) {
        if (millis() - timeout > BELT_VELOCITY_TIMEOUT_MS) {
            Serial.println("ERROR: Belt motor timeout waiting for target velocity!");
            return false;
        }
    }

    return true;
}

bool RampToVelocitySelection(int velocityIndex) {
    // -------------------------------------------------------
    // [DBG] Log every entry into this function
    // -------------------------------------------------------
    Serial.print("[DBG] RampToVelocitySelection(");
    Serial.print(velocityIndex);
    Serial.print(") called | bladeMotorRunning=");
    Serial.print(bladeMotorRunning);
    Serial.print(" | HLFB=");
    Serial.print((int)BladeMotor.HlfbState());
    Serial.print(" | InA=");
    Serial.print(BladeMotor.MotorInAState());
    Serial.print(" InB=");
    Serial.println(BladeMotor.MotorInBState());

    if (BladeMotor.StatusReg().bit.MotorInFault) {
        if (HANDLE_MOTOR_FAULTS) {
            Serial.println("Motor fault detected. Move canceled.");
            HandleMotorFaults();
        } else {
            Serial.println("Motor fault detected. Move canceled. Enable automatic fault handling by setting HANDLE_MOTOR_FAULTS to 1.");
        }
        return false;
    }

    switch (velocityIndex) {
        case 0:
            BladeMotor.MotorInAState(false);
            BladeMotor.MotorInBState(false);
            t.bladeMotorUptimeMs = 0;
            bladeMotorRunning = false;  // Blade is now stopped
            Serial.println("Blade OFF (Inputs A Off/B Off)");
            break;
        case 1:
            BladeMotor.MotorInAState(true);
            BladeMotor.MotorInBState(false);
            if (t.bladeMotorUptimeMs == 0) t.bladeMotorUptimeMs = millis();
            bladeMotorRunning = true;   // Blade is now spinning
            Serial.println("Blade speed 1 (Inputs A On/B Off)");
            break;
        case 2:
            BladeMotor.MotorInAState(false);
            BladeMotor.MotorInBState(true);
            if (t.bladeMotorUptimeMs == 0) t.bladeMotorUptimeMs = millis();
            bladeMotorRunning = true;   // Blade is now spinning
            Serial.println("Blade speed 2 (Inputs A Off/B On)");
            break;
        case 3:
            BladeMotor.MotorInAState(true);
            BladeMotor.MotorInBState(true);
            if (t.bladeMotorUptimeMs == 0) t.bladeMotorUptimeMs = millis();
            bladeMotorRunning = true;   // Blade is now spinning
            Serial.println("Blade speed 3 (Inputs A On/B On)");
            break;
        default:
            Serial.print("[DBG] RampToVelocitySelection: invalid index ");
            Serial.println(velocityIndex);
            return false;
    }

    delay(20 + INPUT_A_B_FILTER);

    // -------------------------------------------------------
    // [DBG] Log HLFB state just before entering the wait loop.
    // If velocityIndex == 0 this should NOT be entered due to
    // caller guards, but log it anyway to catch unexpected calls.
    // -------------------------------------------------------
    Serial.print("[DBG] Pre-HLFB-wait: velocityIndex=");
    Serial.print(velocityIndex);
    Serial.print(" | HLFB=");
    Serial.print((int)BladeMotor.HlfbState());
    Serial.print(" | bladeMotorRunning=");
    Serial.println(bladeMotorRunning);

    if (velocityIndex == 0) {
        // HLFB does NOT assert when both inputs are false in CPM_MODE_A_DIRECT_B_DIRECT.
        // Skipping HLFB wait for stop command to avoid guaranteed timeout.
        Serial.println("[DBG] velocityIndex==0: skipping HLFB wait (would hang)");
        Serial.println("Blade stopped (no HLFB wait for off state)");
        return true;
    }

    Serial.println("Moving.. Waiting for HLFB");
    uint32_t timeout = millis();
    while (BladeMotor.HlfbState() != MotorDriver::HLFB_ASSERTED &&
           !BladeMotor.StatusReg().bit.MotorInFault) {
        if (millis() - timeout > BLADE_HLFB_TIMEOUT_MS) {
            // -------------------------------------------------------
            // [DBG] Log what we see at timeout — this is the key info
            // -------------------------------------------------------
            Serial.println("ERROR: Blade motor timeout waiting for HLFB!");
            Serial.print("[DBG] TIMEOUT: velocityIndex="); Serial.println(velocityIndex);
            Serial.print("[DBG] TIMEOUT: HLFB=");          Serial.println((int)BladeMotor.HlfbState());
            Serial.print("[DBG] TIMEOUT: MotorInFault=");  Serial.println(BladeMotor.StatusReg().bit.MotorInFault);
            Serial.print("[DBG] TIMEOUT: InA=");           Serial.print(BladeMotor.MotorInAState());
            Serial.print(" InB=");                         Serial.println(BladeMotor.MotorInBState());
            Serial.print("[DBG] TIMEOUT: bladeMotorRunning="); Serial.println(bladeMotorRunning);
            return false;
        }
    }

    if (BladeMotor.StatusReg().bit.MotorInFault) {
        Serial.println("Motor fault detected.");
        if (HANDLE_MOTOR_FAULTS) {
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
    Serial.println("Faults present. Cycling enable signal to motor to clear faults.");
    BeltMotor.EnableRequest(false);
    Delay_ms(10);
    BeltMotor.EnableRequest(true);
  }
  Serial.println("Clearing alerts.");
  BeltMotor.ClearAlerts();
  t.beltMotorUptimeMs = millis();
}

void HandleMotorFaults(){
  Serial.println("Handling fault: clearing faults by cycling enable signal to motor.");
  BladeMotor.EnableRequest(false);
  Delay_ms(10);
  BladeMotor.EnableRequest(true);
  Delay_ms(100);
  t.bladeMotorUptimeMs = millis();
}

//------------------------------------------------------------------------------
// Linear Rail Motor Functions
//------------------------------------------------------------------------------

bool LinearRailMoveAbsolute(int32_t position) {
    if (position > LINEAR_RAIL_MIN_POS) {
        Serial.print("WARNING: Position ");
        Serial.print(abs(position) / STEPS_PER_MM);
        Serial.print(" mm is below minimum. Clamping to ");
        Serial.print(abs(LINEAR_RAIL_MIN_POS) / STEPS_PER_MM);
        Serial.println(" mm.");
        position = LINEAR_RAIL_MIN_POS;
    }
    if (position < LINEAR_RAIL_MAX_POS) {
        Serial.print("WARNING: Position ");
        Serial.print(abs(position) / STEPS_PER_MM);
        Serial.print(" mm exceeds maximum. Clamping to ");
        Serial.print(abs(LINEAR_RAIL_MAX_POS) / STEPS_PER_MM);
        Serial.println(" mm.");
        position = LINEAR_RAIL_MAX_POS;
    }

    if (LinearRailMotor.StatusReg().bit.AlertsPresent) {
        Serial.println("LinearRailMotor alert detected.");
        PrintLinearRailAlerts();
        if (HANDLE_ALERTS) {
            HandleLinearRailAlerts();
        } else {
            Serial.println("Enable automatic alert handling by setting HANDLE_ALERTS to 1.");
        }
        Serial.println("Move canceled.");
        return false;
    }

    int32_t currentPos = LinearRailMotor.PositionRefCommanded();
    Serial.print("LinearRail current position: ");
    Serial.print(currentPos);
    Serial.print(" steps (");
    Serial.print(abs(currentPos) / STEPS_PER_MM);
    Serial.println(" mm)");

    Serial.print("LinearRail moving to position: ");
    Serial.print(position);
    Serial.print(" steps (");
    Serial.print(abs(position) / STEPS_PER_MM);
    Serial.println(" mm)");

    LinearRailMotor.Move(position, MotorDriver::MOVE_TARGET_ABSOLUTE);

    Serial.println("Moving.. Waiting for HLFB");
    while ((!LinearRailMotor.StepsComplete() ||
            LinearRailMotor.HlfbState() != MotorDriver::HLFB_ASSERTED) &&
           !LinearRailMotor.StatusReg().bit.AlertsPresent) {
        continue;
    }

    if (LinearRailMotor.StatusReg().bit.AlertsPresent) {
        Serial.println("LinearRailMotor alert detected during move.");
        PrintLinearRailAlerts();
        if (HANDLE_ALERTS) {
            HandleLinearRailAlerts();
        }
        Serial.println("Motion may not have completed as expected.");
        return false;
    }

    Serial.println("LinearRail Move Done");
    return true;
}

void LinearRailCyclePositions() {
    if (LinearRailMoveAbsolute(linearRailPositions[currentPositionIndex])) {
        currentPositionIndex = (currentPositionIndex + 1) % 3;
    }
}

void PrintLinearRailAlerts() {
    Serial.println("LinearRailMotor Alerts present: ");
    if (LinearRailMotor.AlertReg().bit.MotionCanceledInAlert) {
        Serial.println("    MotionCanceledInAlert ");
    }
    if (LinearRailMotor.AlertReg().bit.MotionCanceledPositiveLimit) {
        Serial.println("    MotionCanceledPositiveLimit ");
    }
    if (LinearRailMotor.AlertReg().bit.MotionCanceledNegativeLimit) {
        Serial.println("    MotionCanceledNegativeLimit ");
    }
    if (LinearRailMotor.AlertReg().bit.MotionCanceledSensorEStop) {
        Serial.println("    MotionCanceledSensorEStop ");
    }
    if (LinearRailMotor.AlertReg().bit.MotionCanceledMotorDisabled) {
        Serial.println("    MotionCanceledMotorDisabled ");
    }
    if (LinearRailMotor.AlertReg().bit.MotorFaulted) {
        Serial.println("    MotorFaulted ");
    }
}

void HandleLinearRailAlerts() {
    Serial.println("Cycling LinearRailMotor enable to clear alert state.");
    LinearRailMotor.EnableRequest(false);
    Delay_ms(50);
    LinearRailMotor.EnableRequest(true);
    Delay_ms(100);
    Serial.println("Clearing LinearRailMotor alerts.");
    LinearRailMotor.ClearAlerts();
}

//------------------------------------------------------------------------------
// TCP Communication Functions
//------------------------------------------------------------------------------

bool ReadFromTCPServer() {
    // Ensure any previous socket state is fully cleared
    // before attempting a new connection
    if (client.connected()) {
        client.stop();
    }
    delay(50);
    if (client.connect(serverIp, PORT_NUM)) {
        uint32_t timeout = millis();
        while (!client.available() && millis() - timeout < 2000) {
            delay(10);
        }

        if (client.available()) {
            int len = client.read(packetReceived, MAX_PACKET_LENGTH - 1);
            packetReceived[len] = '\0';

            char *token;
            char *packetCopy = (char *)packetReceived;

            token = strtok(packetCopy, ",");
            if (token != NULL) readyToRun = atoi(token);

            token = strtok(NULL, ",");
            if (token != NULL) activeVariety = atoi(token);

            token = strtok(NULL, ",");
            if (token != NULL) bladeSpeed = atoi(token);

            token = strtok(NULL, ",");
            if (token != NULL) beltSpeed = atoi(token);

            token = strtok(NULL, ",");
            if (token != NULL) bladeHeight = atoi(token);

            token = strtok(NULL, ",");
            if (token != NULL) airKnifeMode = atoi(token);

            Serial.print("TCP: airKnifeMode = ");
            Serial.println(airKnifeMode);

            client.stop();
            return true;
        }

        client.stop();
        Serial.println("TCP: No data received");
        return false;
    } else {
        Serial.println("TCP: Connection failed");
        return false;
    }
}

void PrintTCPVariables() {
    Serial.println("--- TCP Variables ---");
    Serial.print("  readyToRun: ");   Serial.println(readyToRun);
    Serial.print("  activeVariety: ");Serial.println(activeVariety);
    Serial.print("  bladeSpeed: ");   Serial.println(bladeSpeed);
    Serial.print("  beltSpeed: ");    Serial.println(beltSpeed);
    Serial.print("  bladeHeight: ");  Serial.println(bladeHeight);
    Serial.print("  airKnifeMode: "); Serial.println(airKnifeMode);
    Serial.println("---------------------");
}

//------------------------------------------------------------------------------
// Telemetry Helper Functions
//------------------------------------------------------------------------------

static uint16_t EncodeBeltAlerts() {
    uint16_t bits = 0;
    bits |= BeltMotor.AlertReg().bit.MotionCanceledInAlert       ? (1 << 0) : 0;
    bits |= BeltMotor.AlertReg().bit.MotionCanceledPositiveLimit ? (1 << 1) : 0;
    bits |= BeltMotor.AlertReg().bit.MotionCanceledNegativeLimit ? (1 << 2) : 0;
    bits |= BeltMotor.AlertReg().bit.MotionCanceledSensorEStop   ? (1 << 3) : 0;
    bits |= BeltMotor.AlertReg().bit.MotionCanceledMotorDisabled ? (1 << 4) : 0;
    bits |= BeltMotor.AlertReg().bit.MotorFaulted                ? (1 << 5) : 0;
    return bits;
}

static int16_t ReadTorquePctOrUnknown() {
    if (BeltMotor.HlfbState() == MotorDriver::HLFB_HAS_MEASUREMENT) {
        return (int16_t)BeltMotor.HlfbPercent();
    }
    return -1;
}

void SendEvent(const char *eventCode, int32_t value) {
  if (Ethernet.linkStatus() != LinkON) {
        return;
    }
    char telemetryBuffer[256];
    uint32_t uptimeMs = millis();
    bool beltFault  = BeltMotor.StatusReg().bit.AlertsPresent;
    bool bladeFault = BladeMotor.StatusReg().bit.MotorInFault;
    uint16_t alertBits = EncodeBeltAlerts();
    int killSwitch = digitalRead(KillSwitchPin) ? 1 : 0;
    uint32_t cmdAgeMs = uptimeMs - t.lastRxCmdMs;
    uint32_t seq = ++t.seq;

    uint32_t beltUptime  = t.beltMotorUptimeMs  ? (uptimeMs - t.beltMotorUptimeMs)  : 0;
    uint32_t bladeUptime = t.bladeMotorUptimeMs ? (uptimeMs - t.bladeMotorUptimeMs) : 0;

    snprintf(telemetryBuffer, sizeof(telemetryBuffer),
             "EVENT,1,%lu,%lu,%lu,%s,%ld,%lu,%lu,%d,%d,%u,%d,%lu,%lu",
             (unsigned long)t.bootId,
             (unsigned long)seq,
             (unsigned long)uptimeMs,
             eventCode,
             (long)value,
             (unsigned long)beltUptime,
             (unsigned long)bladeUptime,
             beltFault ? 1 : 0,
             bladeFault ? 1 : 0,
             (unsigned int)alertBits,
             killSwitch,
             (unsigned long)cmdAgeMs,
             (unsigned long)t.udpSendFailCount);

    Udp.beginPacket(serverIp, remotePort);
    Udp.write((const uint8_t *)telemetryBuffer, strlen(telemetryBuffer));
    if (!Udp.endPacket()) {
        t.udpSendFailCount++;
    }
}

void SendStatusUpdate() {

  if (Ethernet.linkStatus() != LinkON) {
        return;
    }
    char telemetryBuffer[256];
    uint32_t uptimeMs = millis();
    bool beltFault  = BeltMotor.StatusReg().bit.AlertsPresent;
    bool bladeFault = BladeMotor.StatusReg().bit.MotorInFault;
    uint16_t alertBits = EncodeBeltAlerts();
    int16_t torquePct  = ReadTorquePctOrUnknown();
    int killSwitch = digitalRead(KillSwitchPin) ? 1 : 0;
    uint32_t cmdAgeMs = uptimeMs - t.lastRxCmdMs;
    uint32_t seq = ++t.seq;

    uint32_t beltUptime  = t.beltMotorUptimeMs  ? (uptimeMs - t.beltMotorUptimeMs)  : 0;
    uint32_t bladeUptime = t.bladeMotorUptimeMs ? (uptimeMs - t.bladeMotorUptimeMs) : 0;

    snprintf(telemetryBuffer, sizeof(telemetryBuffer),
             "STATUS_UPDATE,1,%lu,%lu,%lu,%lu,%lu,%d,%d,%d,%u,%d,%lu,%lu,%lu",
             (unsigned long)t.bootId,
             (unsigned long)seq,
             (unsigned long)uptimeMs,
             (unsigned long)beltUptime,
             (unsigned long)bladeUptime,
             (int)torquePct,
             beltFault ? 1 : 0,
             bladeFault ? 1 : 0,
             (unsigned int)alertBits,
             killSwitch,
             (unsigned long)cmdAgeMs,
             (unsigned long)t.udpSendFailCount,
             (unsigned long)trayCount);

    Udp.beginPacket(serverIp, remotePort);
    Udp.write((const uint8_t *)telemetryBuffer, strlen(telemetryBuffer));
    if (!Udp.endPacket()) {
        t.udpSendFailCount++;
    }
}
