#ifndef HARVESTER_H
#define HARVESTER_H

#include "../RuntimeTypes.h"
#include "../utils/Axis.h"
#include "../utils/IClock.h"
#include "../utils/ILogger.h"
#include "SafetySupervisor.h"

struct HarvesterState {
    DesiredPreset desired_preset {};
    std::uint64_t applied_revision = 0;

    SafetyState safety {};
    // SequenceState sequence;

    // MotorDesiredState belt_desired;
    // MotorDesiredState blade_desired;
    // MotorDesiredState rail_desired;

    // MotorObservedState belt_observed;
    // MotorObservedState blade_observed;
    // MotorObservedState rail_observed;
};

class HarvesterSequenceRunner {
public:
    void start_height_change(double target_mm);
    void start_apply_preset(const DesiredPreset& preset);

    void tick(HarvesterState& state);

    bool busy() const;
    void cancel();
    void reset();
};

class Harvester {
public:
    Harvester(IClock& clock, IVelocityAxis& belt, IVelocityAxis& blade, IPositionAxis& rail);

    Result submit(const HarvesterCommand& command);
    void tick();
    HarvesterState snapshot() const;

private:
    HarvesterState state_;

    // SafetySupervisor safety_;
    HarvesterSequenceRunner sequences_;

    IVelocityAxis& belt_;
    IVelocityAxis& blade_;
    IPositionAxis& rail_;

    IClock& clock_;
    // ILogger& logger_;
};

#endif
