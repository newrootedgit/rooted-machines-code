#ifndef SEEDER_MACHINE_CONFIG_H
#define SEEDER_MACHINE_CONFIG_H

#include "../utils/Axis.h"
#include <cstddef>
#include <cstdint>

namespace seeder::machine {

inline constexpr const char* kTouchEncoderJsonPath = "/home/rooted/te-cli/TE_Variable_Values.json";
inline constexpr const char* kTelemetryLogPath = "/home/rooted/telemetry_log.jsonl";

inline constexpr std::uint64_t kSnapshotIntervalMs = 1000;

inline constexpr std::size_t kExpectedNodeCount = 1;
inline constexpr double kClearCoreEnableTimeoutMs = 5000.0;
inline constexpr std::size_t kBeltNodeIndex = 0;
inline constexpr std::size_t kRollerNodeIndexSingleNode = 0;
inline constexpr std::size_t kRollerNodeIndexMultiNode = 1;
inline constexpr const char* kRollerSingleNodeAxisLabel = "axis 0";

// Belt: TE belt_speed in [0, 20] → RPM. Negative scale flips direction.
inline constexpr VelocityAxisConfig kBelt {
    /*vel_limit_rpm=*/700,
    /*acc_limit_rpm_per_sec=*/100000,
    /*velocity_timeout_ms=*/3000.0,
    /*rpm_per_te_unit=*/-6,
    /*mode=*/VelocityDriveMode::RampToVelocity,
};

// Roller: TE roller_speed in [0, 20] → RPM. The roller node's Input-A is in
// Move-Trigger mode via ClearView; the on-node PLA fires the queued triggered
// move when Input-A asserts, after the GP timer delay (see kFireDelayScale).
inline constexpr VelocityAxisConfig kRoller {
    /*vel_limit_rpm=*/700,
    /*acc_limit_rpm_per_sec=*/100000,
    /*velocity_timeout_ms=*/3000.0,
    /*rpm_per_te_unit=*/6,
    /*mode=*/VelocityDriveMode::InputTriggered,
};

// Photoeye-to-meter trigger delay, applied on-node via CPM_P_GP_TIMER.
// Period_ms = clamp(kFireDelayScale / belt_speed, kFireDelayMinMs, kFireDelayMaxMs).
// Host re-writes this only on TE belt_speed change.
// Units: TE_units · ms.
inline constexpr int kFireDelayScale = 5000;
inline constexpr std::uint64_t kFireDelayMinMs = 0;
inline constexpr std::uint64_t kFireDelayMaxMs = 10000;

// Solenoid wired to SC-Hub Brake_0 (first brake terminal). Mode is forced to
// user-settable via BrakeSetting(GPO_ON/GPO_OFF) at runtime — the SDK call
// itself takes the pin out of BRAKE_AUTOCONTROL. Fires a fixed-duration pulse
// on each photoeye rising edge. Brake terminals are not PLA-routable, so this
// path stays host-driven.
inline constexpr std::size_t kSolenoidBrakeNum = 0;
inline constexpr std::uint64_t kSolenoidPulseMs = 100;

} // namespace seeder::machine

#endif
