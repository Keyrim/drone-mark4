class_name SimHud
extends Label

## On screen overlay: simulated time, pilot inputs, motor commands and the
## headline sensor values. Refreshed a few times per second, which is enough to
## read it and cheap enough to stay out of the way of the physics.

## Delay between two overlay updates [s].
const REFRESH_PERIOD_S := 0.1

## Drone the overlay reads its values from.
@export var drone_path: NodePath

var _drone: Drone = null
var _elapsed_s: float = 0.0


func _ready() -> void:
	if not drone_path.is_empty():
		_drone = get_node_or_null(drone_path) as Drone
	if _drone == null:
		push_warning("overlay: no drone to watch")
	_refresh()


func _process(delta: float) -> void:
	_elapsed_s += delta
	if _elapsed_s < REFRESH_PERIOD_S:
		return
	_elapsed_s = 0.0
	_refresh()


func _refresh() -> void:
	if _drone == null:
		text = "no drone"
		return
	var link := _drone.sim_link
	var lines := PackedStringArray()
	lines.append("sim time   %8.3f s" % _drone.simulated_time_s())
	lines.append("kill       %s" % _drone.pilot.kill_switch_text())
	lines.append("arm        %s" % _drone.pilot.arm_switch_text())
	lines.append("throttle   %.2f" % _drone.pilot.throttle)
	lines.append("motors     %s" % link.motor_commands_text())
	lines.append("accel      %6.2f g" % _drone.sensors.accel_magnitude_g())
	lines.append("altitude   %6.2f m" % _drone.altitude_m())
	lines.append(
		(
			"link       sent %d  received %d  dropped %d"
			% [link.packets_sent, link.packets_received, link.packets_dropped]
		)
	)
	if link.lockstep:
		lines.append("lockstep   on, %d timeouts" % link.lockstep_timeouts)
	lines.append("keys       K kill  A arm  Up/Down throttle  H hold  SPACE throw  R reset  ESC quit")
	text = "\n".join(lines)
