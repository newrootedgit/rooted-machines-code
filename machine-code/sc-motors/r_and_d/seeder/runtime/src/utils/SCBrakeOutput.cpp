#include "utils/SCBrakeOutput.h"

using namespace sFnd;

SCBrakeOutput::SCBrakeOutput(IPort& port, std::size_t brake_num)
    : port_(port), brake_num_(brake_num) {}

Result SCBrakeOutput::set_on() {
    try {
        port_.BrakeControl.BrakeSetting(brake_num_, GPO_ON);
        on_ = true;
        return {ResultCode::Ok};
    } catch (...) {
        return {ResultCode::Error, "BrakeSetting GPO_ON failed"};
    }
}

Result SCBrakeOutput::set_off() {
    try {
        port_.BrakeControl.BrakeSetting(brake_num_, GPO_OFF);
        on_ = false;
        return {ResultCode::Ok};
    } catch (...) {
        return {ResultCode::Error, "BrakeSetting GPO_OFF failed"};
    }
}

bool SCBrakeOutput::is_on() const {
    return on_;
}
