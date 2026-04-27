#ifndef JSONL_TELEMETRY_LOGGER_H
#define JSONL_TELEMETRY_LOGGER_H

#include "../RuntimeTypes.h"
#include "ITelemetrySink.h"
#include "TelemetryTypes.h"
#include <string>
#include <fstream>

class JsonlTelemetryLogger : public ITelemetrySink {
    public:
        explicit JsonlTelemetryLogger(const std::string& path);
        ~JsonlTelemetryLogger() override = default;

        Result log_event(const MachineEvent& event) override;
        Result log_snapshot(const MachineSnapshot& snapshot) override;

    private:
        Result ensure_open();

        std::string path_;
        std::ofstream file_;
};

#endif
