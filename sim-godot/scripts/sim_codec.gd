class_name SimCodec
extends RefCounted

## Hand-written protobuf codec of the two envelopes exchanged once per
## physics tick: SimSensor out, SimActuator in.
##
## The generated codec (scripts/gen/mark4.gd, godobuf) builds an object
## tree per message and costs about 200 us to encode a SimSensor and 130 us
## to decode a SimActuator; at 500 Hz per drone that is a third of a
## desktop core, and it decides the frame rate of the whole simulator. The
## two messages are small, fixed and stable, so they are written and read
## straight into bytes here, in a few microseconds. Everything else the
## plant sends or reads (Announce, Status, SimScenario) is rare and keeps
## the generated codec.
##
## The wire is software/components/protocol/mark4.proto; the field numbers
## below are its. Repeated floats travel packed, as protoc and godobuf
## emit them, and nanopb reads both forms. The output is byte for byte what
## godobuf produces for the same values, up to the fields godobuf skips
## when they hold their default.

## Envelope tags: (field number << 3) | wire type 2.
const TAG_ENVELOPE_SIM_SENSOR := 0x12
const TAG_ENVELOPE_SIM_ACTUATOR := 0x1A

## SimSensor fields.
const TAG_SENSOR_TIMESTAMP := 0x08
const TAG_SENSOR_GYRO := 0x12
const TAG_SENSOR_ACCEL := 0x1A
const TAG_SENSOR_BARO := 0x25
const TAG_SENSOR_RESET_COUNT := 0x28
const TAG_SENSOR_LOCKSTEP_TIMEOUTS := 0x30
const TAG_SENSOR_TRUTH := 0x3A

## PlantTruth fields.
const TAG_TRUTH_ATTITUDE := 0x0A
const TAG_TRUTH_POSITION := 0x12
const TAG_TRUTH_VELOCITY := 0x1A

## SimActuator fields.
const FIELD_ACTUATOR_ECHO := 1
const FIELD_ACTUATOR_MOTOR := 2

const MOTOR_COUNT := 4

## Protobuf wire types.
const WIRE_VARINT := 0
const WIRE_FIXED64 := 1
const WIRE_LENGTH := 2
const WIRE_FIXED32 := 5


## One SimSensor envelope. Vectors and the quaternion are already in the
## drone frame convention of the wire; the quaternion is written w first.
static func encode_sensor(
	timestamp_us: int,
	gyro_rad_s: Vector3,
	accel_mps2: Vector3,
	baro_pa: float,
	reset_count: int,
	lockstep_timeouts: int,
	attitude: Quaternion,
	position_m: Vector3,
	velocity_mps: Vector3
) -> PackedByteArray:
	var truth := PackedByteArray()
	_put_floats(truth, TAG_TRUTH_ATTITUDE, [attitude.w, attitude.x, attitude.y, attitude.z])
	_put_floats(truth, TAG_TRUTH_POSITION, [position_m.x, position_m.y, position_m.z])
	_put_floats(truth, TAG_TRUTH_VELOCITY, [velocity_mps.x, velocity_mps.y, velocity_mps.z])

	var sensor := PackedByteArray()
	sensor.append(TAG_SENSOR_TIMESTAMP)
	_put_varint(sensor, timestamp_us)
	_put_floats(sensor, TAG_SENSOR_GYRO, [gyro_rad_s.x, gyro_rad_s.y, gyro_rad_s.z])
	_put_floats(sensor, TAG_SENSOR_ACCEL, [accel_mps2.x, accel_mps2.y, accel_mps2.z])
	sensor.append(TAG_SENSOR_BARO)
	_put_float(sensor, baro_pa)
	sensor.append(TAG_SENSOR_RESET_COUNT)
	_put_varint(sensor, reset_count)
	sensor.append(TAG_SENSOR_LOCKSTEP_TIMEOUTS)
	_put_varint(sensor, lockstep_timeouts)
	sensor.append(TAG_SENSOR_TRUTH)
	_put_varint(sensor, truth.size())
	sensor.append_array(truth)

	var envelope := PackedByteArray()
	envelope.append(TAG_ENVELOPE_SIM_SENSOR)
	_put_varint(envelope, sensor.size())
	envelope.append_array(sensor)
	return envelope


## Read a SimActuator envelope.
## @return {"echo_us": int, "motor": PackedFloat32Array of 4}, or an empty
## dictionary when the payload is not a well formed SimActuator with four
## motors. Fields this reader does not know are skipped.
static func decode_actuator(payload: PackedByteArray) -> Dictionary:
	if payload.size() < 2 or payload[0] != TAG_ENVELOPE_SIM_ACTUATOR:
		return {}
	var cursor := [1]
	var length := _get_varint(payload, cursor)
	var end: int = cursor[0] + length
	if length < 0 or end > payload.size():
		return {}
	var echo_us := 0
	var motor := PackedFloat32Array()
	while cursor[0] < end:
		var tag := _get_varint(payload, cursor)
		if tag < 0:
			return {}
		var field := tag >> 3
		var wire := tag & 7
		if field == FIELD_ACTUATOR_ECHO and wire == WIRE_VARINT:
			echo_us = _get_varint(payload, cursor)
		elif field == FIELD_ACTUATOR_MOTOR and wire == WIRE_LENGTH:
			var packed := _get_varint(payload, cursor)
			if packed < 0 or packed % 4 != 0 or cursor[0] + packed > end:
				return {}
			for _index: int in packed / 4:
				motor.append(payload.decode_float(cursor[0]))
				cursor[0] += 4
		elif field == FIELD_ACTUATOR_MOTOR and wire == WIRE_FIXED32:
			if cursor[0] + 4 > end:
				return {}
			motor.append(payload.decode_float(cursor[0]))
			cursor[0] += 4
		elif not _skip(payload, cursor, wire, end):
			return {}
		if cursor[0] < 0:
			return {}
	if motor.size() != MOTOR_COUNT:
		return {}
	return {"echo_us": echo_us, "motor": motor}


static func _put_varint(buffer: PackedByteArray, value: int) -> void:
	while value >= 0x80:
		buffer.append((value & 0x7F) | 0x80)
		value >>= 7
	buffer.append(value)


static func _put_float(buffer: PackedByteArray, value: float) -> void:
	var at := buffer.size()
	buffer.resize(at + 4)
	buffer.encode_float(at, value)


## A packed repeated float field: tag, byte length, the values.
static func _put_floats(buffer: PackedByteArray, tag: int, values: Array) -> void:
	buffer.append(tag)
	buffer.append(4 * values.size())
	for value: float in values:
		_put_float(buffer, value)


## Read a varint at cursor[0] and advance it; -1 and a negative cursor when
## the buffer ends first (the callers check either).
static func _get_varint(buffer: PackedByteArray, cursor: Array) -> int:
	var value := 0
	var shift := 0
	while cursor[0] < buffer.size() and shift < 64:
		var byte := buffer[cursor[0]]
		cursor[0] += 1
		value |= (byte & 0x7F) << shift
		if byte < 0x80:
			return value
		shift += 7
	cursor[0] = -1
	return -1


## Skip one field of the given wire type.
static func _skip(buffer: PackedByteArray, cursor: Array, wire: int, end: int) -> bool:
	match wire:
		WIRE_VARINT:
			return _get_varint(buffer, cursor) >= 0
		WIRE_FIXED64:
			cursor[0] += 8
		WIRE_LENGTH:
			var length := _get_varint(buffer, cursor)
			if length < 0:
				return false
			cursor[0] += length
		WIRE_FIXED32:
			cursor[0] += 4
		_:
			return false
	return cursor[0] <= end
