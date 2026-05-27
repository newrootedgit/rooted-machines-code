#ifndef AXIS_H
#define AXIS_H

#include "../RuntimeTypes.h"
#include "pubSysCls.h"

enum class VelocityDriveMode {
    // Pi commands the velocity; motor ramps under configured accel/decel limits.
    RampToVelocity,
    // Motor's own input pin (pre-configured via ClearView) fires the move;
    // Pi only pushes the setpoint and does not wait on VelocityAtTarget.
    InputTriggered,
};

struct VelocityAxisConfig {
    int vel_limit_rpm = 700;
    int acc_limit_rpm_per_sec = 100000;
    double velocity_timeout_ms = 3000.0;
    // TE-unit → RPM scale. Sign flips direction. Default 1 = passthrough.
    int rpm_per_te_unit = 1;
    VelocityDriveMode mode = VelocityDriveMode::RampToVelocity;
};

class SCVelocityAxis {
public:
    SCVelocityAxis(sFnd::INode& node, const VelocityAxisConfig& config);

    Result set_velocity_rpm(int rpm);
    Result stop();
    AxisStatus status() const;

private:
    bool is_bus_power_low() const;
    bool is_alert_present() const;

    sFnd::INode& node_;
    VelocityAxisConfig config_;
};

#endif
