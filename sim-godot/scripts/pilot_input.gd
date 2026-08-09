class_name PilotInput
extends Node

## Keyboard scenario keys.
##
## Turns the world keys into one-shot requests the drone consumes on its next
## physics tick. This node never touches the physics state itself, so key
## presses are always applied at a tick boundary.
##
## Piloting is deliberately absent: the kill switch, the arm switch and the
## throttle are RC, they reach the flight process out-of-band on the exact
## path a real flight uses, and this project is the plant, not the cockpit.
## Fly from the hub.
##
## Key bindings are declared as input actions in project.godot: H picks the
## drone up in the simulated hand, SPACE throws (a hand throw when held, an
## instant throw otherwise), R resets the world and ESC quits.

const ACTION_THROW := &"sim_throw"
const ACTION_GRAB := &"sim_grab"
const ACTION_RESET := &"sim_reset"
const ACTION_QUIT := &"sim_quit"

var _throw_requested: bool = false
var _grab_requested: bool = false
var _reset_requested: bool = false


func _unhandled_input(event: InputEvent) -> void:
	if event.is_action_pressed(ACTION_QUIT):
		get_tree().quit()
	elif event.is_action_pressed(ACTION_THROW):
		_throw_requested = true
	elif event.is_action_pressed(ACTION_GRAB):
		_grab_requested = true
	elif event.is_action_pressed(ACTION_RESET):
		_reset_requested = true
	else:
		return
	get_viewport().set_input_as_handled()


## Return true once per SPACE press, then clear the request.
func take_throw_request() -> bool:
	var requested := _throw_requested
	_throw_requested = false
	return requested


## Return true once per H press, then clear the request.
func take_grab_request() -> bool:
	var requested := _grab_requested
	_grab_requested = false
	return requested


## Return true once per R press, then clear the request.
func take_reset_request() -> bool:
	var requested := _reset_requested
	_reset_requested = false
	return requested
