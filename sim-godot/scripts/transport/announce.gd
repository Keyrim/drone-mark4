class_name Mark4Announce
extends RefCounted

## The plant's identity, the answer it unicasts to whoever asks who it is.
##
## An encoded Envelope opens with the tag of its body (the oneof comes
## first, the request id after it), so one byte says whether a payload is
## worth decoding: the plant answers a lockstep exchange at 500 Hz and must
## not run the codec on frames it does not want.

const Mark4 := preload("res://scripts/gen/mark4.gd")

## First byte of an Envelope per body: (field number << 3) | 2, the first
## byte of the varint tag for the fields above 15.
const TAG_STATUS := 0x0A
const TAG_SIM_ACTUATOR := 0x1A
const TAG_ANNOUNCE := 0x2A
const TAG_SIM_SCENARIO := 0x82
## Two bytes here: the varint tag of the IdentityRequest (field 39) is
## 0xBA 0x02, and TuningAck (field 23) opens with the same first byte, so
## one byte does not tell them apart.
const TAG_IDENTITY_REQUEST: Array[int] = [0xBA, 0x02]

const NAME := "godot-plant"


## The Announce of this plant: kind PLANT, mcu SIM, no build identity
## (nothing is packaged), the wire hash of the generated codec. It leaves
## as the answer to an IdentityRequest, never unasked, and as a request of
## its own: the envelope rather than its bytes, because the request helper
## numbers it before it is encoded.
static func build() -> Mark4.Envelope:
	var envelope := Mark4.Envelope.new()
	var announce: Mark4.Announce = envelope.new_announce()
	announce.set_kind(Mark4.NodeKind.PLANT)
	announce.set_name(NAME)
	announce.set_mcu(Mark4.Mcu.SIM)
	announce.set_build_epoch(0)
	announce.set_git_hash("")
	announce.set_wire_hash(WireHash.VALUE)
	return envelope
