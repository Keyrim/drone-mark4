extends SceneTree

## Smoke test of the GDScript transport, no peer process: two nodes in this
## process on a private discovery port. One beacons, the other hears it
## (broadcast, header decoded, node learnt, beacon unicast back), answers
## with a unicast, and the silent node expires. Exit code 0 when every step
## holds, 1 otherwise. Run by ctest:
##
##   godot --headless --path sim-godot --script res://tests/transport_check.gd

## Away from the bench port, so a live hub never hears this.
const PORT := 47899
const STEP_TIMEOUT_MS := 2000

var _failures: int = 0


func _check(condition: bool, what: String) -> void:
	if not condition:
		_failures += 1
		push_error("transport_check: FAILED " + what)


func _init() -> void:
	var a := Mark4Transport.new()
	var b := Mark4Transport.new()
	_check(a.open(0x0A0A0A0A, PORT), "node a opens")
	_check(b.open(0x0B0B0B0B, PORT), "node b opens next to a on the same port")

	var heard := {}
	b.payload_received.connect(func(src: int, payload: PackedByteArray) -> void: heard[src] = payload)
	var ups: Array = []
	b.node_up.connect(func(id: int) -> void: ups.append(id))
	var downs: Array = []
	b.node_down.connect(func(id: int) -> void: downs.append(id))
	var a_heard := {}
	a.payload_received.connect(func(src: int, payload: PackedByteArray) -> void: a_heard[src] = payload)

	var beacon := PackedByteArray([0x2A, 0x01, 0x02, 0x03])
	a.set_beacon(beacon)
	var now_us := 1_000_000
	a.poll(now_us)  # first poll broadcasts the beacon
	var deadline := Time.get_ticks_msec() + STEP_TIMEOUT_MS
	while not heard.has(a.node_id) and Time.get_ticks_msec() < deadline:
		OS.delay_msec(5)
		b.poll(now_us)
	_check(heard.get(a.node_id, PackedByteArray()) == beacon, "b receives a's beacon payload")
	_check(ups == [a.node_id], "b saw exactly one node come up, a")
	_check(b.nodes[a.node_id]["received"] == 1, "one frame counted from a")
	_check(b.nodes[a.node_id]["port"] > 0, "a's data port learnt")

	# b learnt a from the broadcast, so a unicast reaches a's data socket
	# and a learns b from it.
	var reply := PackedByteArray([0x1A, 0xAA])
	_check(b.send(a.node_id, reply), "b unicasts to a")
	deadline = Time.get_ticks_msec() + STEP_TIMEOUT_MS
	while not a_heard.has(b.node_id) and Time.get_ticks_msec() < deadline:
		OS.delay_msec(5)
		a.poll(now_us)
	_check(a_heard.get(b.node_id, PackedByteArray()) == reply, "a receives b's unicast")
	_check(not b.send(0x0C0C0C0C, reply), "a unicast to an unknown node is refused")
	_check(b.dropped == 1, "and counted as dropped")

	# a answered b's first frame with its beacon, unicast: drain it, then
	# nothing from a for longer than the expiry and b forgets it.
	OS.delay_msec(20)
	b.poll(now_us)
	_check(b.nodes[a.node_id]["received"] == 2, "a's unicast beacon to the newcomer b was counted")
	b.poll(now_us + Mark4Transport.NODE_EXPIRY_US)
	_check(downs == [a.node_id], "a expired from b's table")
	_check(not b.is_alive(a.node_id), "a is not alive any more")

	a.close()
	b.close()
	if _failures == 0:
		print("transport_check: ok")
		quit(0)
	else:
		quit(1)
