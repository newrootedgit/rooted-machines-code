#ifndef HARVESTER_MACHINE_CONFIG_H
#define HARVESTER_MACHINE_CONFIG_H

#include "../utils/Axis.h"

namespace harvester::machine {

// Belt: TE belt_speed in [0, 20] → RPM. Negative scale flips direction.
inline constexpr VelocityAxisConfig kBelt {
    /*vel_limit_rpm=*/700,
    /*acc_limit_rpm_per_sec=*/100000,
    /*velocity_timeout_ms=*/3000.0,
    /*rpm_per_te_unit=*/-6,
};

// Blade: TE blade_speed in [0, 3] → RPM.
inline constexpr VelocityAxisConfig kBlade {
    /*vel_limit_rpm=*/700,
    /*acc_limit_rpm_per_sec=*/100000,
    /*velocity_timeout_ms=*/3000.0,
    /*rpm_per_te_unit=*/233,
};

inline constexpr PositionAxisConfig kRail {
    /*vel_limit_rpm=*/700,
    /*acc_limit_rpm_per_sec=*/100000,
    /*counts_per_mm=*/397.0,
    /*move_timeout_ms=*/10000.0,
    /*home_timeout_ms=*/15000.0,
};

} // namespace harvester::machine

#endif
