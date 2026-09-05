class_name DroneView
extends Node3D

## What a virtual drone looks like: the props spinning with the motor
## speeds, a status light colored by the flight phase, a ring on the ground
## under the followed drone, and the trail it leaves in the air.
##
## Everything here is read from the Drone parent in _process and drawn;
## nothing is written back, and nothing runs when the plant is headless (a
## batch campaign has no screen to draw on).

## Visual spin rate of a prop at full speed [rad/s]. Any value aliases at
## screen rates; this one reads as a spinning disc rather than a strobe.
const PROP_MAX_RAD_S := TAU * 25.0
## Spin direction of each prop, opposite to the reaction torque it produces.
const PROP_SPIN_SIGNS: Array[float] = [1.0, -1.0, -1.0, 1.0]
## Motor speed at which the blades are fully blurred into a disc.
const BLUR_FULL_SPEED := 0.15
## Opacity of the blur disc at full speed (1 = opaque).
const DISC_MAX_OPACITY := 0.45
## Height of the follow ring above the ground [m].
const RING_HEIGHT_M := 0.004
## Blink rate of the status light while the RC fail-safe is active [Hz].
const BLINK_HZ := 4.0
## Emission strength of the status light.
const LED_ENERGY := 2.5

var _drone: Drone = null
var _props: Array[Node3D] = []
## Per instance copies of the blade and disc materials: their alpha is the
## blur, which every renderer supports (the instance fade of Forward+ does
## not exist in the compatibility renderer).
var _blade_materials: Array[StandardMaterial3D] = []
var _disc_materials: Array[StandardMaterial3D] = []
var _angles := PackedFloat32Array([0.0, 0.0, 0.0, 0.0])
var _led_material: StandardMaterial3D = null
var _ring: MeshInstance3D = null
var _trail: DroneTrail = null
var _followed: bool = false
var _time_s: float = 0.0


func _ready() -> void:
	_drone = get_parent() as Drone
	if _drone == null or DisplayServer.get_name() == "headless":
		set_process(false)
		return
	for index: int in Drone.MOTOR_COUNT:
		var prop := get_node("Prop%d" % index) as Node3D
		_props.append(prop)
		_blade_materials.append(_own_material(prop.get_node("Blade") as MeshInstance3D))
		_disc_materials.append(_own_material(prop.get_node("Disc") as MeshInstance3D))
		_disc_materials[index].albedo_color.a = 0.0
	var led := $Led as MeshInstance3D
	_led_material = led.get_active_material(0).duplicate() as StandardMaterial3D
	_led_material.emission_energy_multiplier = LED_ENERGY
	led.material_override = _led_material
	_ring = $Ring
	_ring.top_level = true
	_ring.visible = false
	_trail = $Trail
	_drone.run_reset.connect(_trail.clear)


## Mark this drone as the one the camera and the overlay look at.
func set_followed(followed: bool) -> void:
	_followed = followed
	if _ring != null:
		_ring.visible = followed


func _process(delta: float) -> void:
	_time_s += delta
	var speeds := _drone.motor_speeds()
	for index: int in Drone.MOTOR_COUNT:
		var speed := speeds[index]
		_angles[index] = fmod(_angles[index] + PROP_SPIN_SIGNS[index] * speed * PROP_MAX_RAD_S * delta, TAU)
		_props[index].rotation.y = _angles[index]
		var blur := clampf(speed / BLUR_FULL_SPEED, 0.0, 1.0)
		_blade_materials[index].albedo_color.a = 1.0 - blur
		_disc_materials[index].albedo_color.a = DISC_MAX_OPACITY * blur
	var phase_color := _phase_color()
	var led_color := _blinked(phase_color)
	_led_material.albedo_color = led_color
	_led_material.emission = led_color
	_trail.tint = phase_color
	if _followed:
		_ring.global_position = Vector3(_drone.global_position.x, RING_HEIGHT_M, _drone.global_position.z)
	var camera := get_viewport().get_camera_3d()
	var eye := camera.global_position if camera != null else Vector3.ZERO
	_trail.update(_drone.global_position, _drone.linear_velocity, delta, eye)


## Give a mesh its own alpha blended copy of its material, and return it.
static func _own_material(mesh: MeshInstance3D) -> StandardMaterial3D:
	var material := mesh.get_active_material(0).duplicate() as StandardMaterial3D
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mesh.material_override = material
	return material


## Color of the reported phase; dim gray before any status.
func _phase_color() -> Color:
	var link := _drone.sim_link
	if not link.status_fresh():
		return PhaseStyle.UNKNOWN_COLOR.darkened(0.5)
	return PhaseStyle.color(link.phase)


## The status light blinks while the flight process reports its RC
## fail-safe active.
func _blinked(color: Color) -> Color:
	var link := _drone.sim_link
	if link.status_fresh() and not link.rc_link_ok and fmod(_time_s * BLINK_HZ, 1.0) < 0.5:
		return color.darkened(0.75)
	return color
