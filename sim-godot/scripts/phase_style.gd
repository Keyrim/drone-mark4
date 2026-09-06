class_name PhaseStyle
extends RefCounted

## Colors and short names of the flight phases, shared by the overlay and
## the status light of the drone model so the two always agree.
##
## The phases are the ones of the wire (Mark4.FlightPhase); an unknown value
## gets a neutral gray and its number, so a schema ahead of this project
## still displays something.

const Mark4 := preload("res://scripts/gen/mark4.gd")

## Color of a phase this project does not know, and of "no status yet".
const UNKNOWN_COLOR := Color(0.45, 0.48, 0.52)


## Display color of a flight phase.
static func color(phase: int) -> Color:
	match phase:
		Mark4.FlightPhase.PHASE_IDLE:
			return Color(0.55, 0.58, 0.62)
		Mark4.FlightPhase.PHASE_ARMED:
			return Color(0.98, 0.72, 0.20)
		Mark4.FlightPhase.PHASE_BALLISTIC:
			return Color(0.30, 0.80, 0.98)
		Mark4.FlightPhase.PHASE_RECOVERY:
			return Color(0.70, 0.50, 1.00)
		Mark4.FlightPhase.PHASE_HOVER:
			return Color(0.35, 0.85, 0.50)
		Mark4.FlightPhase.PHASE_ALTITUDE_AUTO:
			return Color(0.25, 0.80, 0.75)
		Mark4.FlightPhase.PHASE_MANUAL:
			return Color(0.35, 0.60, 1.00)
		Mark4.FlightPhase.PHASE_LEVEL:
			return Color(0.55, 0.70, 1.00)
		Mark4.FlightPhase.PHASE_CUTOFF:
			return Color(0.90, 0.40, 0.20)
		Mark4.FlightPhase.PHASE_FAULT:
			return Color(0.95, 0.25, 0.25)
		_:
			return UNKNOWN_COLOR


## Short lowercase name of a flight phase.
static func label(phase: int) -> String:
	match phase:
		Mark4.FlightPhase.PHASE_IDLE:
			return "idle"
		Mark4.FlightPhase.PHASE_ARMED:
			return "armed"
		Mark4.FlightPhase.PHASE_BALLISTIC:
			return "ballistic"
		Mark4.FlightPhase.PHASE_RECOVERY:
			return "recovery"
		Mark4.FlightPhase.PHASE_HOVER:
			return "hover"
		Mark4.FlightPhase.PHASE_ALTITUDE_AUTO:
			return "altitude auto"
		Mark4.FlightPhase.PHASE_MANUAL:
			return "manual"
		Mark4.FlightPhase.PHASE_LEVEL:
			return "level"
		Mark4.FlightPhase.PHASE_CUTOFF:
			return "cutoff"
		Mark4.FlightPhase.PHASE_FAULT:
			return "fault"
		_:
			return "phase %d" % phase


## True for the phases flown from the sticks.
static func is_piloted(phase: int) -> bool:
	return phase == Mark4.FlightPhase.PHASE_MANUAL or phase == Mark4.FlightPhase.PHASE_LEVEL
