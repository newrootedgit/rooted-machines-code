#ifndef IAXIS_H
#define IAXIS_H

#include "../RuntimeTypes.h"
#include "pubSysCls.h"

class IAxis {
public:
    virtual ~IAxis() = default;

    virtual Result stop() = 0;
    virtual AxisStatus status() const = 0;
};

class IVelocityAxis : public virtual IAxis {
public:
    virtual Result set_velocity_rpm(int rpm) = 0;
};

class IPositionAxis : public virtual IAxis {
public:
    virtual Result home() = 0;
    virtual Result move_to_mm(double position_mm) = 0;
};

struct VelocityAxisConfig {
    int vel_limit_rpm = 700;
    int acc_limit_rpm_per_sec = 100000;
    double velocity_timeout_ms = 3000.0;
};

struct PositionAxisConfig {
    int vel_limit_rpm = 700;
    int acc_limit_rpm_per_sec = 100000;
    double counts_per_mm = 397.0;
    double move_timeout_ms = 10000.0;
    double home_timeout_ms = 15000.0;
};

class SCVelocityAxis : public IVelocityAxis {
public:
    SCVelocityAxis(sFnd::INode& node, const VelocityAxisConfig& config);

    Result stop() override;
    AxisStatus status() const override;

    Result set_velocity_rpm(int rpm) override;

private:
    bool is_bus_power_low() const;
    bool is_alert_present() const;

    sFnd::INode& node_;
    VelocityAxisConfig config_;
};

class SCPositionAxis : public IPositionAxis {
public:
    SCPositionAxis(sFnd::INode& node, const PositionAxisConfig& config);

    Result stop() override;
    AxisStatus status() const override;

    Result home() override;
    Result move_to_mm(double position_mm) override;

private:
    bool is_bus_power_low() const;
    bool is_alert_present() const;

    sFnd::INode& node_;
    PositionAxisConfig config_;
};

#endif
