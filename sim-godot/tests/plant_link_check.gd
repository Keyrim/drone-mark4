extends SceneTree

## Wire check of the plant's codecs and transport against the C++ ones,
## driven by the desktop unit tests (software/tests/unit/test_plant_link.cpp):
## opens a transport node on the discovery port given after `--`, asks the
## node it hears who it is and waits for an identity of kind DRONE_SIM,
## sends it one SimSensor envelope and expects a SimActuator echoing its
## timestamp and a SimScenario back.
## The sensor and the actuator go through the hand-written SimCodec, the
## path of the physics tick, the scenario through the generated codec, so
## the nanopb side checks both.
##
##   godot --headless --path sim-godot --script tests/plant_link_check.gd -- --discovery-port N
##
## Exit code 0 when both replies decode as expected, 1 otherwise.

const Mark4 := preload("res://scripts/gen/mark4.gd")

const REPLY_TIMEOUT_MS := 5000
const TEST_TIMESTAMP_US := 42
const EXPECTED_SEQUENCE := 3

var _flight_node: int = 0
var _got_actuator: bool = false
var _got_scenario: bool = false
var _failed: bool = false


func _init() -> void:
	var port := 0
	var args := OS.get_cmdline_user_args()
	for index in args.size():
		if args[index] == "--discovery-port" and index + 1 < args.size():
			port = int(args[index + 1])
	if port == 0:
		push_error("plant_link_check: --discovery-port is required")
		quit(1)
		return

	var transport := Mark4Transport.new()
	if not transport.open(0, port):
		quit(1)
		return
	var discovery := Mark4Discovery.new()
	discovery.setup(transport, Mark4Announce.build())
	discovery.identity.connect(_on_identity)
	transport.payload_received.connect(_on_payload)

	# The flight process is asked who it is, then spoken to first, like on
	# the sim link.
	var deadline := Time.get_ticks_msec() + REPLY_TIMEOUT_MS
	while _flight_node == 0 and Time.get_ticks_msec() < deadline:
		transport.poll(Time.get_ticks_usec())
		discovery.tick(Time.get_ticks_usec())
		OS.delay_msec(5)
	if _flight_node == 0:
		push_error("plant_link_check: no DRONE_SIM node said who it is")
		quit(1)
		return

	var payload := SimCodec.encode_sensor(
		TEST_TIMESTAMP_US,
		Vector3(0.25, -0.5, 1.5),
		Vector3(0.0, 0.0, 9.80665),
		101325.0,
		3,
		7,
		Quaternion(0.0, 0.0, 0.0, 1.0),
		Vector3(0.0, 0.0, 1.5),
		Vector3(-2.0, 0.0, 0.0)
	)
	if not transport.send(_flight_node, payload):
		push_error("plant_link_check: send failed")
		quit(1)
		return

	deadline = Time.get_ticks_msec() + REPLY_TIMEOUT_MS
	while Time.get_ticks_msec() < deadline and not (_got_actuator and _got_scenario) and not _failed:
		transport.poll(Time.get_ticks_usec())
		discovery.tick(Time.get_ticks_usec())
		OS.delay_msec(5)
	transport.close()
	if _got_actuator and _got_scenario:
		print("plant_link_check: ok")
		quit(0)
	else:
		push_error("plant_link_check: no valid reply within %d ms" % REPLY_TIMEOUT_MS)
		quit(1)


## One node said who it is: the flight process is the DRONE_SIM one.
func _on_identity(node_id: int, kind: int, _name: String) -> void:
	if kind == Mark4.NodeKind.DRONE_SIM and _flight_node == 0:
		_flight_node = node_id


func _on_payload(src: int, payload: PackedByteArray) -> void:
	if src != _flight_node:
		return
	if payload.size() > 0 and payload[0] == Mark4Announce.TAG_SIM_ACTUATOR:
		var actuator := SimCodec.decode_actuator(payload)
		_got_actuator = (
			not actuator.is_empty()
			and actuator["echo_us"] == TEST_TIMESTAMP_US
			and is_equal_approx(actuator["motor"][0], 0.1)
			and is_equal_approx(actuator["motor"][3], 0.4)
		)
		if not _got_actuator:
			push_error("plant_link_check: unexpected actuator %s" % str(actuator))
		return
	var reply := Mark4.Envelope.new()
	if reply.from_bytes(payload) != Mark4.PB_ERR.NO_ERRORS:
		push_error("plant_link_check: a reply did not decode")
		_failed = true
		return
	match reply.get_body_case():
		Mark4.Envelope.BodyCase.SIM_SCENARIO:
			var scenario: Mark4.SimScenario = reply.get_sim_scenario()
			_got_scenario = (
				scenario.get_sequence() == EXPECTED_SEQUENCE
				and scenario.get_kind() == Mark4.SimScenarioKind.THROW
				and scenario.get_velocity_mps().size() == 3
				and is_equal_approx(scenario.get_velocity_mps()[2], 6.5)
			)
			if not _got_scenario:
				push_error("plant_link_check: unexpected scenario %s" % str(reply))
		Mark4.Envelope.BodyCase.ANNOUNCE:
			pass
		_:
			push_error("plant_link_check: unexpected body %d" % reply.get_body_case())
