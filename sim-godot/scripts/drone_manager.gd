class_name DroneManager
extends Node3D

## One virtual drone per flight process: the plant's transport node and the
## lifecycle of the drones it hosts.
##
## The transport is polled once per physics tick, before the drones run:
## the beacon of a node announcing itself as DRONE_SIM creates a virtual
## drone bound to that node, every unicast payload is dispatched to the
## drone whose node sent it, and a node that expired is given a grace
## period before its drone is freed (a flight process restarts with a new
## node id, so a new drone appears next to the old one's spot). Drones
## spawn on a small grid so several can rest on the ground at once.
##
## One drone is followed: the camera, the arena fade and the overlay look
## at it. The first one created is followed by default, then the oldest
## survivor; TAB cycles through them in spawn order and a click picks the
## one under the cursor. Following changes nothing for the drone itself:
## every drone talks to its flight process whether it is looked at or not.

## A flight process was found and its drone spawned.
signal drone_added(node_id: int, drone: Drone)
## A drone left the world, its flight process gone.
signal drone_removed(node_id: int)
## The followed drone changed; null when none is left.
signal followed_changed(drone: Drone)

const DRONE_SCENE := preload("res://scenes/drone.tscn")
const Mark4 := preload("res://scripts/gen/mark4.gd")

## Delay between a node expiring and its drone leaving the world [us].
const REMOVAL_GRACE_US := 3_000_000
## Spawn grid: one drone per cell, this far apart [m].
const GRID_PITCH_M := 1.0
const GRID_COLUMNS := 4
## A click within that many pixels of a drone on screen picks it.
const PICK_RADIUS_PX := 40.0
## Wall time between two lines reporting the per-tick cost of the link [us].
const REPORT_PERIOD_US := 10_000_000

## Viewer keys: next drone, pick.
@export var input_path: NodePath
@export var camera_path: NodePath
@export var arena_path: NodePath

var transport := Mark4Transport.new()
## Flight process node id -> Drone, in spawn order.
var drones: Dictionary = {}
## Drone the camera and the overlay follow; null when none.
var followed: Drone = null

var _camera: CameraRig = null
var _arena: Arena = null
## Node id -> instant it expired [us], drones waiting to be freed.
var _leaving: Dictionary = {}
var _next_slot: int = 0
var _last_report_us: int = 0


## Say what one sensor exchange costs on this host (codec plus transport),
## summed over every drone hosted: the number a batch host is sized on.
func _print_cost() -> void:
	var ticks := 0
	var spent_us := 0
	for drone: Drone in drones.values():
		ticks += drone.sim_link.packets_sent
		spent_us += drone.sim_link.exchange_us
	if ticks > 0:
		print("drones: %d sensor frames, %.1f us per exchange" % [ticks, float(spent_us) / ticks])


func _ready() -> void:
	_camera = get_node_or_null(camera_path) as CameraRig
	_arena = get_node_or_null(arena_path) as Arena
	var input := get_node_or_null(input_path) as WorldInput
	if input != null:
		input.next_drone_requested.connect(follow_next)
		input.pick_requested.connect(pick)
	var port := SimArgs.get_port("discovery-port", Mark4Transport.DISCOVERY_PORT)
	if not transport.open(0, port):
		return
	transport.set_beacon(Mark4Announce.build())
	transport.node_down.connect(_on_node_down)
	transport.payload_received.connect(_on_payload)
	print("drones: plant node %08x, wire %08x, waiting for flight processes" % [transport.node_id, WireHash.VALUE])


func _exit_tree() -> void:
	_print_cost()
	transport.close()


func _physics_process(_delta: float) -> void:
	var now_us := Time.get_ticks_usec()
	transport.poll(now_us)
	if now_us - _last_report_us >= REPORT_PERIOD_US:
		_last_report_us = now_us
		_print_cost()
	for node_id: int in _leaving.keys():
		if now_us - _leaving[node_id] >= REMOVAL_GRACE_US:
			_remove_drone(node_id)


## Follow the drone of that flight process; unknown ids change nothing.
func follow(node_id: int) -> void:
	var drone: Drone = drones.get(node_id)
	if drone != null and drone != followed:
		_follow(drone)


## Follow the next drone in spawn order, wrapping around.
func follow_next() -> void:
	if drones.size() < 2:
		return
	var ids: Array = drones.keys()
	var index := ids.find(followed.sim_link.flight_node) if followed != null else -1
	follow(ids[(index + 1) % ids.size()])


## Follow the drone drawn nearest to that screen position, if one is close.
func pick(screen_position: Vector2) -> void:
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		return
	var best: Drone = null
	var best_px := PICK_RADIUS_PX
	for drone: Drone in drones.values():
		var position := drone.global_position
		if camera.is_position_behind(position):
			continue
		var distance_px := camera.unproject_position(position).distance_to(screen_position)
		if distance_px < best_px:
			best_px = distance_px
			best = drone
	if best != null and best != followed:
		_follow(best)


func _on_payload(src: int, payload: PackedByteArray) -> void:
	var drone: Drone = drones.get(src)
	if drone != null:
		if _leaving.erase(src):
			print("drones: node %08x is back, keeping its drone" % src)
		drone.sim_link.receive(payload)
		return
	if Mark4Announce.kind_of(payload) == Mark4.NodeKind.DRONE_SIM:
		_add_drone(src)


func _on_node_down(node_id: int) -> void:
	if drones.has(node_id) and not _leaving.has(node_id):
		print("drones: node %08x expired, freeing its drone in %d s" % [node_id, REMOVAL_GRACE_US / 1_000_000])
		_leaving[node_id] = Time.get_ticks_usec()


func _add_drone(node_id: int) -> void:
	var drone: Drone = DRONE_SCENE.instantiate()
	var column := _next_slot % GRID_COLUMNS
	var row := _next_slot / GRID_COLUMNS
	_next_slot += 1
	drone.start_position = Vector3(
		(column - (GRID_COLUMNS - 1) * 0.5) * GRID_PITCH_M, drone.start_position.y, row * GRID_PITCH_M
	)
	drone.position = drone.start_position
	drone.name = "Drone_%08x" % node_id
	add_child(drone)
	drone.sim_link.setup(transport, node_id)
	drones[node_id] = drone
	print("drones: flight process %08x found, virtual drone %d spawned at %s" % [node_id, drones.size(), drone.start_position])
	drone_added.emit(node_id, drone)
	if followed == null:
		_follow(drone)


func _remove_drone(node_id: int) -> void:
	_leaving.erase(node_id)
	var drone: Drone = drones.get(node_id)
	if drone == null:
		return
	drones.erase(node_id)
	print("drones: flight process %08x gone, virtual drone removed (%d left)" % [node_id, drones.size()])
	if _camera != null:
		_camera.forget(drone)
	if followed == drone:
		followed = null
		for other: Drone in drones.values():
			_follow(other)
			break
		if followed == null:
			_follow(null)
	drone.queue_free()
	drone_removed.emit(node_id)


func _follow(drone: Drone) -> void:
	if followed != null and is_instance_valid(followed):
		followed.view.set_followed(false)
	followed = drone
	if drone != null:
		drone.view.set_followed(true)
	if _camera != null:
		_camera.set_target(drone)
	if _arena != null:
		_arena.set_target(drone)
	followed_changed.emit(drone)
