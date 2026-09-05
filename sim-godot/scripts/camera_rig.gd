class_name CameraRig
extends Camera3D

## The viewer's camera: four ways of looking at the followed drone.
##
## - CHASE trails the drone from a fixed world offset and keeps it framed.
##   The offset is in world axes on purpose: the view stays readable while
##   the drone tumbles, which is what a throw looks like.
## - FOLLOW sits behind and above the drone, aligned on its heading (yaw
##   only, never its roll or pitch), the third person view of a piloted
##   flight.
## - LOS stands where a pilot would, at a fixed spot on the ground, and
##   turns to keep the drone in sight, its field of view tightening with the
##   distance: what a line of sight flight actually looks like.
## - FPV rides the drone, tilted up like a flight camera.
##
## Purely cosmetic, so it runs in _process and never touches the physics
## state. The last mode chosen for a drone is remembered while it lives, and
## a drone entering a piloted phase pulls the camera from CHASE to FOLLOW
## once, unless a mode was chosen by hand for it.

## The mode changed, by key or on its own.
signal mode_changed(mode: Mode)

enum Mode { CHASE, FOLLOW, LOS, FPV }

## Display names, indexed by Mode.
const MODE_NAMES: Array[String] = ["chase", "follow", "los", "fpv"]

## Smoothing rate of the chase and follow positions, higher is stiffer [1/s].
const SMOOTHING_RATE := 4.0
## Smoothing rate of the line of sight turn [1/s].
const LOS_TURN_RATE := 8.0
## Half height of the frame the line of sight view keeps around the drone [m].
const LOS_FRAME_M := 1.2
const LOS_FOV_MIN_DEG := 12.0
const LOS_FOV_MAX_DEG := 70.0
## Below that distance look_at has no usable direction.
const MIN_LOOK_DISTANCE_M := 0.01
## Horizontal speed of the heading below which the follow view keeps its
## last heading rather than swinging around a hovering drone [unitless].
const MIN_HEADING_COMPONENT := 0.15
## Zoom range and step, as a multiplier of the configured distances.
const ZOOM_MIN := 0.25
const ZOOM_MAX := 4.0
const ZOOM_STEP := 1.15

## Source of the mode and zoom keys.
@export var input_path: NodePath

@export_group("Chase")
## World offset from the drone to the camera [m].
@export var chase_offset: Vector3 = Vector3(0.0, 0.6, 1.8)

@export_group("Follow")
## Distance behind the drone, along its heading [m].
@export var follow_distance_m: float = 1.6
## Height above the drone [m].
@export var follow_height_m: float = 0.6

@export_group("Line of sight")
## Where the pilot stands [m], world axes.
@export var pilot_position: Vector3 = Vector3(0.0, 1.7, 7.0)

@export_group("FPV")
## Upward tilt of the flight camera [deg].
@export var fpv_tilt_deg: float = 20.0
## Field of view of the flight camera [deg].
@export var fpv_fov_deg: float = 100.0

var mode: Mode = Mode.CHASE

var _target: Node3D = null
var _zoom: float = 1.0
var _default_fov_deg: float = 75.0
var _heading: Vector3 = Vector3.FORWARD
var _los_fov_deg: float = 40.0
## Target instance id -> [Mode, chosen by hand], remembered while it lives.
var _remembered: Dictionary = {}
var _hand_chosen: bool = false
var _was_piloted: bool = false


func _ready() -> void:
	_default_fov_deg = fov
	var input := get_node_or_null(input_path) as WorldInput
	if input != null:
		input.camera_mode_requested.connect(cycle_mode)
		input.zoom_requested.connect(zoom_by)


## Follow another node; null parks the camera where it is.
func set_target(target: Node3D) -> void:
	if _target != null and is_instance_valid(_target):
		_remembered[_target.get_instance_id()] = [mode, _hand_chosen]
	_target = target
	_hand_chosen = false
	_was_piloted = false
	var next := Mode.CHASE
	if target != null:
		var remembered: Array = _remembered.get(target.get_instance_id(), [])
		if not remembered.is_empty():
			next = remembered[0]
			_hand_chosen = remembered[1]
		_heading = _horizontal_heading(target.global_transform.basis, _heading)
	_set_mode(next)


## Forget a drone that left the world.
func forget(target: Node3D) -> void:
	_remembered.erase(target.get_instance_id())


## Next mode, by hand.
func cycle_mode() -> void:
	_hand_chosen = true
	_set_mode((mode + 1) % Mode.size() as Mode)


## Move the chase and follow views by that many wheel steps.
func zoom_by(steps: int) -> void:
	_zoom = clampf(_zoom * pow(ZOOM_STEP, steps), ZOOM_MIN, ZOOM_MAX)


func _set_mode(next: Mode) -> void:
	var changed := next != mode
	mode = next
	if mode != Mode.FPV and mode != Mode.LOS:
		fov = _default_fov_deg
	if changed:
		mode_changed.emit(mode)


func _process(delta: float) -> void:
	if _target == null or not is_instance_valid(_target):
		return
	_auto_follow()
	var target_position := _target.global_position
	var weight := 1.0 - exp(-SMOOTHING_RATE * delta)
	match mode:
		Mode.CHASE:
			_approach(target_position + chase_offset * _zoom, weight)
			_look(target_position)
		Mode.FOLLOW:
			_heading = _horizontal_heading(_target.global_transform.basis, _heading)
			var behind := target_position - _heading * follow_distance_m * _zoom
			behind.y += follow_height_m * _zoom
			_approach(behind, weight)
			_look(target_position)
		Mode.LOS:
			global_position = pilot_position
			var line := target_position - pilot_position
			if line.length() > MIN_LOOK_DISTANCE_M:
				var wanted := Basis.looking_at(line, Vector3.UP)
				var turn := 1.0 - exp(-LOS_TURN_RATE * delta)
				global_transform.basis = global_transform.basis.orthonormalized().slerp(wanted, turn)
				var wanted_fov := rad_to_deg(2.0 * atan(LOS_FRAME_M / line.length()))
				_los_fov_deg = lerpf(_los_fov_deg, clampf(wanted_fov, LOS_FOV_MIN_DEG, LOS_FOV_MAX_DEG), turn)
				fov = _los_fov_deg
		Mode.FPV:
			var tilt := Basis(Vector3.RIGHT, deg_to_rad(fpv_tilt_deg))
			global_transform = _target.global_transform * Transform3D(tilt, Vector3(0.0, 0.03, -0.04))
			fov = fpv_fov_deg


## A drone entering a piloted phase pulls a chase camera behind it, once.
func _auto_follow() -> void:
	var drone := _target as Drone
	if drone == null:
		return
	var piloted := drone.sim_link.status_fresh() and PhaseStyle.is_piloted(drone.sim_link.phase)
	if piloted and not _was_piloted and mode == Mode.CHASE and not _hand_chosen:
		_set_mode(Mode.FOLLOW)
	_was_piloted = piloted


func _approach(wanted: Vector3, weight: float) -> void:
	global_position = global_position.lerp(wanted, weight)


func _look(target_position: Vector3) -> void:
	if global_position.distance_to(target_position) > MIN_LOOK_DISTANCE_M:
		look_at(target_position, Vector3.UP)


## Heading of a body, projected on the ground; the previous one when the
## body points too close to the vertical for the projection to mean much.
static func _horizontal_heading(body: Basis, previous: Vector3) -> Vector3:
	var forward := -body.z
	forward.y = 0.0
	if forward.length() < MIN_HEADING_COMPONENT:
		return previous
	return forward.normalized()
