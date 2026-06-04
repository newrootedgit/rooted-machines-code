
#include "ClearCore.h"
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <string.h>

#define BladeMotor ConnectorM0

#define LinearRailMotor ConnectorM2
#define BeltMotor ConnectorM3

// Pins that support digital interrupts: DI-6, DI-7, DI-8, A-9, A-10, A-11, A-12
#define UpperLimitPin DI7  // DI7 -> Upper Limit Switch (supports interrupts)
#define LowerLimitPin DI8  // DI8 -> Lower Limit Switch (supports interrupts)


#define KillSwitchPin DI6  // Blade Soft Kill Switch

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

// ---- UDP telemetry ----
// Telemetry rides a SEPARATE UDP socket from the TCP control channel so that a
// slow or backed-up control connection can never delay status emission.
// Fire-and-forget — never block on send.
EthernetUDP        Udp;
const uint16_t     UDP_LOCAL_PORT     = 9998;
unsigned int       remotePort         = 9999;
const uint16_t     STATUS_INTERVAL_MS = 1000;

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
    uint32_t beltMotorUptimeMs;   // millis() when belt started moving (0 = idle)
    uint32_t bladeMotorUptimeMs;  // millis() when blade started moving (0 = idle)
};
TelemetryState t;

int accelerationLimit = 100000; // pulses per sec^2
int screen2Var1Value = 0;  // Value for motor RPM
int screen3Var1Value = 0;  // Value saved for later implementation

// Linear rail configuration
#define STEPS_PER_MM 397             // Steps per millimeter of linear rail travel
#define LINEAR_RAIL_MIN_POS -100     // Minimum position (counts) ~-0.25mm
#define LINEAR_RAIL_MAX_POS -25001   // Maximum position (counts) ~-63mm
#define LINEAR_RAIL_VELOCITY 1000    // pulses per sec (10% of example)
#define LINEAR_RAIL_ACCEL 2000       // pulses per sec^2 (20% of original)

// Three absolute positions for the linear rail cycle (in steps)
int32_t linearRailPositions[3] = {-15000, -20000, -24000};  // Position 1, 2, 3
int currentPositionIndex = 0;  // Track which position we're moving to

// Variables from TCP server (CSV format: ready_to_run,active_variety,blade_speed,belt_speed,blade_height)
int readyToRun = 0;
int activeVariety = -1;
int bladeSpeed = 0;
int beltSpeed = 0;
int bladeHeight = 0;

int lastBladeHeight = -1;  // Track previous blade height to detect changes
int lastBeltSpeed = -1;    // Track previous belt speed to detect changes
int lastBladeSpeed = -1;   // Track previous blade speed to detect changes
bool lastKillSwitchState = false;  // Track kill switch state changes
bool lastReadyToRunState = false;  // Track ready_to_run changes

// Belt speed scaling factor (converts beltSpeed value to pulses/sec)
#define BELT_SPEED_SCALE 600  // Adjust this to tune belt speed

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
bool ParseTCPMessage(char *message);
void PrintTCPVariables();

// Telemetry functions
void SendStatusUpdate();
void SendEvent(const char *eventCode, const char *motor);

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
    pinMode(KillSwitchPin, INPUT);

    Serial.begin(baudRate);
    delay(2000); // Give time for Serial to initialize

    Serial.println("=== ClearCore Starting ===");

    // Attach interrupts for limit switches (RISING = LOW -> HIGH transition)
    // attachInterrupt(digitalPinToInterrupt(LowerLimitPin), OnLowerLimitHit, RISING);
    // attachInterrupt(digitalPinToInterrupt(UpperLimitPin), OnUpperLimitHit, RISING);
    // Serial.println("Interrupts attached for limit switches on DI7 and DI8");
    Serial.println("Limit switch interrupts DISABLED");

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
    /////////////// Telemetry Initialization ///////////////
    ////////////////////////////////////////////////////////
    t.bootId             = millis() ^ 0x5EED5EED;
    t.seq                = 0;
    t.lastStatusMs       = 0;
    t.lastRxCmdMs        = millis();
    t.faultCountBelt     = 0;
    t.faultCountBlade    = 0;
    t.udpSendFailCount   = 0;
    t.lastBeltFault      = false;
    t.lastBladeFault     = false;
    t.beltMotorUptimeMs  = 0;
    t.bladeMotorUptimeMs = 0;

    Udp.begin(UDP_LOCAL_PORT);
    Serial.println("UDP telemetry initialized");

    Serial.println("Setup complete. TCP reconnect starts in loop.");
}

