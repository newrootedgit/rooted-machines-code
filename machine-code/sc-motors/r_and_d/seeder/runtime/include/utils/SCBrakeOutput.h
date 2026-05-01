#ifndef SCBRAKE_OUTPUT_H
#define SCBRAKE_OUTPUT_H

#include "../RuntimeTypes.h"
#include "pubSysCls.h"
#include <cstddef>

// Drives an SC-Hub brake/GPO line as a general-purpose output. P21 = Brake_1
// (brake_num = 1), P12-side = Brake_0. The pin must be configured in ClearView
// as a brake/general output, not auto-control, for GPO_ON/GPO_OFF to apply.
class SCBrakeOutput {
public:
    SCBrakeOutput(sFnd::IPort& port, std::size_t brake_num);

    Result set_on();
    Result set_off();
    bool is_on() const;

private:
    sFnd::IPort& port_;
    std::size_t brake_num_;
    bool on_ = false;
};

#endif
