class_name Mark4Announce
extends RefCounted

## The plant's beacon and the cheap reading of everyone else's.
##
## An encoded Envelope opens with the tag of its body (the Envelope has one
## field, its oneof), so one byte says whether a payload is worth decoding:
## the plant hears every broadcast of the LAN, telemetry at 500 Hz included,
## and must not run the codec on frames it does not want.

const Mark4 := preload("res://scripts/gen/mark4.gd")

## First byte of an Envelope per body: (field number << 3) | 2, the first
## byte of the varint tag for the fields above 15.
const TAG_STATUS := 0x0A
const TAG_SIM_ACTUATOR := 0x1A
const TAG_ANNOUNCE := 0x2A
const TAG_SIM_SCENARIO := 0x82

const NAME := "godot-plant"


## The Announce of this plant, encoded: kind PLANT, mcu SIM, no build
## identity (nothing is packaged), the wire hash of the generated codec.
static func build() -> PackedByteArray:
	var envelope := Mark4.Envelope.new()
	var announce: Mark4.Announce = envelope.new_announce()
	announce.set_kind(Mark4.NodeKind.PLANT)
	announce.set_name(NAME)
	announce.set_mcu(Mark4.Mcu.SIM)
	announce.set_build_epoch(0)
	announce.set_git_hash("")
	announce.set_wire_hash(WireHash.VALUE)
	return envelope.to_bytes()


## Node kind carried by a payload, or -1 when it is not an Announce.
static func kind_of(payload: PackedByteArray) -> int:
	if payload.is_empty() or payload[0] != TAG_ANNOUNCE:
		return -1
	var envelope := Mark4.Envelope.new()
	if envelope.from_bytes(payload) != Mark4.PB_ERR.NO_ERRORS:
		return -1
	if envelope.get_body_case() != Mark4.Envelope.BodyCase.ANNOUNCE:
		return -1
	return envelope.get_announce().get_kind()
