class_name SimHud
extends Label

## On screen overlay: simulated time, link health, motor commands and the
## headline sensor values of the followed drone, plus how many drones the
## plant hosts. Refreshed a few times per second, which is enough to read it
## and cheap enough to stay out of the way of the physics.
##
## No pilot state here: this project holds none. Kill, arm and throttle live
## on the RC path, between the cockpit and the flight process.

## Delay between two overlay updates [s].
const REFRESH_PERIOD_S := 0.1

## Manager whose followed drone the overlay reads.
@export var drones_path: NodePath

var _drones: DroneManager = null
var _elapsed_s: float = 0.0


func _ready() -> void:
	if not drones_path.is_empty():
		_drones = get_node_or_null(drones_path) as DroneManager
	if _drones == null:
		push_warning("overlay: no drone manager to watch")
	_refresh()


func _process(delta: float) -> void:
	_elapsed_s += delta
	if _elapsed_s < REFRESH_PERIOD_S:
		return
	_elapsed_s = 0.0
	_refresh()


func _refresh() -> void:
	if _drones == null:
		text = "no drone manager"
		return
	var lines := PackedStringArray()
	lines.append("plant      node %08x  wire %08x" % [_drones.transport.node_id, WireHash.VALUE])
	lines.append("drones     %d" % _drones.drones.size())
	var drone := _drones.followed
	if drone == null:
		lines.append("no flight process yet: start a drone_sim")
		lines.append("keys       ESC quit")
		text = "\n".join(lines)
		return
	var link := drone.sim_link
	lines.append("following  %08x" % link.flight_node)
	lines.append("sim time   %8.3f s" % drone.simulated_time_s())
	lines.append("motors     %s" % link.motor_commands_text())
	lines.append("accel      %6.2f g" % drone.sensors.accel_magnitude_g())
	lines.append("altitude   %6.2f m" % drone.altitude_m())
	lines.append(
		(
			"link       sent %d  received %d  dropped %d"
			% [link.packets_sent, link.packets_received, link.packets_dropped]
		)
	)
	lines.append("timeouts   %d lockstep" % link.lockstep_timeouts)
	if link.lockstep:
		lines.append("lockstep   on")
	lines.append("keys       H hold  SPACE throw  R reset  ESC quit")
	text = "\n".join(lines)
