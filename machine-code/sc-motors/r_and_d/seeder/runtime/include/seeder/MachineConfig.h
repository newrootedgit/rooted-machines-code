#ifndef SEEDER_MACHINE_CONFIG_H
#define SEEDER_MACHINE_CONFIG_H

#include "../utils/Axis.h"

namespace seeder::machine {

// Belt: TE belt_speed in [0, 20] → RPM. Negative scale flips direction.
inline constexpr VelocityAxisConfig kBelt {
    /*vel_limit_rpm=*/700,
    /*acc_limit_rpm_per_sec=*/100000,
    /*velocity_timeout_ms=*/3000.0,
    /*rpm_per_te_unit=*/-6,
};

// Roller: TE roller_speed in [0, 20] → RPM.
inline constexpr VelocityAxisConfig kRoller {
    /*vel_limit_rpm=*/700,
    /*acc_limit_rpm_per_sec=*/100000,
    /*velocity_timeout_ms=*/3000.0,
    /*rpm_per_te_unit=*/6,
};

} // namespace seeder::machine

#endif
