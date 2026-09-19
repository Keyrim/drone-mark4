#pragma once

/// @file
/// @brief The gate of a node that flies: the arming state comes from the
///        flight core it carries.

#include "flight_core/flight_core.hpp"
#include "ota/gate.hpp"

namespace mark4
{
    /// The gate of a flight composition: the flight core answers for the
    /// motors. This is the one header of the component that knows the core
    /// exists, so a node without one carries the provider all the same.
    class FlightOtaGate final : public AbsOtaGate
    {
      public:
        /// @param core flight core read for the arming state; must outlive
        ///        the gate
        explicit FlightOtaGate(const FlightCore &core)
            : m_core(core)
        {
        }

        /// @return true while the core is armed
        [[nodiscard]] bool armed() const override
        {
            return m_core.armed();
        }

        /// @return true, always, for now
        [[nodiscard]] bool voltageOk() const override
        {
            // TODO(tmagne): read the real pack voltage here. mark1 has no
            // battery sense at all, so the voltage floor of
            // docs/ota-design.md section 3.2 cannot be enforced yet; the AIO
            // board brings the divider that makes it measurable. A desktop
            // process has no pack behind its store and always passes.
            return true;
        }

      private:
        const FlightCore &m_core; ///< arming state read per request, not owned
    };
} // namespace mark4
