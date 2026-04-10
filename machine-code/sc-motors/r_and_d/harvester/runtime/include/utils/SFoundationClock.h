#ifndef SFOUNDATION_CLOCK_H
#define SFOUNDATION_CLOCK_H

#include "IClock.h"
#include "pubSysCls.h"

class SFoundationClock : public IClock {
public:
    std::uint64_t now_ms() const override {
        return static_cast<std::uint64_t>(sFnd::SysManager::Instance()->TimeStampMsec());
    }

    void sleep_ms(std::uint32_t duration_ms) override {
        sFnd::SysManager::Instance()->Delay(duration_ms);
    }
};

#endif
