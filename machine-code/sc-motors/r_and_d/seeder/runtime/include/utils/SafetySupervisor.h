#ifndef SAFETY_SUPERVISOR_H
#define SAFETY_SUPERVISOR_H

#include "../RuntimeTypes.h"

class SafetySupervisor {
public:
    SafetySupervisor() = default;

    void set_kill_ok(bool ok);
    Result latch_fault(const char* reason);

    bool motion_allowed() const;
    const SafetyState& state() const;
    const char* fault_reason() const;

private:
    SafetyState state_ {true, false};
    const char* fault_reason_ = "";
};

#endif
