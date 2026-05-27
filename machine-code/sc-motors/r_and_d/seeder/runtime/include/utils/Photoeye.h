#ifndef PHOTOEYE_H
#define PHOTOEYE_H

#include "../RuntimeTypes.h"
#include "pubSysCls.h"

// Reads Input-A on a ClearPath-SC node. Input-A is in Move-Trigger mode (the
// node's PLA fires queued triggered moves on rising edge), but the status
// register still reflects the raw pin level, which is what the host uses to
// pulse the solenoid.
class Photoeye {
public:
    explicit Photoeye(sFnd::INode& node) : node_(node) {}

    Result refresh() {
        try {
            blocked_ = node_.Status.RT.Value().cpm.InA;
            return {ResultCode::Ok};
        } catch (...) {
            return {ResultCode::Error, "Failed to read motor input"};
        }
    }

    bool blocked() const { return blocked_; }

private:
    sFnd::INode& node_;
    bool blocked_ = false;
};

#endif
