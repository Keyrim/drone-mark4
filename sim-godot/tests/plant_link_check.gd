extends SceneTree

## Wire check of the plant's codec and transport against the C++ ones,
## driven by the desktop unit tests (software/tests/unit/test_plant_link.cpp):
## opens a transport node on the discovery port given after `--`, waits for
## a node announcing itself as DRONE_SIM, sends it one SimSensor envelope
## and expects a SimActuator echoing its timestamp and a SimScenario back,
## decoded with the generated codec.
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
	transport.set_beacon(Mark4Announce.build())
	transport.payload_received.connect(_on_payload)

	# The flight process is found by its beacon, then spoken to first, like
	# on the sim link.
	var deadline := Time.get_ticks_msec() + REPLY_TIMEOUT_MS
	while _flight_node == 0 and Time.get_ticks_msec() < deadline:
		transport.poll(Time.get_ticks_usec())
		OS.delay_msec(5)
	if _flight_node == 0:
		push_error("plant_link_check: no DRONE_SIM node announced itself")
		quit(1)
		return

	var envelope := Mark4.Envelope.new()
	var sensor: Mark4.SimSensor = envelope.new_sim_sensor()
	sensor.set_timestamp_us(TEST_TIMESTAMP_US)
	for value in [0.25, -0.5, 1.5]:
		sensor.add_gyro_rad_s(value)
	for value in [0.0, 0.0, 9.80665]:
		sensor.add_accel_mps2(value)
	sensor.set_baro_pa(101325.0)
	sensor.set_reset_count(3)
	sensor.set_lockstep_timeouts(7)
	var truth: Mark4.PlantTruth = sensor.new_truth()
	for value in [1.0, 0.0, 0.0, 0.0]:
		truth.add_attitude_quat(value)
	for value in [0.0, 0.0, 1.5]:
		truth.add_position_m(value)
	for value in [-2.0, 0.0, 0.0]:
		truth.add_velocity_mps(value)
	if not transport.send(_flight_node, envelope.to_bytes()):
		push_error("plant_link_check: send failed")
		quit(1)
		return

	deadline = Time.get_ticks_msec() + REPLY_TIMEOUT_MS
	while Time.get_ticks_msec() < deadline and not (_got_actuator and _got_scenario) and not _failed:
		transport.poll(Time.get_ticks_usec())
		OS.delay_msec(5)
	transport.close()
	if _got_actuator and _got_scenario:
		print("plant_link_check: ok")
		quit(0)
	else:
		push_error("plant_link_check: no valid reply within %d ms" % REPLY_TIMEOUT_MS)
		quit(1)


func _on_payload(src: int, payload: PackedByteArray) -> void:
	if _flight_node == 0:
		if Mark4Announce.kind_of(payload) == Mark4.NodeKind.DRONE_SIM:
			_flight_node = src
		return
	if src != _flight_node:
		return
	var reply := Mark4.Envelope.new()
	if reply.from_bytes(payload) != Mark4.PB_ERR.NO_ERRORS:
		push_error("plant_link_check: a reply did not decode")
		_failed = true
		return
	match reply.get_body_case():
		Mark4.Envelope.BodyCase.SIM_ACTUATOR:
			var actuator: Mark4.SimActuator = reply.get_sim_actuator()
			var motor: Array = actuator.get_motor()
			_got_actuator = (
				actuator.get_echo_timestamp_us() == TEST_TIMESTAMP_US
				and motor.size() == 4
				and is_equal_approx(motor[0], 0.1)
				and is_equal_approx(motor[3], 0.4)
			)
			if not _got_actuator:
				push_error("plant_link_check: unexpected actuator %s" % str(reply))
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
