class_name Protocol

## Single GDScript source of the wire protocol constants.
##
## Mirrors protocol/include/protocol/ (version.hpp, header.hpp, ports.hpp
## and the packet headers), which are the source of truth. Every script
## that touches the wire reads the constants here and nowhere else; the
## golden packet fixtures in CI catch any drift against the C++ layout.
##
## All packets are packed, little endian, and open with a version byte
## then a type byte: nothing is ever demultiplexed by size alone. Stream
## packets (telemetry, sim raw) follow with a source id byte and a u16
## sequence number.

const VERSION := 15

## Packet types, the second byte of every packet (header.hpp).
const TYPE_SIM_SENSOR := 1
const TYPE_SIM_ACTUATOR := 2
const TYPE_TELEMETRY := 3
const TYPE_SIM_RAW := 4
const TYPE_SIM_SCENARIO := 5
const TYPE_RC_COMMAND := 6
const TYPE_REBOOT_COMMAND := 7
const TYPE_BLACKBOX_RECORD := 8
const TYPE_ANNOUNCE := 9
const TYPE_TUNING_SET := 10
const TYPE_TUNING_GET := 11
const TYPE_TUNING_LIST := 12
const TYPE_TUNING_ACK := 13
const TYPE_TUNING_INFO := 14
const TYPE_SIM_RUN_STATS := 15

## Stream source identities (header.hpp).
const SOURCE_FIRMWARE := 1
const SOURCE_DRONE_SIM := 2
const SOURCE_DRONE_REPLAY := 3
const SOURCE_SIM_PLANT := 4

## Packed packet sizes (sim_link.hpp, sim_raw.hpp, commands.hpp,
## sim_stats.hpp).
const SIM_SENSOR_PACKET_SIZE := 45
const SIM_ACTUATOR_PACKET_SIZE := 84
const SIM_RAW_PACKET_SIZE := 53
const SIM_SCENARIO_PACKET_SIZE := 60
const SIM_SCENARIO_SIZE := 58
const SIM_RUN_STATS_PACKET_SIZE := 31
const RC_COMMAND_PACKET_SIZE := 9

## Scenarios carried by the SimScenario block (commands.hpp). Each one
## opens with a reset; everything after that reset tick is scheduled by
## the plant on its own tick grid.
const SIM_SCENARIO_RESET := 1
## Retired since v11: RC travels as RcCommandPacket to the flight process
## command receiver. The value stays reserved so the neighbors keep theirs.
const SIM_SCENARIO_RC := 2
const SIM_SCENARIO_THROW := 3
const SIM_SCENARIO_HAND_THROW := 4

## Piloting modes carried next to the RC state (commands.hpp). Reserved:
## consumed by the mode feature, defined so the wire never breaks again.
const RC_MODE_MANUAL := 0
const RC_MODE_ALTITUDE_AUTO := 1

## Field offsets consumed by the decoding scripts, frozen by the C++
## static_asserts and the golden fixtures.
const FLOAT_SIZE := 4
const SIM_ACTUATOR_ECHO_OFFSET := 2
const SIM_ACTUATOR_MOTOR_OFFSET := 10
## Where the scenario block starts inside the lockstep reply, the only
## path this project decodes a scenario on.
const SIM_ACTUATOR_SCENARIO_OFFSET := 26

## Offsets inside the SimScenario block, counted from its own start.
const SCENARIO_SEED_OFFSET := 2
const SCENARIO_THROW_DELAY_OFFSET := 10
const SCENARIO_VELOCITY_OFFSET := 18
const SCENARIO_ANGULAR_OFFSET := 30
const SCENARIO_HELD_OFFSET := 42

## Default UDP ports (ports.hpp).
const SIM_LINK_PORT := 47800
const TELEMETRY_PORT := 47801
const SIM_RAW_PORT := 47802
## 47803 is unassigned: it used to mirror the telemetry broadcast for the
## one consumer whose socket stack could not share a bound port.
## 47804 is unassigned: this project binds no command port any more, and
## scenarios reach it inside the lockstep reply.
const RC_COMMAND_PORT := 47805
const ANNOUNCE_PORT := 47806


## True when the payload opens with the protocol version and the given
## packet type.
static func has_header(payload: PackedByteArray, type: int) -> bool:
	if payload.size() < 2:
		return false
	return payload.decode_u8(0) == VERSION and payload.decode_u8(1) == type
