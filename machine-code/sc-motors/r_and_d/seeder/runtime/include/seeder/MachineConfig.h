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

// Roller: TE roller_speed in [0, 20] → RPM. Motor input edge fires the move.
inline constexpr VelocityAxisConfig kRoller {
    /*vel_limit_rpm=*/700,
    /*acc_limit_rpm_per_sec=*/100000,
    /*velocity_timeout_ms=*/3000.0,
    /*rpm_per_te_unit=*/6,
    /*mode=*/VelocityDriveMode::InputTriggered,
};

} // namespace seeder::machine

#endif
