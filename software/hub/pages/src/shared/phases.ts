/**
 * The flight core states as words. Two enums live on the wire as numbers
 * (FlightPhase, ThrowState) and every page that shows one shows the same
 * word for it, so the names live in one place.
 */

/** mark4.FlightPhase, by value. */
export const FLIGHT_PHASE_NAMES = [
    "idle",
    "altitude auto",
    "armed",
    "ballistic",
    "recovery",
    "hover",
    "cutoff",
    "manual",
    "fault",
    "level",
];

/** mark4.ThrowState, by value. */
export const THROW_STATE_NAMES = ["idle", "thrust", "ballistic"];
