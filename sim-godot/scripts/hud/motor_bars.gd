class_name MotorBars
extends Control

## Four vertical bars: the motor commands the flight process sent, filled,
## and the effective speeds after the motor lag as a marker on each bar.

const BAR_WIDTH_PX := 10.0
const GAP_PX := 4.0
const HEIGHT_PX := 30.0
const MARKER_PX := 2.0

var _commands := PackedFloat32Array([0.0, 0.0, 0.0, 0.0])
var _speeds := PackedFloat32Array([0.0, 0.0, 0.0, 0.0])


func _init() -> void:
	custom_minimum_size = Vector2(4.0 * BAR_WIDTH_PX + 3.0 * GAP_PX, HEIGHT_PX)
	mouse_filter = Control.MOUSE_FILTER_IGNORE


## Show these commands and speeds, both in [0, 1].
func set_levels(commands: PackedFloat32Array, speeds: PackedFloat32Array) -> void:
	_commands = commands.duplicate()
	_speeds = speeds.duplicate()
	queue_redraw()


func _draw() -> void:
	var height := size.y
	for index: int in 4:
		var x := index * (BAR_WIDTH_PX + GAP_PX)
		draw_rect(Rect2(x, 0.0, BAR_WIDTH_PX, height), HudStyle.TRACK)
		var fill := clampf(_commands[index], 0.0, 1.0) * height
		draw_rect(Rect2(x, height - fill, BAR_WIDTH_PX, fill), HudStyle.ACCENT)
		var marker := height - clampf(_speeds[index], 0.0, 1.0) * height
		draw_rect(Rect2(x, clampf(marker - MARKER_PX * 0.5, 0.0, height - MARKER_PX), BAR_WIDTH_PX, MARKER_PX), HudStyle.TEXT)
