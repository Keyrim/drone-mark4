class_name PilotInput
extends Node

## Keyboard pilot.
##
## Holds the two RC fields that travel in every sensor packet (kill switch and
## throttle) and turns the throw and reset keys into one-shot requests that the
## drone consumes on its next physics tick. This node never touches the physics
## state itself, so key presses are always applied at a tick boundary.
##
## Key bindings are declared as input actions in project.godot:
## K toggles the kill switch, A toggles the arm switch, Up and Down nudge the
## throttle, SPACE throws, R resets and ESC quits.

## Throttle increment applied on each Up or Down key press.
const THROTTLE_STEP := 0.05

const ACTION_KILL_TOGGLE := &"sim_kill_toggle"
const ACTION_ARM_TOGGLE := &"sim_arm_toggle"
const ACTION_THROTTLE_UP := &"sim_throttle_up"
const ACTION_THROTTLE_DOWN := &"sim_throttle_down"
const ACTION_THROW := &"sim_throw"
const ACTION_RESET := &"sim_reset"
const ACTION_QUIT := &"sim_quit"

## Kill switch state sent in the sensor packet, true means engaged (motors cut).
## Starts released so the flight process runs its normal loop.
var kill_switch: bool = false

## Normalized RC throttle sent in the sensor packet, in [0, 1].
var throttle: float = 0.0

## Arm switch state sent in the sensor packet, true means the flight process
## may fly on its own after a throw.
var arm_switch: bool = false

var _throw_requested: bool = false
var _reset_requested: bool = false


func _unhandled_input(event: InputEvent) -> void:
	if event.is_action_pressed(ACTION_QUIT):
		get_tree().quit()
	elif event.is_action_pressed(ACTION_KILL_TOGGLE):
		kill_switch = not kill_switch
	elif event.is_action_pressed(ACTION_ARM_TOGGLE):
		arm_switch = not arm_switch
	elif event.is_action_pressed(ACTION_THROTTLE_UP):
		throttle = clampf(throttle + THROTTLE_STEP, 0.0, 1.0)
	elif event.is_action_pressed(ACTION_THROTTLE_DOWN):
		throttle = clampf(throttle - THROTTLE_STEP, 0.0, 1.0)
	elif event.is_action_pressed(ACTION_THROW):
		_throw_requested = true
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


## Return true once per R press, then clear the request.
func take_reset_request() -> bool:
	var requested := _reset_requested
	_reset_requested = false
	return requested


## Human readable kill switch state for the overlay.
func kill_switch_text() -> String:
	return "ENGAGED" if kill_switch else "released"


## Human readable arm switch state for the overlay.
func arm_switch_text() -> String:
	return "ARMED" if arm_switch else "off"
