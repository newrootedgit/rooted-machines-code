
#include "ClearCore.h"

#define BladeMotor ConnectorM0

#define INPUT_A_B_FILTER 20
#define baudRate 9600
#define HANDLE_MOTOR_FAULTS (1)

bool RampToVelocitySelection(int velocityIndex);
void HandleMotorFaults();

void setup() {
    Serial.begin(baudRate);
    delay(2000);

    Serial.println("=== Blade Motor Test ===");

    ////////////////////////////////////////////////////////////////
    /////////////////// Blade Motor Setup //////////////////////////
    ////////////////////////////////////////////////////////////////

    MotorMgr.MotorModeSet(MotorManager::MOTOR_M0M1, Connector::CPM_MODE_A_DIRECT_B_DIRECT);
    BladeMotor.HlfbMode(MotorDriver::HLFB_MODE_HAS_BIPOLAR_PWM);
    BladeMotor.HlfbCarrier(MotorDriver::HLFB_CARRIER_482_HZ);
    BladeMotor.MotorInAState(false);
    BladeMotor.MotorInBState(false);
    BladeMotor.EnableRequest(true);
    Serial.println("BladeMotor Enabled");

    uint32_t startTime = millis();
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

    Serial.println("");
    Serial.println("Enter blade speed (0-3):");
    Serial.println("  0 = Off (A Off/B Off)");
    Serial.println("  1 = Speed 1 (A On/B Off)");
    Serial.println("  2 = Speed 2 (A Off/B On)");
    Serial.println("  3 = Speed 3 (A On/B On)");
}

void loop() {
    if (Serial.available() > 0) {
        String input = Serial.readStringUntil('\n');
        input.trim();

        int speed = input.toInt();

        if (speed >= 0 && speed <= 3) {
            Serial.print("Setting blade speed to: ");
            Serial.println(speed);
            RampToVelocitySelection(speed);
        } else {
            Serial.println("Invalid input. Enter 0-3.");
        }
    }
}

bool RampToVelocitySelection(int velocityIndex) {
    if (BladeMotor.StatusReg().bit.MotorInFault) {
        if (HANDLE_MOTOR_FAULTS) {
            Serial.println("Motor fault detected. Move canceled.");
            HandleMotorFaults();
        } else {
            Serial.println("Motor fault detected. Move canceled.");
        }
        return false;
    }

    Serial.print("Setting velocity selection: ");
    Serial.print(velocityIndex);

    switch (velocityIndex) {
        case 0:
            BladeMotor.MotorInAState(false);
            BladeMotor.MotorInBState(false);
            Serial.println(" (Inputs A Off/B Off) - OFF");
            break;
        case 1:
            BladeMotor.MotorInAState(true);
            BladeMotor.MotorInBState(false);
            Serial.println(" (Inputs A On/B Off)");
            break;
        case 2:
            BladeMotor.MotorInAState(false);
            BladeMotor.MotorInBState(true);
            Serial.println(" (Inputs A Off/B On)");
            break;
        case 3:
            BladeMotor.MotorInAState(true);
            BladeMotor.MotorInBState(true);
            Serial.println(" (Inputs A On/B On)");
            break;
        default:
            Serial.println(" - Invalid");
            return false;
    }

    delay(20 + INPUT_A_B_FILTER);

    Serial.println("Waiting for HLFB...");
    while (BladeMotor.HlfbState() != MotorDriver::HLFB_ASSERTED &&
           !BladeMotor.StatusReg().bit.MotorInFault) {
        continue;
    }

    if (BladeMotor.StatusReg().bit.MotorInFault) {
        Serial.println("Motor fault detected.");
        if (HANDLE_MOTOR_FAULTS) {
            HandleMotorFaults();
        }
        Serial.println("Motion may not have completed.");
        return false;
    } else {
        Serial.println("Done");
        return true;
    }
}

void HandleMotorFaults() {
    Serial.println("Clearing faults by cycling enable...");
    BladeMotor.EnableRequest(false);
    delay(10);
    BladeMotor.EnableRequest(true);
    delay(100);
}
