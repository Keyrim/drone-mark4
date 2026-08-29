class_name FollowCamera
extends Camera3D

## Camera that trails the drone from a fixed world offset and keeps it framed.
##
## Purely cosmetic, so it runs in _process and never touches the physics state.
## The offset is expressed in world axes on purpose: the view stays readable
## while the drone tumbles.

## Smoothing rate, higher is stiffer [1/s].
const SMOOTHING_RATE := 4.0
## Below that distance look_at has no usable direction.
const MIN_LOOK_DISTANCE_M := 0.01

## Node the camera follows.
@export var target_path: NodePath
## World offset from the target to the camera [m].
@export var offset: Vector3 = Vector3(0.0, 1.0, 3.0)

var _target: Node3D = null


func _ready() -> void:
	# Usually empty: the DroneManager hands the followed drone over with
	# set_target() once a flight process shows up.
	if not target_path.is_empty():
		_target = get_node_or_null(target_path) as Node3D


## Follow another node; null parks the camera where it is.
func set_target(target: Node3D) -> void:
	_target = target


func _process(delta: float) -> void:
	if _target == null:
		return
	var target_position := _target.global_position
	var weight := 1.0 - exp(-SMOOTHING_RATE * delta)
	global_position = global_position.lerp(target_position + offset, weight)
	if global_position.distance_to(target_position) > MIN_LOOK_DISTANCE_M:
		look_at(target_position, Vector3.UP)
