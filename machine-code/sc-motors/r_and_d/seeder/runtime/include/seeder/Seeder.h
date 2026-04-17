#ifndef SEEDER_H
#define SEEDER_H

#include "../RuntimeTypes.h"
#include "../utils/Axis.h"
#include "../utils/IClock.h"
#include "../utils/ILogger.h"
#include "SafetySupervisor.h"

struct SeederState {
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

class SeederSequenceRunner {
public:
    void start_height_change(double target_mm);
    void start_apply_preset(const DesiredPreset& preset);

    void tick(SeederState& state);

    bool busy() const;
    void cancel();
    void reset();
};

class Seeder {
public:
    Seeder(IClock& clock, IVelocityAxis& belt, IVelocityAxis& blade, IPositionAxis& rail);

    Result submit(const SeederCommand& command);
    void tick();
    SeederState snapshot() const;

private:
    SeederState state_;

    // SafetySupervisor safety_;
    SeederSequenceRunner sequences_;

    IVelocityAxis& belt_;
    IVelocityAxis& blade_;
    IPositionAxis& rail_;

    IClock& clock_;
    // ILogger& logger_;
};

#endif
