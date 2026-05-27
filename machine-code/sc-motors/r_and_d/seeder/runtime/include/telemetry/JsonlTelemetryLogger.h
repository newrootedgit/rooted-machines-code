#ifndef JSONL_TELEMETRY_LOGGER_H
#define JSONL_TELEMETRY_LOGGER_H

#include "../RuntimeTypes.h"
#include "TelemetryTypes.h"
#include <string>
#include <fstream>

class JsonlTelemetryLogger {
public:
    explicit JsonlTelemetryLogger(const std::string& path);

    Result log_event(const MachineEvent& event);
    Result log_snapshot(const MachineSnapshot& snapshot);

private:
    Result ensure_open();

    std::string path_;
    std::ofstream file_;
};

#endif
