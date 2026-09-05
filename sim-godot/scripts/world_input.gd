class_name WorldInput
extends Node

## Keyboard and mouse of the viewer: which drone to look at, from where.
##
## Nothing here reaches a drone. The plant is not the cockpit: the kill
## switch, the arm switch and the sticks are RC and travel between the
## cockpit and the flight process; resets and throws are scenarios the
## console and the phone send through the flight process. What is left to
## the viewer is the view, and this node turns its keys into signals the
## drone manager and the camera rig listen to.
##
## Bindings are input actions of project.godot: TAB cycles the followed
## drone, C cycles the camera mode, ESC quits. The mouse is read here: a
## left click picks the drone under the cursor, the wheel zooms.

## Cycle to the next drone.
signal next_drone_requested
## Cycle to the next camera mode.
signal camera_mode_requested
## Zoom by that many wheel steps, positive away from the drone.
signal zoom_requested(steps: int)
## Pick the drone nearest to that screen position.
signal pick_requested(screen_position: Vector2)

const ACTION_NEXT_DRONE := &"sim_next_drone"
const ACTION_CAMERA_MODE := &"sim_camera_mode"
const ACTION_QUIT := &"sim_quit"


func _unhandled_input(event: InputEvent) -> void:
	if event.is_action_pressed(ACTION_QUIT):
		get_tree().quit()
	elif event.is_action_pressed(ACTION_NEXT_DRONE):
		next_drone_requested.emit()
	elif event.is_action_pressed(ACTION_CAMERA_MODE):
		camera_mode_requested.emit()
	elif event is InputEventMouseButton and event.pressed:
		var mouse := event as InputEventMouseButton
		match mouse.button_index:
			MOUSE_BUTTON_LEFT:
				pick_requested.emit(mouse.position)
			MOUSE_BUTTON_WHEEL_UP:
				zoom_requested.emit(-1)
			MOUSE_BUTTON_WHEEL_DOWN:
				zoom_requested.emit(1)
			_:
				return
	else:
		return
	get_viewport().set_input_as_handled()
