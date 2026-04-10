#include "utils/Axis.h"
#include <cmath>

SCVelocityAxis::SCVelocityAxis(sFnd::INode& node, const VelocityAxisConfig& config)
    : node_(node), config_(config) {
    node_.AccUnit(sFnd::INode::RPM_PER_SEC);
    node_.VelUnit(sFnd::INode::RPM);
    node_.Motion.AccLimit = config_.acc_limit_rpm_per_sec;
    node_.Motion.VelLimit = config_.vel_limit_rpm;
}

bool SCVelocityAxis::is_bus_power_low() const {
    return node_.Status.Power.Value().fld.InBusLoss;
}

bool SCVelocityAxis::is_alert_present() const {
    node_.Status.RT.Refresh();
    return node_.Status.RT.Value().cpm.AlertPresent;
}

Result SCVelocityAxis::set_velocity_rpm(int rpm) {
    try {
        if (is_alert_present()) {
            return {ResultCode::Error, "Node has alerts"};
        }

        if (!node_.Motion.IsReady()) {
            return {ResultCode::Error, "Node not ready"};
        }

        int clamped_rpm = rpm;
        if (clamped_rpm > config_.vel_limit_rpm) {
            clamped_rpm = config_.vel_limit_rpm;
        }
        if (clamped_rpm < -config_.vel_limit_rpm) {
            clamped_rpm = -config_.vel_limit_rpm;
        }

        node_.Motion.MoveVelStart(clamped_rpm);

        sFnd::SysManager* mgr = sFnd::SysManager::Instance();
        const double timeout = mgr->TimeStampMsec() + config_.velocity_timeout_ms;

        while (!node_.Motion.VelocityAtTarget()) {
            if (is_alert_present()) {
                return {ResultCode::Error, "Velocity move faulted"};
            }

            if (is_bus_power_low()) {
                return {ResultCode::Error, "Velocity move failed: bus power low"};
            }

            if (mgr->TimeStampMsec() > timeout) {
                return {ResultCode::Error, "Velocity move timed out"};
            }
        }

        return {ResultCode::Ok};
    } catch (sFnd::mnErr&) {
        return {ResultCode::Error, "Velocity move failed"};
    }
}

Result SCVelocityAxis::stop() {
    try {
        node_.Motion.NodeStop(STOP_TYPE_RAMP_AT_DECEL);
        return {ResultCode::Ok};
    } catch (sFnd::mnErr&) {
        return {ResultCode::Error, "Velocity stop failed"};
    }
}

AxisStatus SCVelocityAxis::status() const {
    try {
        node_.Status.RT.Refresh();

        AxisStatus axis_status;
        axis_status.enabled = node_.Status.RT.Value().cpm.Ready;
        axis_status.faulted = node_.Status.RT.Value().cpm.AlertPresent;
        axis_status.moving = std::fabs(node_.Motion.VelMeasured.Value()) > 0.1;
        return axis_status;
    } catch (sFnd::mnErr&) {
        AxisStatus axis_status;
        axis_status.enabled = false;
        axis_status.faulted = true;
        axis_status.moving = false;
        return axis_status;
    }
}

SCPositionAxis::SCPositionAxis(sFnd::INode& node, const PositionAxisConfig& config)
    : node_(node), config_(config) {
    node_.AccUnit(sFnd::INode::RPM_PER_SEC);
    node_.VelUnit(sFnd::INode::RPM);
    node_.Motion.AccLimit = config_.acc_limit_rpm_per_sec;
    node_.Motion.VelLimit = config_.vel_limit_rpm;
}

bool SCPositionAxis::is_bus_power_low() const {
    return node_.Status.Power.Value().fld.InBusLoss;
}

bool SCPositionAxis::is_alert_present() const {
    node_.Status.RT.Refresh();
    return node_.Status.RT.Value().cpm.AlertPresent;
}

Result SCPositionAxis::stop() {
    try {
        node_.Motion.NodeStop(STOP_TYPE_RAMP_AT_DECEL);
        return {ResultCode::Ok};
    } catch (sFnd::mnErr&) {
        return {ResultCode::Error, "Position stop failed"};
    }
}

AxisStatus SCPositionAxis::status() const {
    try {
        node_.Status.RT.Refresh();

        AxisStatus axis_status;
        axis_status.enabled = node_.Status.RT.Value().cpm.Ready;
        axis_status.faulted = node_.Status.RT.Value().cpm.AlertPresent;
        axis_status.moving = !node_.Motion.MoveIsDone();
        return axis_status;
    } catch (sFnd::mnErr&) {
        AxisStatus axis_status;
        axis_status.enabled = false;
        axis_status.faulted = true;
        axis_status.moving = false;
        return axis_status;
    }
}

Result SCPositionAxis::home() {
    return {ResultCode::Error, "Position homing not implemented"};
}

Result SCPositionAxis::move_to_mm(double /*position_mm*/) {
    return {ResultCode::Error, "Position move not implemented"};
}
