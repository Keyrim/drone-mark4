class_name DroneSensors
extends Node

## Sensor models for the simulated drone: accelerometer, rate gyro, barometer.
##
## The accelerometer reports SPECIFIC FORCE in the body frame, which is what a
## real MEMS accelerometer measures: the sum of the non gravitational forces
## divided by the mass. It is computed from the velocity delta produced by the
## physics step, so contact forces are included for free:
##
##   f_world = (v - v_previous) / dt - g_world
##   f_body  = basis_inverse * f_world
##
## which reads +1 g upward at rest on the ground, exactly 0 in free fall (the
## body then only follows gravity, so the velocity delta is g * dt) and a few g
## while a hand accelerates the drone.
##
## Every channel goes through the same pipeline: ideal value, constant bias,
## gaussian noise, clipping to the full scale range, quantization to the LSB of
## a 16 bit converter. Noise and bias amplitudes are exported; the generator is
## seeded explicitly so a run is reproducible.

const GRAVITY_MPS2 := 9.80665
const GRAVITY_WORLD := Vector3(0.0, -GRAVITY_MPS2, 0.0)
const DEG_TO_RAD := PI / 180.0

## Signed full scale of a 16 bit converter, one LSB is range / COUNTS_PER_RANGE.
const COUNTS_PER_RANGE := 32768.0

## Accelerometer full scale, +/- 16 g like a typical flight controller IMU.
const ACCEL_RANGE_MPS2 := 16.0 * GRAVITY_MPS2
const ACCEL_LSB_MPS2 := ACCEL_RANGE_MPS2 / COUNTS_PER_RANGE

## Gyro full scale, +/- 4000 deg/s: a tumbling throw saturates a 2000 deg/s part.
const GYRO_RANGE_RAD_S := 4000.0 * DEG_TO_RAD
const GYRO_LSB_RAD_S := GYRO_RANGE_RAD_S / COUNTS_PER_RANGE

## Standard atmosphere, pressure as a function of geometric altitude.
const SEA_LEVEL_PRESSURE_PA := 101325.0
const ATMOSPHERE_LAPSE_PER_M := 2.25577e-5
const ATMOSPHERE_EXPONENT := 5.25588

## Altitude clamp keeping the standard atmosphere formula in its valid domain.
const MIN_ALTITUDE_M := -1000.0
const MAX_ALTITUDE_M := 40000.0

@export_group("Noise")
## Standard deviation of the accelerometer white noise, per axis.
@export var accel_noise_sigma_mps2: float = 0.05
## Standard deviation of the gyro white noise, per axis.
@export var gyro_noise_sigma_rad_s: float = 0.002
## Standard deviation of the barometer white noise.
@export var baro_noise_sigma_pa: float = 3.0

@export_group("Bias")
## Spread of the constant accelerometer bias drawn once at startup, per axis.
@export var accel_bias_sigma_mps2: float = 0.02
## Spread of the constant gyro bias drawn once at startup, per axis.
@export var gyro_bias_sigma_rad_s: float = 0.002

@export_group("Determinism")
## Seed of the generator. The same seed replays the same noise and bias.
@export var rng_seed: int = 20250804

## Last sampled body angular rates [rad/s].
var gyro_rad_s: Vector3 = Vector3.ZERO
## Last sampled specific force in the body frame [m/s^2].
var accel_mps2: Vector3 = Vector3.ZERO
## Last sampled static pressure [Pa].
var baro_pa: float = SEA_LEVEL_PRESSURE_PA

var _rng := RandomNumberGenerator.new()
var _accel_bias_mps2: Vector3 = Vector3.ZERO
var _gyro_bias_rad_s: Vector3 = Vector3.ZERO


func _ready() -> void:
	reseed(rng_seed)


## Reseed the generator and redraw the constant biases.
func reseed(seed_value: int) -> void:
	rng_seed = seed_value
	_rng.seed = seed_value
	_accel_bias_mps2 = _gaussian_vector(accel_bias_sigma_mps2)
	_gyro_bias_rad_s = _gaussian_vector(gyro_bias_sigma_rad_s)


## Sample the three sensors for the physics step that just completed.
##
## @param body_basis orientation of the drone, world from body.
## @param world_velocity linear velocity after the step [m/s].
## @param previous_world_velocity linear velocity before the step [m/s].
## @param world_angular_velocity angular velocity in the world frame [rad/s].
## @param altitude_m geometric altitude of the drone [m].
## @param dt duration of the physics step [s].
func sample(
	body_basis: Basis,
	world_velocity: Vector3,
	previous_world_velocity: Vector3,
	world_angular_velocity: Vector3,
	altitude_m: float,
	dt: float
) -> void:
	var body_from_world := body_basis.inverse()

	var specific_force_world := Vector3.ZERO
	if dt > 0.0:
		specific_force_world = (world_velocity - previous_world_velocity) / dt - GRAVITY_WORLD
	accel_mps2 = _condition(
		body_from_world * specific_force_world,
		_accel_bias_mps2,
		accel_noise_sigma_mps2,
		ACCEL_RANGE_MPS2,
		ACCEL_LSB_MPS2
	)

	gyro_rad_s = _condition(
		body_from_world * world_angular_velocity,
		_gyro_bias_rad_s,
		gyro_noise_sigma_rad_s,
		GYRO_RANGE_RAD_S,
		GYRO_LSB_RAD_S
	)

	baro_pa = pressure_from_altitude(altitude_m) + _rng.randfn(0.0, baro_noise_sigma_pa)


## Static pressure of the standard atmosphere at the given geometric altitude.
func pressure_from_altitude(altitude_m: float) -> float:
	var clamped_m := clampf(altitude_m, MIN_ALTITUDE_M, MAX_ALTITUDE_M)
	return (
		SEA_LEVEL_PRESSURE_PA * pow(1.0 - ATMOSPHERE_LAPSE_PER_M * clamped_m, ATMOSPHERE_EXPONENT)
	)


## Magnitude of the last accelerometer sample, expressed in g.
func accel_magnitude_g() -> float:
	return accel_mps2.length() / GRAVITY_MPS2


func _condition(
	ideal: Vector3, bias: Vector3, noise_sigma: float, range_value: float, lsb: float
) -> Vector3:
	var measured := ideal + bias + _gaussian_vector(noise_sigma)
	return Vector3(
		_quantize(clampf(measured.x, -range_value, range_value), lsb),
		_quantize(clampf(measured.y, -range_value, range_value), lsb),
		_quantize(clampf(measured.z, -range_value, range_value), lsb)
	)


func _quantize(value: float, lsb: float) -> float:
	return roundf(value / lsb) * lsb


func _gaussian_vector(sigma: float) -> Vector3:
	return Vector3(_rng.randfn(0.0, sigma), _rng.randfn(0.0, sigma), _rng.randfn(0.0, sigma))
