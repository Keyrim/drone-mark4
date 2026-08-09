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

const VERSION := 10

## Packet types, the second byte of every packet (header.hpp).
const TYPE_SIM_SENSOR := 1
const TYPE_SIM_ACTUATOR := 2
const TYPE_TELEMETRY := 3
const TYPE_SIM_RAW := 4
const TYPE_SIM_COMMAND := 5
const TYPE_RC_COMMAND := 6
const TYPE_REBOOT_COMMAND := 7
const TYPE_BLACKBOX_RECORD := 8
const TYPE_ANNOUNCE := 9

## Stream source identities (header.hpp).
const SOURCE_FIRMWARE := 1
const SOURCE_DRONE_SIM := 2
const SOURCE_DRONE_REPLAY := 3
const SOURCE_SIM_PLANT := 4

## Packed packet sizes (sim_link.hpp, telemetry.hpp, sim_raw.hpp,
## commands.hpp).
const SIM_SENSOR_PACKET_SIZE := 45
const SIM_ACTUATOR_PACKET_SIZE := 26
const TELEMETRY_PACKET_SIZE := 99
const SIM_RAW_PACKET_SIZE := 53
const SIM_COMMAND_PACKET_SIZE := 50

## Scenario commands carried by SimCommandPacket (commands.hpp).
const SIM_COMMAND_RESET := 1
const SIM_COMMAND_RC := 2
const SIM_COMMAND_THROW := 3
const SIM_COMMAND_HAND_THROW := 4

## Piloting modes carried next to the RC state (commands.hpp). Reserved:
## consumed by the mode feature, defined so the wire never breaks again.
const RC_MODE_MANUAL := 0
const RC_MODE_ALTITUDE_AUTO := 1

## Default UDP ports (ports.hpp).
const SIM_LINK_PORT := 47800
const TELEMETRY_PORT := 47801
const SIM_RAW_PORT := 47802
const TELEMETRY_MIRROR_PORT := 47803
const SIM_COMMAND_PORT := 47804
const RC_COMMAND_PORT := 47805


## True when the payload opens with the protocol version and the given
## packet type.
static func has_header(payload: PackedByteArray, type: int) -> bool:
	if payload.size() < 2:
		return false
	return payload.decode_u8(0) == VERSION and payload.decode_u8(1) == type
