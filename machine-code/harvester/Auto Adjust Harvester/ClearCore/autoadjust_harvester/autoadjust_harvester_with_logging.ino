
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
#define STEPS_PER_MM 397             // Steps per millimeter of linear rail travel
#define LINEAR_RAIL_MIN_POS -100     // Minimum position (counts) ~-0.25mm
#define LINEAR_RAIL_MAX_POS -25001   // Maximum position (counts) ~-63mm
#define LINEAR_RAIL_VELOCITY 1000    // pulses per sec (10% of example)
#define LINEAR_RAIL_ACCEL 2000       // pulses per sec^2 (20% of original)

// Three absolute positions for the linear rail cycle (in steps)
int32_t linearRailPositions[3] = {-15000, -20000, -24000};  // Position 1, 2, 3
int currentPositionIndex = 0;  // Track which position we're moving to

// Variables from TCP server (CSV format: ready_to_run,active_variety,blade_speed,belt_speed,blade_height,airknife_mode)
int readyToRun = 0;
int activeVariety = -1;
int bladeSpeed = 0;
int beltSpeed = 0;
int bladeHeight = 0;
int airKnifeMode = 0;

// Auto-polling configuration
#define TCP_POLL_INTERVAL_MS 1000  // Poll TCP server every 1 second
unsigned long lastTcpPollTime = 0;
int lastBladeHeight = -1;  // Track previous blade height to detect changes
int lastBeltSpeed = -1;    // Track previous belt speed to detect changes
int lastBladeSpeed = -1;   // Track previous blade speed to detect changes
// lastKillSwitchState is now tracked in TelemetryState t

// Belt speed scaling factor (converts beltSpeed value to pulses/sec)
#define BELT_SPEED_SCALE 600  // Adjust this to tune belt speed

// Tray detection and solenoid state machine
enum TrayState {
    TRAY_IDLE,              // Waiting for tray detection
    TRAY_BOT_SOLENOID,      // Bot solenoid is on (for 500ms)
    TRAY_WAIT_FIRST_PULSE,  // Waiting 1s after bottom off before first top pulse
    TRAY_FIRST_TOP_PULSE,   // First top pulse (250ms)
    TRAY_WAIT_TRAY_CLEAR,   // Waiting for tray to clear
    TRAY_WAIT_SECOND_PULSE, // Waiting 500ms after tray clears before second pulse
    TRAY_SECOND_TOP_PULSE   // Second top pulse (500ms)
};
TrayState trayState = TRAY_IDLE;
unsigned long trayStateStartTime = 0;
bool lastTrayDetected = false;          // Track tray state for edge detection
uint32_t trayCount = 0;                 // Running total of trays detected this session

// Interrupt flags for limit switches
volatile bool lowerLimitHit = false;
volatile bool upperLimitHit = false;

// Debounce timing (milliseconds)
#define DEBOUNCE_MS 200
volatile unsigned long lastLowerTrigger = 0;
volatile unsigned long lastUpperTrigger = 0;

bool MoveAtVelocity(int32_t velocity);
void PrintAlerts();
void HandleAlerts();

bool RampToVelocitySelection(int velocityIndex);
void HandleMotorFaults();

// Linear rail movement functions
bool LinearRailMoveAbsolute(int32_t position);
void LinearRailCyclePositions();
void PrintLinearRailAlerts();
void HandleLinearRailAlerts();

// TCP communication functions
bool ReadFromTCPServer();
void PrintTCPVariables();

// Telemetry functions
void SendEvent(const char *eventCode, int32_t value);
void SendStatusUpdate();

// ISR callback for lower limit switch (with debounce)
void OnLowerLimitHit() {
    unsigned long now = millis();
    if (now - lastLowerTrigger > DEBOUNCE_MS) {
        lowerLimitHit = true;
        lastLowerTrigger = now;
    }
}

