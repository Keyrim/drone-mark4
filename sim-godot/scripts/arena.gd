class_name Arena
extends StaticBody3D

## Circular wall around the launch point, for "recover within X meters" runs.
##
## A ring of box segments is built procedurally at the configured radius:
## collision and mesh both, so a drone drifting too far slams into it and the
## impact cutoff turns the overshoot into a measurable failure. A radius of
## zero (the default) builds nothing.
##
## The wall is always translucent, and the segments crossing the camera to
## drone sightline fade almost out, so the view is never blocked. The fade is
## a pure geometry test against the sightline, cheap enough to run per frame.

## Wall distance from the launch point [m]; 0 disables the arena.
@export var radius_m: float = 0.0

## Wall height [m].
@export var height_m: float = 4.0

## Number of box segments approximating the circle.
@export var segment_count: int = 24

## Camera and subject of the occlusion fade.
@export var camera_path: NodePath
@export var target_path: NodePath

const WALL_COLOR := Color(0.85, 0.5, 0.15)
const WALL_ALPHA := 0.2
const OCCLUDED_ALPHA := 0.05
const WALL_THICKNESS_M := 0.1

var _segments: Array[MeshInstance3D] = []
var _materials: Array[StandardMaterial3D] = []
var _chord_m: float = 0.0
var _camera: Node3D = null
var _target: Node3D = null


func _ready() -> void:
	radius_m = float(SimArgs.get_value("arena-radius", str(radius_m)))
	if radius_m <= 0.0:
		set_process(false)
		return
	_camera = get_node_or_null(camera_path)
	_target = get_node_or_null(target_path)
	_build()
	print("arena: wall ring at %.1f m" % radius_m)


## Fade the segments crossing the sightline to another node; null fades none.
func set_target(target: Node3D) -> void:
	_target = target


func _process(_delta: float) -> void:
	if _camera == null or _target == null:
		return
	var cam := _camera.global_position
	var target := _target.global_position
	for index: int in _segments.size():
		var occluded := _occludes(cam, target, _segments[index].global_position)
		_materials[index].albedo_color.a = OCCLUDED_ALPHA if occluded else WALL_ALPHA


func _build() -> void:
	# Chords slightly longer than the exact arc, so the ring stays closed.
	_chord_m = 2.0 * PI * radius_m / segment_count * 1.05
	for index: int in segment_count:
		var angle := TAU * float(index) / segment_count
		var center := Vector3(radius_m * cos(angle), height_m * 0.5, radius_m * sin(angle))

		var material := StandardMaterial3D.new()
		material.albedo_color = Color(WALL_COLOR, WALL_ALPHA)
		material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		material.cull_mode = BaseMaterial3D.CULL_DISABLED
		_materials.append(material)

		var mesh := BoxMesh.new()
		mesh.size = Vector3(WALL_THICKNESS_M, height_m, _chord_m)
		mesh.material = material
		var instance := MeshInstance3D.new()
		instance.mesh = mesh
		instance.position = center
		instance.rotation.y = -angle
		add_child(instance)
		_segments.append(instance)

		var shape := BoxShape3D.new()
		shape.size = mesh.size
		var collider := CollisionShape3D.new()
		collider.shape = shape
		collider.position = center
		collider.rotation.y = -angle
		add_child(collider)


## True when the segment centered there sits close to the camera-target
## sightline, between the two: near the crossing point of the ring.
func _occludes(cam: Vector3, target: Vector3, center: Vector3) -> bool:
	var line := target - cam
	var length_sq := line.length_squared()
	if length_sq < 1e-6:
		return false
	var along := clampf((center - cam).dot(line) / length_sq, 0.0, 1.0)
	var closest := cam + line * along
	return closest.distance_to(center) < _chord_m
