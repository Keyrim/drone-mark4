#include "status_leds.hpp"

#include "platform_stm32/board.hpp"

namespace
{
    /// IDLE and healthy: one short flash per second.
    constexpr std::uint32_t PATTERN_HEALTHY = 0b00000000000000000011U;

    /// IDLE but degraded: two short flashes per second.
    constexpr std::uint32_t PATTERN_DEGRADED = 0b00000000000000110011U;

    /// Kill engaged (the RC fail-safe included): three short flashes per
    /// second, the safety lock is visible from across the room.
    constexpr std::uint32_t PATTERN_KILL = 0b00000000001100110011U;

    /// ARMED: 2 Hz blink, the detector may spin the motors up on its own.
    constexpr std::uint32_t PATTERN_ARMED = 0b00000111110000011111U;

    /// Flight active: solid, the motors are commanded or imminent.
    constexpr std::uint32_t PATTERN_FLIGHT = 0b11111111111111111111U;

    /// CUTOFF: 5 Hz blink, safety latched until rearm. Kept distinct from
    /// the continuous 10 Hz of a failed init.
    constexpr std::uint32_t PATTERN_CUTOFF = 0b00110011001100110011U;

    /// @brief Samples a pattern at the slot the frame falls in.
    /// @param pattern 20-slot cycle, least significant bit first
    /// @param frameIndex frames since boot
    /// @return true when the LED is lit during this frame's slot
    bool patternBit(std::uint32_t pattern, std::uint32_t frameIndex)
    {
        const std::uint32_t slot =
            (frameIndex / mark4::LED_FRAMES_PER_SLOT) % mark4::LED_PATTERN_SLOTS;
        return ((pattern >> slot) & 1U) != 0U;
    }

    /// @brief Selects the pattern: the kill lock first (the phase is
    ///        always IDLE under a kill), then the flight state, health
    ///        only while the board is idle and free.
    /// @param phase current phase of the flight state machine
    /// @param killEngaged true while the kill switch or fail-safe applies
    /// @param degraded true while a service reports failures
    /// @return 20-slot pattern
    std::uint32_t selectPattern(mark4::FlightPhase phase, bool killEngaged, bool degraded)
    {
        if (killEngaged)
        {
            return PATTERN_KILL;
        }
        switch (phase)
        {
            case mark4::FlightPhase::IDLE:
                return degraded ? PATTERN_DEGRADED : PATTERN_HEALTHY;
            case mark4::FlightPhase::ARMED:
                return PATTERN_ARMED;
            case mark4::FlightPhase::CUTOFF:
                return PATTERN_CUTOFF;
            case mark4::FlightPhase::MANUAL:
            case mark4::FlightPhase::BALLISTIC:
            case mark4::FlightPhase::RECOVERY:
            case mark4::FlightPhase::HOVER:
                return PATTERN_FLIGHT;
        }
        return PATTERN_FLIGHT; // unreachable, safe side: light the LED
    }
} // namespace

namespace mark4
{
    void updateStatusLeds(FlightPhase phase,
                          bool killEngaged,
                          bool degraded,
                          std::uint32_t frameIndex)
    {
        setLed1(patternBit(selectPattern(phase, killEngaged, degraded), frameIndex));
    }
} // namespace mark4