// ISR callback for upper limit switch (with debounce)
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

    // Configure solenoid outputs (ClearCore IO connectors)
    TopSolenoid.Mode(Connector::OUTPUT_DIGITAL);
    BotSolenoid.Mode(Connector::OUTPUT_DIGITAL);
    TopSolenoid.State(false);
    BotSolenoid.State(false);

    Serial.begin(baudRate);
    delay(2000); // Give time for Serial to initialize

    Serial.println("=== ClearCore Starting ===");
    Serial.println("Solenoids configured (IO1=Top, IO2=Bot)");

    // Attach interrupts for limit switches (RISING = LOW -> HIGH transition)
    // attachInterrupt(digitalPinToInterrupt(LowerLimitPin), OnLowerLimitHit, RISING);
    // attachInterrupt(digitalPinToInterrupt(UpperLimitPin), OnUpperLimitHit, RISING);
    // Serial.println("Interrupts attached for limit switches on DI7 and DI8");
    Serial.println("Limit switch interrupts DISABLED");

    Serial.println("Setup complete. Entering loop...");

    ////////////////////////////////////////////////////////
    /////////////////// Linear Rail Motor Set Up (M2) //////
    ////////////////////////////////////////////////////////

    // Set input clocking rate for step and direction
    MotorMgr.MotorInputClocking(MotorManager::CLOCK_RATE_NORMAL);

    // Set M2 to step and direction mode
    MotorMgr.MotorModeSet(MotorManager::MOTOR_M2M3, Connector::CPM_MODE_STEP_AND_DIR);

    // Configure HLFB for position feedback
    LinearRailMotor.HlfbMode(MotorDriver::HLFB_MODE_HAS_BIPOLAR_PWM);
    LinearRailMotor.HlfbCarrier(MotorDriver::HLFB_CARRIER_482_HZ);

    // Set velocity and acceleration limits
    LinearRailMotor.VelMax(LINEAR_RAIL_VELOCITY);
    LinearRailMotor.AccelMax(LINEAR_RAIL_ACCEL);

    // Enable the motor
    LinearRailMotor.EnableRequest(true);
    Serial.println("LinearRailMotor Enabled");

    // Wait for HLFB to assert (homing complete if applicable)
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

    // M3 is already configured as step and direction (part of M2M3 pair)
    // Configure HLFB for velocity feedback
    BeltMotor.HlfbMode(MotorDriver::HLFB_MODE_HAS_BIPOLAR_PWM);
    BeltMotor.HlfbCarrier(MotorDriver::HLFB_CARRIER_482_HZ);

    // Set velocity and acceleration limits for belt motor
    BeltMotor.VelMax(10000);      // pulses per sec
    BeltMotor.AccelMax(20000);    // pulses per sec^2 (20% of original)

    // Enable the belt motor
    BeltMotor.EnableRequest(true);
    Serial.println("BeltMotor Enabled");

    // Wait for HLFB to assert
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

    Ethernet.begin(mac, ip); // Set static IP
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
    t.beltMotorUptimeMs = 0;        // belt starts enabled but not moving
    t.bladeMotorUptimeMs = 0;       // blade starts stopped

    Udp.begin(UDP_LOCAL_PORT);
    Serial.println("UDP telemetry initialized");
}

