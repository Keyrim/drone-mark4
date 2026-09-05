class_name DroneTrail
extends MeshInstance3D

## Ribbon of the recent positions of a drone, fading with age.
##
## A throw, a recovery or a piloted turn reads at a glance from the path it
## leaves in the air. Points are sampled at a fixed period while the drone
## moves, kept for a few seconds and dropped; the ribbon is rebuilt every
## frame as a triangle strip facing the camera, so it stays visible from any
## angle. The mesh lives in world space (top_level), not in the body frame.
##
## Cosmetic: fed from _process by the drone view, never by the physics.

## Period between two samples [s].
const SAMPLE_PERIOD_S := 0.02
## Age at which a sample vanishes [s].
const LIFETIME_S := 3.0
## Below that speed nothing is sampled: a resting drone leaves no dot.
const MIN_SPEED_MPS := 0.3
## Half width of the ribbon [m].
const HALF_WIDTH_M := 0.008
## Opacity of the newest sample.
const MAX_ALPHA := 0.7

## Color of the ribbon, set by the view.
var tint: Color = Color(0.30, 0.80, 0.98)

var _points := PackedVector3Array()
var _ages := PackedFloat32Array()
var _since_sample_s: float = 0.0
var _mesh := ImmediateMesh.new()


func _ready() -> void:
	top_level = true
	transform = Transform3D.IDENTITY
	mesh = _mesh
	cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.vertex_color_use_as_albedo = true
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material_override = material


## Forget the path: a reset teleports the body, nothing joins the two.
func clear() -> void:
	_points.clear()
	_ages.clear()
	_mesh.clear_surfaces()


## Advance the ribbon by one frame.
func update(position: Vector3, velocity: Vector3, delta: float, camera_position: Vector3) -> void:
	for index: int in _ages.size():
		_ages[index] += delta
	while not _ages.is_empty() and _ages[0] > LIFETIME_S:
		_ages.remove_at(0)
		_points.remove_at(0)
	_since_sample_s += delta
	if _since_sample_s >= SAMPLE_PERIOD_S:
		_since_sample_s = 0.0
		if velocity.length() > MIN_SPEED_MPS:
			_points.append(position)
			_ages.append(0.0)
	_rebuild(camera_position)


func _rebuild(camera_position: Vector3) -> void:
	_mesh.clear_surfaces()
	var count := _points.size()
	if count < 2:
		return
	_mesh.surface_begin(Mesh.PRIMITIVE_TRIANGLE_STRIP)
	for index: int in count:
		var point := _points[index]
		var along := _points[mini(index + 1, count - 1)] - _points[maxi(index - 1, 0)]
		var side := along.cross(camera_position - point)
		if side.length_squared() < 1e-9:
			side = Vector3.UP
		side = side.normalized() * HALF_WIDTH_M
		var alpha := MAX_ALPHA * (1.0 - _ages[index] / LIFETIME_S)
		_mesh.surface_set_color(Color(tint, alpha))
		_mesh.surface_add_vertex(point - side)
		_mesh.surface_set_color(Color(tint, alpha))
		_mesh.surface_add_vertex(point + side)
	_mesh.surface_end()
