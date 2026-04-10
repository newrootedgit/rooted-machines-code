#ifndef ICLOCK_H
#define ICLOCK_H

#include <cstdint>

class IClock {
public:
    virtual ~IClock() = default;

    virtual std::uint64_t now_ms() const = 0;
    virtual void sleep_ms(std::uint32_t duration_ms) = 0;
};

#endif