void loop() {
    // Handle limit switch interrupts (DISABLED)
    // if (lowerLimitHit) {
    //     lowerLimitHit = false;
    //     Serial.println(">>> [INTERRUPT] Stopping linear rail and going up <<<");
    // }
    // if (upperLimitHit) {
    //     upperLimitHit = false;
    //     Serial.println(">>> [INTERRUPT] Stopping linear rail and going down <<<");
    // }

    // Telemetry: sample fault states for edge detection
    uint32_t currentTime = millis();
    bool beltFault  = BeltMotor.StatusReg().bit.AlertsPresent;
    bool bladeFault = BladeMotor.StatusReg().bit.MotorInFault;

    // Belt fault edge detection
    if (beltFault && !t.lastBeltFault) {
        t.faultCountBelt++;
        if (BeltMotor.AlertReg().bit.MotorFaulted) { 
          SendEvent("FAULT_BELT_MOTOR_FAULTED", (int32_t)t.faultCountBelt); 
        }
        else if (BeltMotor.AlertReg().bit.MotionCanceledPositiveLimit) { 
          SendEvent("FAULT_BELT_POS_LIMIT", (int32_t)t.faultCountBelt);
        }
        else if (BeltMotor.AlertReg().bit.MotionCanceledNegativeLimit) { 
          SendEvent("FAULT_BELT_NEG_LIMIT", (int32_t)t.faultCountBelt);
        }
        else if (BeltMotor.AlertReg().bit.MotionCanceledSensorEStop) { 
          SendEvent("FAULT_BELT_ESTOP", (int32_t)t.faultCountBelt);
        }
        else if (BeltMotor.AlertReg().bit.MotionCanceledMotorDisabled) { 
          SendEvent("FAULT_BELT_DISABLED", (int32_t)t.faultCountBlade);
        }
        else { 
          SendEvent("FAULT_BELT_ALERT", (int32_t)t.faultCountBelt); 
        }
        t.lastBeltFault = beltFault;
    }

    // Blade fault edge detection
    if (bladeFault != t.lastBladeFault) {
        if (bladeFault) {
            t.faultCountBlade++;
            SendEvent("FAULT_BLADE_RAISED", (int32_t)t.faultCountBlade);
        } else {
            SendEvent("FAULT_BLADE_CLEARED", (int32_t)t.faultCountBlade);
        }
        t.lastBladeFault = bladeFault;
    }

    // Read kill switch state (HIGH = activated, motors can run)
    bool killSwitchActive = digitalRead(KillSwitchPin);

    // Detect kill switch state change
    if (killSwitchActive != t.lastKillSwitchState) {
        Serial.print("Kill switch changed: ");
        Serial.println(killSwitchActive ? "ACTIVE (motors enabled)" : "INACTIVE (motors stopped)");
        SendEvent("KILL_SWITCH_CHANGED", killSwitchActive ? 1 : 0);

        if (killSwitchActive) {
            // Kill switch activated - reset last values so motors will be commanded
            lastBeltSpeed = -1;
            lastBladeSpeed = -1;
            Serial.println("Motors ready - will apply current settings");
        } else {
            // Kill switch deactivated - stop belt and blade motors immediately
            MoveAtVelocity(0);  // Stop belt motor
            RampToVelocitySelection(0);  // Stop blade motor (velocity selection 0 = off)
            Serial.println("Belt and blade motors stopped");
        }
        t.lastKillSwitchState = killSwitchActive;
    }

    // Tray detection and solenoid control state machine
    // airKnifeMode: 0=Off, 1=Bottom only, 2=Top only, 3=Both (full sequence)
    // Sequence: tray -> bot on 500ms -> wait 1s -> top pulse 250ms -> wait for tray clear -> wait 500ms -> top pulse 500ms
    bool trayDetected = digitalRead(TrayDetectionPin);

    // State machine for solenoid control
    switch (trayState) {
        case TRAY_IDLE:
            // Waiting for tray to be detected
            if (trayDetected && !lastTrayDetected) {
              trayCount++;
              if (airKnifeMode > 0) {
                Serial.print("Tray switch: ACTIVE - airknife mode ");
                Serial.println(airKnifeMode);
                SendEvent("TRAYSWITCH_ACTIVATED", 0);

                if (airKnifeMode == 1 || airKnifeMode == 3) {
                    // Mode 1 or 3: Start with bottom solenoid on
                    BotSolenoid.State(true);
                    trayStateStartTime = millis();
                    trayState = TRAY_BOT_SOLENOID;
                    Serial.println("  -> Bottom solenoid ON");
                } else if (airKnifeMode == 2) {
                    // Mode 2: Skip bottom, wait 1s then first top pulse
                    trayStateStartTime = millis();
                    trayState = TRAY_WAIT_FIRST_PULSE;
                    Serial.println("  -> Waiting 1s for first top pulse");
                }
              }
            }
            break;

        case TRAY_BOT_SOLENOID:
            // Bottom solenoid is on for 500ms
            if (millis() - trayStateStartTime >= BOT_SOLENOID_DURATION_MS) {
                BotSolenoid.State(false);
                Serial.println("  -> Bottom solenoid OFF");

                if (airKnifeMode == 1) {
                    // Mode 1: Only bottom, wait for tray to clear then done
                    trayState = TRAY_WAIT_TRAY_CLEAR;
                    Serial.println("  -> Waiting for tray to clear");
                } else if (airKnifeMode == 3) {
                    // Mode 3: Wait 1s before first top pulse
                    trayStateStartTime = millis();
                    trayState = TRAY_WAIT_FIRST_PULSE;
                    Serial.println("  -> Waiting 1s for first top pulse");
                }
            }
            break;

        case TRAY_WAIT_FIRST_PULSE:
            // Waiting 1 second after bottom off before first top pulse
            if (millis() - trayStateStartTime >= FIRST_PULSE_DELAY_MS) {
                TopSolenoid.State(true);
                trayStateStartTime = millis();
                trayState = TRAY_FIRST_TOP_PULSE;
                Serial.println("  -> First top pulse ON (250ms)");
            }
            break;

        case TRAY_FIRST_TOP_PULSE:
            // First top pulse for 250ms
            if (millis() - trayStateStartTime >= FIRST_PULSE_DURATION_MS) {
                TopSolenoid.State(false);
                trayState = TRAY_WAIT_TRAY_CLEAR;
                Serial.println("  -> First top pulse OFF, waiting for tray to clear");
            }
            break;

        case TRAY_WAIT_TRAY_CLEAR:
            // Waiting for tray to clear
            if (!trayDetected && lastTrayDetected) {
                trayStateStartTime = millis();
                if (airKnifeMode == 1) {
                    // Mode 1: Bottom only, sequence complete
                    trayState = TRAY_IDLE;
                    Serial.println("  -> Tray cleared, sequence complete");
                } else {
                    // Mode 2 or 3: Wait 500ms then second top pulse
                    trayState = TRAY_WAIT_SECOND_PULSE;
                    Serial.println("  -> Tray cleared, waiting 500ms for second pulse");
                }
            }
            break;

        case TRAY_WAIT_SECOND_PULSE:
            // Waiting 500ms after tray clears before second pulse
            if (millis() - trayStateStartTime >= SECOND_PULSE_DELAY_MS) {
                TopSolenoid.State(true);
                trayStateStartTime = millis();
                trayState = TRAY_SECOND_TOP_PULSE;
                Serial.println("  -> Second top pulse ON (500ms)");
            }
            break;

        case TRAY_SECOND_TOP_PULSE:
            // Second top pulse for 500ms
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
            // Only act when ready to run
            if (readyToRun) {
                // Check if blade height changed - pause motors during rail move
                if (bladeHeight != lastBladeHeight && bladeHeight >= 0) {
                    Serial.print("Blade height changed: ");
                    Serial.print(lastBladeHeight);
                    Serial.print(" -> ");
                    Serial.println(bladeHeight);

                    // Pause belt and blade motors before moving rail
                    if (killSwitchActive) {
                        Serial.println("Pausing belt and blade for rail move...");
                        MoveAtVelocity(0);  // Stop belt
                        RampToVelocitySelection(0);  // Stop blade
                    }

                    // Convert blade height (mm) to steps and move linear rail
                    int32_t targetPositionSteps = -bladeHeight * STEPS_PER_MM;
                    LinearRailMoveAbsolute(targetPositionSteps);

                    lastBladeHeight = bladeHeight;

                    // Resume belt and blade motors after rail move (if kill switch active)
                    if (killSwitchActive) {
                        Serial.println("Rail move complete. Resuming belt and blade...");
                        // Re-apply belt speed
                        int32_t beltVelocity = -beltSpeed * BELT_SPEED_SCALE;
                        MoveAtVelocity(beltVelocity);
                        // Re-apply blade speed
                        RampToVelocitySelection(bladeSpeed);
                        Serial.println("Motors resumed");
                    }
                }

                // Only run belt and blade motors if kill switch is active
                if (killSwitchActive) {
                    // Check if belt speed changed
                    if (beltSpeed != lastBeltSpeed) {
                        Serial.print("Belt speed changed: ");
                        Serial.print(lastBeltSpeed);
                        Serial.print(" -> ");
                        Serial.println(beltSpeed);

                        // Convert belt speed value to velocity (pulses/sec)
                        // Negative to reverse direction
                        int32_t beltVelocity = -beltSpeed * BELT_SPEED_SCALE;
                        MoveAtVelocity(beltVelocity);

                        lastBeltSpeed = beltSpeed;
                    }

                    // Check if blade speed changed
                    if (bladeSpeed != lastBladeSpeed) {
                        Serial.print("Blade speed changed: ");
                        Serial.print(lastBladeSpeed);
                        Serial.print(" -> ");
                        Serial.println(bladeSpeed);

                        // Use blade speed as velocity selection (0-3)
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

        // Check for commands
        if (input.equalsIgnoreCase("tcp") || input.equalsIgnoreCase("read")) {
            // Read from TCP server
            Serial.println("Reading from TCP server...");
            if (ReadFromTCPServer()) {
                PrintTCPVariables();
            }
        } else if (input.equalsIgnoreCase("vars")) {
            // Print current variables
            PrintTCPVariables();
        } else if (input.equalsIgnoreCase("help")) {
            Serial.println("Commands:");
            Serial.println("  tcp/read - Read from TCP server");
            Serial.println("  vars - Print current TCP variables");
            Serial.println("  b0-b3 - Set blade speed (0=off, 1-3=speeds)");
            Serial.println("  belt <num> - Set belt velocity (e.g. belt 500)");
            Serial.println("  rail <num> - Move linear rail to position in mm");
            Serial.print("  Valid rail range: 0 to ");
            Serial.print(abs(LINEAR_RAIL_MAX_POS) / STEPS_PER_MM);
            Serial.println(" mm");
        } else if (input.startsWith("b") && input.length() == 2) {
            // Blade speed command: b0, b1, b2, b3
            int speed = input.substring(1).toInt();
            if (speed >= 0 && speed <= 3) {
                Serial.print("Setting blade speed to: ");
                Serial.println(speed);
                RampToVelocitySelection(speed);
            } else {
                Serial.println("Invalid blade speed. Use b0, b1, b2, or b3.");
            }
        } else if (input.startsWith("belt ")) {
            // Belt velocity command: belt <velocity>
            int32_t velocity = input.substring(5).toInt();
            Serial.print("Setting belt velocity to: ");
            Serial.println(velocity);
            MoveAtVelocity(velocity);
        } else if (input.startsWith("rail ")) {
            // Linear rail command: rail <mm>
            int32_t targetPositionMm = input.substring(5).toInt();
            int32_t targetPositionSteps = -targetPositionMm * STEPS_PER_MM;

            Serial.print("Moving rail to: ");
            Serial.print(targetPositionMm);
            Serial.print(" mm (");
            Serial.print(targetPositionSteps);
            Serial.println(" steps)");

            LinearRailMoveAbsolute(targetPositionSteps);
        } else {
            // Assume it's a position in mm for backwards compatibility
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

    // Periodic telemetry status update
    if (currentTime - t.lastStatusMs >= STATUS_INTERVAL_MS) {
        t.lastStatusMs = currentTime;
        SendStatusUpdate();
    }
}

bool MoveAtVelocity(int32_t velocity) {
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

    // Track uptime: 0 = stopped, non-zero = start timestamp
    if (velocity == 0) {
        t.beltMotorUptimeMs = 0;
    } else if (t.beltMotorUptimeMs == 0) {
        t.beltMotorUptimeMs = millis();
    }

    // Waits for the step command to ramp up/down to the commanded velocity.
    // This time will depend on your Acceleration Limit.
    while (!BeltMotor.StatusReg().bit.AtTargetVelocity) {
        continue;
    }

    SendEvent("BELT_AT_TARGET_VELOCITY", 0);

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
        case 0:
            // Sets Input A and B for velocity 0 (OFF)
            BladeMotor.MotorInAState(false);
            BladeMotor.MotorInBState(false);
            t.bladeMotorUptimeMs = 0;
            Serial.println(" (Inputs A Off/B Off) - OFF");
            break;
        case 1:
            // Sets Input A and B for velocity 1
            BladeMotor.MotorInAState(true);
            BladeMotor.MotorInBState(false);
            if (t.bladeMotorUptimeMs == 0) t.bladeMotorUptimeMs = millis();
            Serial.println(" (Inputs A On/B Off)");
            break;
        case 2:
            // Sets Input A and B for velocity 2
            BladeMotor.MotorInAState(false);
            BladeMotor.MotorInBState(true);
            if (t.bladeMotorUptimeMs == 0) t.bladeMotorUptimeMs = millis();
            Serial.println(" (Inputs A Off/B On)");
            break;
        case 3:
            // Sets Input A and B for velocity 3
            BladeMotor.MotorInAState(true);
            BladeMotor.MotorInBState(true);
            if (t.bladeMotorUptimeMs == 0) t.bladeMotorUptimeMs = millis();
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

/*
 * LinearRailMoveAbsolute
 *    Command the linear rail motor to move to an absolute position
 *    Returns when HLFB asserts (motor has reached position)
 */
bool LinearRailMoveAbsolute(int32_t position) {
    // Clamp position to valid range with warnings
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

    // Check if a motor alert is currently preventing motion
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

    // Command the absolute position move
    LinearRailMotor.Move(position, MotorDriver::MOVE_TARGET_ABSOLUTE);

    // Wait for HLFB to assert (move complete)
    Serial.println("Moving.. Waiting for HLFB");
    while ((!LinearRailMotor.StepsComplete() ||
            LinearRailMotor.HlfbState() != MotorDriver::HLFB_ASSERTED) &&
           !LinearRailMotor.StatusReg().bit.AlertsPresent) {

        // Check for limit switch interrupts during move (DISABLED)
        // if (lowerLimitHit || upperLimitHit) {
        //     LinearRailMotor.MoveStopAbrupt();
        //     Serial.println("Move stopped by limit switch!");
        //     return false;
        // }
        continue;
    }

    // Check if motor alert occurred during move
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

/*
 * LinearRailCyclePositions
 *    Cycles through the three defined positions
 *    Call this repeatedly in loop() to continuously cycle
 */
void LinearRailCyclePositions() {
    // Move to the current position in the cycle
    if (LinearRailMoveAbsolute(linearRailPositions[currentPositionIndex])) {
        // Move succeeded, advance to next position
        currentPositionIndex = (currentPositionIndex + 1) % 3;
    }
}

/*
 * PrintLinearRailAlerts
 *    Prints active alerts for the linear rail motor
 */
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

/*
 * HandleLinearRailAlerts
 *    Clears alerts for the linear rail motor
 */
void HandleLinearRailAlerts() {
    if (LinearRailMotor.AlertReg().bit.MotorFaulted) {
        Serial.println("LinearRailMotor faults present. Cycling enable to clear.");
        LinearRailMotor.EnableRequest(false);
        Delay_ms(10);
        LinearRailMotor.EnableRequest(true);
    }
    Serial.println("Clearing LinearRailMotor alerts.");
    LinearRailMotor.ClearAlerts();
}

//------------------------------------------------------------------------------
// TCP Communication Functions
//------------------------------------------------------------------------------

/*
 * ReadFromTCPServer
 *    Connects to the Raspberry Pi TCP server and reads the CSV data
 *    CSV format: ready_to_run,active_variety,blade_speed,belt_speed,blade_height,airknife_mode
 *    Returns true if successful, false otherwise
 */
bool ReadFromTCPServer() {
    if (client.connect(serverIp, PORT_NUM)) {
        // Wait for data with timeout
        uint32_t timeout = millis();
        while (!client.available() && millis() - timeout < 2000) {
            delay(10);
        }

        if (client.available()) {
            int len = client.read(packetReceived, MAX_PACKET_LENGTH - 1);
            packetReceived[len] = '\0';

            // Parse CSV: ready_to_run,active_variety,blade_speed,belt_speed,blade_height,airknife_mode
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

            // Print labeled values (COMMENTED OUT FOR TESTING)
            // Serial.print("TCP: ready=");
            // Serial.print(readyToRun);
            // Serial.print(" variety=");
            // Serial.print(activeVariety);
            // Serial.print(" blade=");
            // Serial.print(bladeSpeed);
            // Serial.print(" belt=");
            // Serial.print(beltSpeed);
            // Serial.print(" height=");
            // Serial.print(bladeHeight);
            // Serial.print(" airknife=");
            // Serial.print(airKnifeMode);
            // Serial.print(" motors=");
            // Serial.println(digitalRead(KillSwitchPin) ? "ENABLED" : "DISABLED");

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

/*
 * PrintTCPVariables
 *    Prints all the variables received from the TCP server
 */
void PrintTCPVariables() {
    Serial.println("--- TCP Variables ---");
    Serial.print("  readyToRun: ");
    Serial.println(readyToRun);
    Serial.print("  activeVariety: ");
    Serial.println(activeVariety);
    Serial.print("  bladeSpeed: ");
    Serial.println(bladeSpeed);
    Serial.print("  beltSpeed: ");
    Serial.println(beltSpeed);
    Serial.print("  bladeHeight: ");
    Serial.println(bladeHeight);
    Serial.print("  airKnifeMode: ");
    Serial.println(airKnifeMode);
    Serial.println("---------------------");
}


//------------------------------------------------------------------------------
// Telemetry Helper Functions
//------------------------------------------------------------------------------

static uint16_t EncodeBeltAlerts() {
    uint16_t bits = 0;
    bits |= BeltMotor.AlertReg().bit.MotionCanceledInAlert      ? (1 << 0) : 0;
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
    return -1; // unknown / not configured
}

void SendEvent(const char *eventCode, int32_t value) {
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

    // EVENT,1,boot_id,seq,uptime_ms,event_code,value,belt_motor_uptime_ms,blade_motor_uptime_ms,
    //       belt_fault,blade_fault,alert_bits,kill_switch,cmd_age_ms,udp_fail
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

    // STATUS_UPDATE,1,boot_id,seq,uptime_ms,belt_motor_uptime_ms,blade_motor_uptime_ms,
    //               torque_pct,belt_fault,blade_fault,alert_bits,kill_switch,cmd_age_ms,udp_fail,tray_count
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
