#ifndef ICLOCK_H
#define ICLOCK_H

#include <cstdint>
#include "pubSysCls.h"

class IClock {
public:
    virtual ~IClock() = default;

    virtual std::uint64_t now_ms() const = 0;
    virtual void sleep_ms(std::uint32_t duration_ms) = 0;
};


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