void loop() {
    // Match the tabletop seeder pattern: service TCP first in loop. This is the
    // first place we attempt to connect, so motor setup/homing has completed.
    ReadFromTCPServer();

    // Handle limit switch interrupts (DISABLED)
    // if (lowerLimitHit) {
    //     lowerLimitHit = false;
    //     Serial.println(">>> [INTERRUPT] Stopping linear rail and going up <<<");
    // }
    // if (upperLimitHit) {
    //     upperLimitHit = false;
    //     Serial.println(">>> [INTERRUPT] Stopping linear rail and going down <<<");
    // }

    // Read kill switch state (HIGH = activated, motors can run)
    bool killSwitchActive = digitalRead(KillSwitchPin);

    // Detect kill switch state change
    if (killSwitchActive != lastKillSwitchState) {
        Serial.print("Kill switch changed: ");
        Serial.println(killSwitchActive ? "ACTIVE (motors enabled)" : "INACTIVE (motors stopped)");

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
        lastKillSwitchState = killSwitchActive;
    }

    if (readyToRun) {
        if (!lastReadyToRunState) {
            Serial.println("ready_to_run active");
            lastBeltSpeed = -1;
            lastBladeSpeed = -1;
        }

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
    } else if (lastReadyToRunState) {
        MoveAtVelocity(0);
        RampToVelocitySelection(0);
        lastBeltSpeed = -1;
        lastBladeSpeed = -1;
        Serial.println("ready_to_run inactive - belt and blade stopped");
    }
    lastReadyToRunState = readyToRun;

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
}

bool MoveAtVelocity(int32_t velocity) {
  // Check if a motor alert is currently preventing motion
  // Clear alert if configured to do so 
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
    Serial.println();
        return false;
    }

    // Serial.print("Moving at velocity: ");
    // Serial.println(velocity);

    // Command the velocity move
    BeltMotor.MoveVelocity(velocity);

    // Track belt-running uptime for telemetry: 0 commanded -> idle, otherwise
    // stamp the start time so SendStatusUpdate reports the current run duration.
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

    return true; 
}

