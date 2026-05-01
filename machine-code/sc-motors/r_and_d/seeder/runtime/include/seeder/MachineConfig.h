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

// Roller: TE roller_speed in [0, 20] → RPM. Pi gates motion via photoeye state +
// linger window; motor itself runs whenever Pi commands a non-zero velocity.
inline constexpr VelocityAxisConfig kRoller {
    /*vel_limit_rpm=*/700,
    /*acc_limit_rpm_per_sec=*/100000,
    /*velocity_timeout_ms=*/3000.0,
    /*rpm_per_te_unit=*/6,
    /*mode=*/VelocityDriveMode::RampToVelocity,
};

// Roller post-photoeye-clear linger. Duration = clamp(kRollerLingerScale / belt_speed,
// 0, kRollerMaxLingerMs). Empirical; tune at bench against trail-out flush behavior.
// Units: TE_units · ms.
inline constexpr int kRollerLingerScale = 5000;
inline constexpr std::uint64_t kRollerMaxLingerMs = 10000;

// Solenoid wired to SC-Hub Brake_0 (first brake terminal). Mode is forced to
// user-settable via BrakeSetting(GPO_ON/GPO_OFF) at runtime — the SDK call
// itself takes the pin out of BRAKE_AUTOCONTROL. Fires a fixed-duration pulse
// on each photoeye rising edge.
inline constexpr std::size_t kSolenoidBrakeNum = 0;
inline constexpr std::uint64_t kSolenoidPulseMs = 100;
// Ignore photoeye edges for this long after start() — gives the SC-Hub input
// time to stabilize and prevents fake edges from being interpreted as triggers.
inline constexpr std::uint64_t kSolenoidArmDelayMs = 1000;

} // namespace seeder::machine

#endif
