extends SceneTree

## Wire check of the plant's codec against the C++ one, driven by the desktop
## unit tests (software/tests/unit/test_plant_link.cpp): sends one SimSensor
## envelope to the port given after `--`, expects a SimActuator echoing its
## timestamp and a SimScenario back, decoded with the generated codec.
##
##   godot --headless --path sim-godot --script tests/plant_link_check.gd -- --port N
##
## Exit code 0 when both replies decode as expected, 1 otherwise.

const Mark4 := preload("res://scripts/gen/mark4.gd")

const REPLY_TIMEOUT_MS := 5000
const TEST_TIMESTAMP_US := 42
const EXPECTED_SEQUENCE := 3


func _init() -> void:
	var port := 0
	var args := OS.get_cmdline_user_args()
	for index in args.size():
		if args[index] == "--port" and index + 1 < args.size():
			port = int(args[index + 1])
	if port == 0:
		push_error("plant_link_check: --port is required")
		quit(1)
		return

	var socket := PacketPeerUDP.new()
	if socket.bind(0, "127.0.0.1") != OK or socket.set_dest_address("127.0.0.1", port) != OK:
		push_error("plant_link_check: cannot open the socket")
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
	if socket.put_packet(envelope.to_bytes()) != OK:
		push_error("plant_link_check: send failed")
		quit(1)
		return

	var got_actuator := false
	var got_scenario := false
	var deadline := Time.get_ticks_msec() + REPLY_TIMEOUT_MS
	while Time.get_ticks_msec() < deadline and not (got_actuator and got_scenario):
		if socket.get_available_packet_count() == 0:
			OS.delay_msec(5)
			continue
		var reply := Mark4.Envelope.new()
		if reply.from_bytes(socket.get_packet()) != Mark4.PB_ERR.NO_ERRORS:
			push_error("plant_link_check: a reply did not decode")
			quit(1)
			return
		match reply.get_body_case():
			Mark4.Envelope.BodyCase.SIM_ACTUATOR:
				var actuator: Mark4.SimActuator = reply.get_sim_actuator()
				var motor: Array = actuator.get_motor()
				got_actuator = (
					actuator.get_echo_timestamp_us() == TEST_TIMESTAMP_US
					and motor.size() == 4
					and is_equal_approx(motor[0], 0.1)
					and is_equal_approx(motor[3], 0.4)
				)
				if not got_actuator:
					push_error("plant_link_check: unexpected actuator %s" % str(reply))
			Mark4.Envelope.BodyCase.SIM_SCENARIO:
				var scenario: Mark4.SimScenario = reply.get_sim_scenario()
				got_scenario = (
					scenario.get_sequence() == EXPECTED_SEQUENCE
					and scenario.get_kind() == Mark4.SimScenarioKind.THROW
					and scenario.get_velocity_mps().size() == 3
					and is_equal_approx(scenario.get_velocity_mps()[2], 6.5)
				)
				if not got_scenario:
					push_error("plant_link_check: unexpected scenario %s" % str(reply))
			_:
				push_error("plant_link_check: unexpected body %d" % reply.get_body_case())
	socket.close()
	if got_actuator and got_scenario:
		print("plant_link_check: ok")
		quit(0)
	else:
		push_error("plant_link_check: no valid reply within %d ms" % REPLY_TIMEOUT_MS)
		quit(1)
