extends Node

## Command line configuration for scripted runs (batch campaigns).
##
## Parses the user arguments placed after the `--` separator, e.g.:
##   godot --headless --path sim-godot -- --flight-port 48000 \
##     --command-port 48004 --raw-port 48002 --lockstep --time-scale 20
##
## Interactive sessions pass no user arguments: every script keeps its
## inspector defaults. Registered as an autoload, so the values are parsed
## before any scene node reads them in _ready.

var _values: Dictionary = {}
var _flags: Dictionary = {}


func _enter_tree() -> void:
	var args := OS.get_cmdline_user_args()
	var index := 0
	while index < args.size():
		var arg := args[index]
		if arg.begins_with("--"):
			var key := arg.trim_prefix("--")
			if index + 1 < args.size() and not args[index + 1].begins_with("--"):
				_values[key] = args[index + 1]
				index += 1
			else:
				_flags[key] = true
		index += 1

	if has_flag("lockstep") or _values.has("time-scale"):
		# Batch pacing. time_scale alone would keep the tick rate and GROW
		# the integration step (time_scale / ticks_per_second): the physics
		# would coarsen 20x while the tick-counter clock keeps lying at the
		# nominal rate. Scaling the tick rate by the same factor keeps every
		# step at its nominal duration - there are simply more of them per
		# wall second. The simulated physics stays exact.
		var scale := float(get_value("time-scale", "20"))
		Engine.time_scale = scale
		Engine.physics_ticks_per_second = int(round(Engine.physics_ticks_per_second * scale))
		Engine.max_fps = 0
		Engine.max_physics_steps_per_frame = 1000


## Value of --key, or the default when the argument is absent.
func get_value(key: String, default_value: String) -> String:
	return _values.get(key, default_value)


## Integer value of --key, for the port overrides.
func get_port(key: String, default_port: int) -> int:
	return int(get_value(key, str(default_port)))


## True when --key was passed without a value.
func has_flag(key: String) -> bool:
	return _flags.has(key)
