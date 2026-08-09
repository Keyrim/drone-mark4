class_name AttitudeCompare
extends Node

## Picture-in-picture comparison of the exact drone attitude against the one
## estimated by the flight process, decoded from the telemetry broadcast.
##
## A small viewport in the top right corner shows two gizmos rotating in
## place, orientation only: the real drone (solid) and the estimate (ghost)
## superimposed. Any divergence between the two is directly visible.
##
## The telemetry wire layout is defined by protocol/include/protocol/
## telemetry.hpp; only the attitude quaternion is read here. The quaternion
## travels in the drone frame convention of the protocol and is remapped to
## the Godot axes for display.

## Axis remap from the Godot frame to the drone frame: columns are the drone
## coordinates of the Godot x, y and z axes.
const GODOT_TO_DRONE := Basis(Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(-1, 0, 0))

const VIEW_SIZE := Vector2(240.0, 240.0)
const VIEW_MARGIN := 12.0
const CAMERA_POSITION := Vector3(0.5, 0.4, 0.5)

@export_group("Endpoint")
## UDP port of the telemetry mirror, TELEMETRY_MIRROR_PORT in the headers.
## Godot binds ports exclusively (no SO_REUSEADDR), so this view listens to
## the mirror of the broadcast and leaves udp/47801 to the other tools.
@export var telemetry_port: int = 47803

@export_group("Scene")
## The rigid body whose real attitude is mirrored by the solid gizmo.
@export var drone_path: NodePath

## Number of valid telemetry packets decoded.
var packets_received: int = 0
## Number of datagrams rejected because of their size, version or content.
var packets_dropped: int = 0

var _socket := PacketPeerUDP.new()
var _drone: Node3D = null
var _real_gizmo: Node3D
var _estimated_gizmo: Node3D


func _ready() -> void:
	if DisplayServer.get_name() == "headless":
		# Nothing to draw, and the exclusive bind on the mirror port would
		# collide across the instances of a batch campaign.
		return
	_drone = get_node_or_null(drone_path)
	if _drone == null:
		push_error("attitude compare: drone_path does not resolve, view disabled")
		return
	var bind_error := _socket.bind(telemetry_port, "0.0.0.0")
	if bind_error != OK:
		push_error(
			"attitude compare: cannot bind udp/%d (error %d)" % [telemetry_port, bind_error]
		)
		return
	_build_view()
	print("attitude compare: listening on udp/%d" % telemetry_port)


func _exit_tree() -> void:
	_socket.close()


func _process(_delta: float) -> void:
	if _drone == null or _real_gizmo == null:
		return
	while _socket.get_available_packet_count() > 0:
		if _apply_estimate(_socket.get_packet()):
			packets_received += 1
		else:
			packets_dropped += 1
	_real_gizmo.basis = _drone.global_transform.basis


func _apply_estimate(payload: PackedByteArray) -> bool:
	if payload.size() != Protocol.TELEMETRY_PACKET_SIZE:
		return false
	if not Protocol.has_header(payload, Protocol.TYPE_TELEMETRY):
		return false
	var w := payload.decode_float(Protocol.TELEMETRY_QUAT_OFFSET)
	var x := payload.decode_float(Protocol.TELEMETRY_QUAT_OFFSET + Protocol.FLOAT_SIZE)
	var y := payload.decode_float(Protocol.TELEMETRY_QUAT_OFFSET + 2 * Protocol.FLOAT_SIZE)
	var z := payload.decode_float(Protocol.TELEMETRY_QUAT_OFFSET + 3 * Protocol.FLOAT_SIZE)
	var estimated := Quaternion(x, y, z, w)
	if not estimated.is_finite() or estimated.length_squared() < 0.5:
		return false
	estimated = estimated.normalized()

	var to_drone := Quaternion(GODOT_TO_DRONE)
	_estimated_gizmo.basis = Basis(to_drone.inverse() * estimated * to_drone)
	return true


func _build_view() -> void:
	var layer := CanvasLayer.new()
	add_child(layer)

	var container := SubViewportContainer.new()
	container.stretch = true
	container.anchor_left = 1.0
	container.anchor_right = 1.0
	container.anchor_top = 0.0
	container.anchor_bottom = 0.0
	container.offset_left = -(VIEW_SIZE.x + VIEW_MARGIN)
	container.offset_right = -VIEW_MARGIN
	container.offset_top = VIEW_MARGIN
	container.offset_bottom = VIEW_MARGIN + VIEW_SIZE.y
	layer.add_child(container)

	var viewport := SubViewport.new()
	viewport.own_world_3d = true
	viewport.transparent_bg = true
	container.add_child(viewport)

	var camera := Camera3D.new()
	viewport.add_child(camera)
	camera.look_at_from_position(CAMERA_POSITION, Vector3.ZERO, Vector3.UP)

	var light := DirectionalLight3D.new()
	viewport.add_child(light)
	light.rotation_degrees = Vector3(-50.0, -30.0, 0.0)

	_real_gizmo = _make_gizmo(Color(0.25, 0.55, 0.95), 1.0)
	viewport.add_child(_real_gizmo)
	_estimated_gizmo = _make_gizmo(Color(0.95, 0.55, 0.15), 0.5)
	viewport.add_child(_estimated_gizmo)


## A drone-like marker: flat frame, nose block showing forward (-z in Godot),
## mast showing up (+y). Enough asymmetry to read any orientation.
func _make_gizmo(color: Color, alpha: float) -> Node3D:
	var root := Node3D.new()
	root.add_child(_make_part(Vector3(0.34, 0.04, 0.34), Vector3.ZERO, color, alpha))
	root.add_child(
		_make_part(Vector3(0.1, 0.04, 0.14), Vector3(0.0, 0.0, -0.24), color.lightened(0.4), alpha)
	)
	root.add_child(
		_make_part(Vector3(0.05, 0.16, 0.05), Vector3(0.0, 0.1, 0.0), color.darkened(0.25), alpha)
	)
	return root


func _make_part(size: Vector3, local_position: Vector3, color: Color, alpha: float) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(color, alpha)
	if alpha < 1.0:
		material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mesh.material = material
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.position = local_position
	return instance