bool RampToVelocitySelection(int velocityIndex) {
    // Check if a motor fault is currently preventing motion
  // Clear fault if configured to do so 
    if (BladeMotor.StatusReg().bit.MotorInFault) {
    SendEvent("FAULT_BLADE", "blade");
    t.faultCountBlade++;
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
            Serial.println(" (Inputs A Off/B Off) - OFF");
            break;
        case 1:
            // Sets Input A and B for velocity 1
            BladeMotor.MotorInAState(true);
            BladeMotor.MotorInBState(false);
            Serial.println(" (Inputs A On/B Off)");
            break;
        case 2:
            // Sets Input A and B for velocity 2
            BladeMotor.MotorInAState(false);
            BladeMotor.MotorInBState(true);
            Serial.println(" (Inputs A Off/B On)");
            break;
        case 3:
            // Sets Input A and B for velocity 3
            BladeMotor.MotorInAState(true);
            BladeMotor.MotorInBState(true);
            Serial.println(" (Inputs A On/B On)");
            break;
        default:
            // If this case is reached then an incorrect velocityIndex was
            // entered
            return false;
    }

    // Track blade-running uptime for telemetry: selection 0 -> idle, otherwise
    // stamp the start time so SendStatusUpdate reports the current run duration.
    if (velocityIndex == 0) {
        t.bladeMotorUptimeMs = 0;
    } else if (t.bladeMotorUptimeMs == 0) {
        t.bladeMotorUptimeMs = millis();
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
    SendEvent("FAULT_BLADE", "blade");
    t.faultCountBlade++;
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
 *    Maintains a persistent TCP connection and reads newline-framed CSV data.
 *    CSV format: ready_to_run,active_variety,blade_speed,belt_speed,blade_height
 *    Returns true when at least one new snapshot was parsed.
 */
bool ReadFromTCPServer() {
    static unsigned long lastReconnectAttempt = 0;
    static char lineBuf[MAX_PACKET_LENGTH];
    static char lastLine[MAX_PACKET_LENGTH];
    static size_t lineLen = 0;
    bool parsedSnapshot = false;

    if (!client.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectAttempt >= 2000) {
            lastReconnectAttempt = now;
            lineLen = 0;
            if (Ethernet.linkStatus() != LinkON) {
                Serial.println("TCP: Ethernet link is OFF; skipping reconnect");
                return false;
            }
            Serial.println("TCP: attempting reconnect...");
            client.stop();
            if (client.connect(serverIp, PORT_NUM)) {
                Serial.println("TCP: connected to server");
            } else {
                Serial.println("TCP: connection failed");
            }
        }
        return false;
    }

    while (client.available() > 0) {
        int c = client.read();
        if (c < 0) {
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (lineLen > 0) {
                lineBuf[lineLen] = '\0';
                if (strcmp(lineBuf, lastLine) != 0) {
                    memcpy(lastLine, lineBuf, lineLen + 1);
                    if (ParseTCPMessage(lineBuf)) {
                        parsedSnapshot = true;
                    }
                }
            }
            lineLen = 0;
        } else if (lineLen < sizeof(lineBuf) - 1) {
            lineBuf[lineLen++] = (char)c;
        } else {
            lineLen = 0;
            Serial.println("TCP: packet too long, resyncing");
        }
    }

    return parsedSnapshot;
}

bool ParseTCPMessage(char *message) {
    t.lastRxCmdMs = millis(); // for telemetry cmdAgeMs

    int fieldIndex = 0;
    char *token = strtok(message, ",");

    int parsedReadyToRun = 0;
    int parsedActiveVariety = -1;
    int parsedBladeSpeed = 0;
    int parsedBeltSpeed = 0;
    int parsedBladeHeight = 0;

    while (token != NULL && fieldIndex < 5) {
        switch (fieldIndex) {
            case 0:
                parsedReadyToRun = atoi(token);
                break;
            case 1:
                parsedActiveVariety = atoi(token);
                break;
            case 2:
                parsedBladeSpeed = atoi(token);
                break;
            case 3:
                parsedBeltSpeed = atoi(token);
                break;
            case 4:
                parsedBladeHeight = atoi(token);
                break;
        }

        fieldIndex++;
        token = strtok(NULL, ",");
    }

    if (fieldIndex < 5) {
        Serial.println("TCP: invalid CSV snapshot");
        return false;
    }

    readyToRun = parsedReadyToRun;
    activeVariety = parsedActiveVariety;
    bladeSpeed = parsedBladeSpeed;
    beltSpeed = parsedBeltSpeed;
    bladeHeight = parsedBladeHeight;

    Serial.print("TCP: ready=");
    Serial.print(readyToRun);
    Serial.print(" variety=");
    Serial.print(activeVariety);
    Serial.print(" blade=");
    Serial.print(bladeSpeed);
    Serial.print(" belt=");
    Serial.print(beltSpeed);
    Serial.print(" height=");
    Serial.println(bladeHeight);

    return true;
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
    Serial.println("---------------------");
}

//------------------------------------------------------------------------------
// Telemetry — UDP fire-and-forget. NEVER block on send. Uses a separate UDP
// socket from the TCP control channel so control-channel backpressure can't
// stall status emission.
//------------------------------------------------------------------------------

void SendStatusUpdate() {
    if (Ethernet.linkStatus() != LinkON) {
        return;
    }
    char telemetryBuffer[256];
    uint32_t uptimeMs = millis();
    uint32_t cmdAgeMs = uptimeMs - t.lastRxCmdMs;
    uint32_t seq = ++t.seq;

    uint32_t beltUptime  = t.beltMotorUptimeMs  ? (uptimeMs - t.beltMotorUptimeMs)  : 0;
    uint32_t bladeUptime = t.bladeMotorUptimeMs ? (uptimeMs - t.bladeMotorUptimeMs) : 0;

    // Format (schema_ver 2):
    //   STATUS_UPDATE,2,bootId,seq,uptimeMs,belt_motor_uptime_ms,
    //   blade_motor_uptime_ms,cmdAgeMs
    // Both belt and blade run continuously while commanded, so each uptime is
    // the current run duration (0 = idle).
    snprintf(telemetryBuffer, sizeof(telemetryBuffer),
             "STATUS_UPDATE,2,%lu,%lu,%lu,%lu,%lu,%lu",
             (unsigned long)t.bootId,
             (unsigned long)seq,
             (unsigned long)uptimeMs,
             (unsigned long)beltUptime,
             (unsigned long)bladeUptime,
             (unsigned long)cmdAgeMs);

    Udp.beginPacket(serverIp, remotePort);
    Udp.write((const uint8_t *)telemetryBuffer, strlen(telemetryBuffer));
    if (!Udp.endPacket()) {
        t.udpSendFailCount++;
    }
}

// Lightweight fault ping — emitted when a motor fault is detected. Receiver
// joins with the nearest STATUS_UPDATE on (bootId, uptimeMs) for full state at
// event time. eventCode must start with "FAULT_"; motor attributes the fault
// ("belt" or "blade").
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
