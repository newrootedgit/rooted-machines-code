#include "../include/utils/SafetySupervisor.h"

void SafetySupervisor::set_kill_ok(bool ok) {
    state_.kill_ok = ok;
}

Result SafetySupervisor::latch_fault(const char* reason) {
    if (state_.fault_latched) {
        return {ResultCode::Blocked, "Fault already latched"};
    }

    state_.fault_latched = true;
    fault_reason_ = reason;
    return {ResultCode::Ok};
}

bool SafetySupervisor::motion_allowed() const {
    return state_.kill_ok && !state_.fault_latched;
}

const SafetyState& SafetySupervisor::state() const {
    return state_;
}

const char* SafetySupervisor::fault_reason() const {
    return fault_reason_;
}
