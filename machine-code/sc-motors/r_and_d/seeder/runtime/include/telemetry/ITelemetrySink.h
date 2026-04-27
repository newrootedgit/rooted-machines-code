#ifndef ITELEMETRY_SINK_H
#define ITELEMETRY_SINK_H

#include "../RuntimeTypes.h"
#include "TelemetryTypes.h"

class ITelemetrySink {
public:
    virtual ~ITelemetrySink() = default;

    virtual Result log_event(const MachineEvent& event) = 0;
    virtual Result log_snapshot(const MachineSnapshot& snapshot) = 0;
};

#endif
