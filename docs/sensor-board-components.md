# Sensor board - component selection

Comparison and rationale for the custom sensor board (EasyEDA project
`mark1-sensorboard`) that replaces the GY-86 breakout on the mark1 flight
controller (see `bring-up.md`). The board plugs 1:1 into the `sensor_board`
JST-SH 8-pin connector: 3V3, MPU_CS, SPI3 (SCK/MISO/MOSI), I2C1 (SCL/SDA),
GND. Constraints inherited from that connector: one SPI chip select only,
I2C pull-ups on the FC side, no interrupt line (the loop stays timer-paced).

Prices and stock are LCSC, single-unit tier, snapshot of 2026-08-07.
"PCBA type" is the `jlcpcb.com/partdetail` field: it decides whether JLCPCB
Economic assembly accepts the part. Every MEMS sensor checked is
"Standard Only" except the ICM-42688-P; Standard assembly forces a
panel > 70 mm with edge rails, a higher setup fee and X-ray inspection
for bottom-terminated packages (LGA).

## IMU (SPI, gyro range with margin over ~600 deg/s measured throws)

| Part | LCSC | Price | Stock | Gyro range | PCBA type | Verdict |
|---|---|---|---|---|---|---|
| **LSM6DSRTR (chosen)** | C784817 | $3.20 | 5,533 | +/-4000 dps | Standard only | Best range, price and stock; ST mainstream |
| LSM6DSV16XTR | C5267406 | $4.06 | 2,852 | +/-4000 dps | Standard only | Credible backup, newer ST generation |
| ICM-42688-P | C1850418 | $17.00 | 3,584 | +/-2000 dps | Economic and Standard | Best noise specs, FC standard, but shortage pricing (Mouser/DigiKey/Farnell at 0 stock, 45-week lead time); only Economic-eligible MEMS found, useless at this price |
| ICM-42605 | C2655099 | $8.96 | 5,661 | +/-2000 dps | Standard only | Overpriced for the spec |
| BMI270 | C2836813 | $3.65 | 680 | +/-2000 dps | Standard only | Fine part, stock too thin |
| LSM6DS3TR-C | C967633 | $1.56 | 160 | +/-2000 dps | Standard only | Cheapest, stock too thin, older generation |

Note: plan-dev quotes the ICM-42688-P at +/-4000 dps; that figure actually
belongs to the ICM-42686-P (not stocked). The 42688 tops out at +/-2000 dps.
Measured hand throws tumble at <= 10 rad/s (~570 deg/s), so +/-2000 dps was
already sufficient; the LSM6DSR gives 7x margin.

## Barometer (I2C, altitude hold needs sub-metre relative precision)

| Part | LCSC | Price | Stock | Relative accuracy | PCBA type | Verdict |
|---|---|---|---|---|---|---|
| **SPA06-003 (chosen)** | C30589048 | $0.74 | 24,770 | +/-0.03 hPa (~25 cm) | Standard only | Active Goertek successor of the SPL06, huge stock, cheapest |
| BMP388 | C779278 | $2.65 | 18,772 | +/-0.08 hPa | Standard only | Solid Bosch fallback, well supported |
| BMP581 | C5362283 | $3.06 | 4,265 | +/-0.06 hPa (best noise floor) | Standard only | JLC flags assembly as "difficult, may require Advanced"; dropped |
| SPL06-001 | C2684428 | ~$1 | EOL | +/-0.06 hPa | - | Discontinued at LCSC (like DPS310, BMP280, BMP390L) |
| MS5611-01BA03 | - | $6-10 | - | +/-0.025 hPa | - | Old, expensive, counterfeit-prone (the GY-86 cell that failed) |

## Compass (I2C, optional heading; not needed for throw/apex/hover)

| Part | LCSC | Price | Stock | Notes | PCBA type | Verdict |
|---|---|---|---|---|---|---|
| **QMC5883P (chosen)** | C2847467 | $1.56 | 21,020 | 16-bit, addr 0x2C, single 4.7 uF reservoir cap, no SET/RST cap (unlike the 5883L) | Standard only | Cheap, stocked, direct on I2C1 (no MPU bypass dance like the GY-86) |
| IST8310 | C2683055 | $3.77 | 2,718 | 14-bit, Pixhawk-style boards | Standard only | Works, costs more for less resolution |
| LIS2MDLTR | C919695 | $3.64 | 1 | ST mainstream | Standard only | No stock |
| QMC5883L | C976032 | - | EOL | addr 0x0D, needs SET/RST cap | - | Discontinued |
| HMC5883L | - | - | - | the GY-86 part | - | Honeywell exited the business years ago |

## Chosen set, wiring summary

- U4 LSM6DSRTR: SPI3, CS = MPU_CS. SDx/SCx to GND; OCS_Aux and
  SDO_Aux LEFT UNCONNECTED (datasheet mode 1 - both have internal
  pull-ups, grounding them is out of spec), INT1/INT2 not connected.
  VDD 100 nF (datasheet) + 2.2 uF bulk (designer addition), VDDIO
  100 nF.
- U5 SPA06-003: I2C1 addr 0x76 (SDO to GND), CSB to 3V3, VDD/VDDIO 100 nF
  each. Keep the pressure port clear of silkscreen, flux and the connector.
- U3 QMC5883P: I2C1 addr 0x2C, 4.7 uF reservoir on pin C1, VDD 100 nF.
  No copper pour or tracks under the package on any layer; keep the LED
  current loop away.
- P1 SM08B-SRSS-TB (C160407, genuine JST, Extended), passives 0603 Basic,
  power LED + 1k.

No I2C address conflict (0x76 baro, 0x2C compass); the whole set plus
FC-side 4.7k pull-ups shares I2C1 with the optional GPS connector.

## Layout guidelines

Placement priority: compass first (most constrained), then IMU, then baro.

- QMC5883P: board corner, as far as possible from the LED loop, the 3V3
  feed and the FC below (motor/power currents a few mm away once stacked);
  pick the corner opposite the FC power_board connector. Copper keepout
  under the package on all layers plus 2-3 mm guard (QST rule). Nylon
  standoffs and screws on that corner, no steel.
- LSM6DSR: board center, in the stiff area, away from edges and from the
  connector (cable strain path). Axes aligned with board edges, axis arrow
  on silkscreen. Continuous GND plane underneath. 100 nF tight against
  VDD (pin 8) and VDDIO (pin 5) with a GND via at the cap; 2.2 uF bulk a
  few mm behind.
- SPA06-003: pressure port facing up (top side, board sits above the FC),
  no silkscreen or coating on the port, not directly above the FC LDO
  (thermal gradient = drift), away from prop-wash board edges. Reserve
  room for an open-cell foam patch over it (wind and light shielding).
- Routing: near-full bottom GND plane (except the compass keepout), top
  pour stitched with vias. SPI is only 10 MHz: keep it short over the
  plane. I2C is relaxed (pull-ups on the FC). 3V3 as a 0.4-0.5 mm trunk
  from connector pin 1. LED and its resistor near the connector, tight
  loop. Mounting holes matched to the FC stack.
- IMU/baro mutual distance is irrelevant; only the compass needs distance,
  and only from current-carrying or ferrous things.

Mechanical format: standard 30.5 x 30.5 mm stack pattern (M3), mounted
with the same screws as the FC and ESC, at the TOP of the stack (farthest
from ESC battery currents). A smaller board on a 3D-printed adapter was
rejected: a printed bracket puts the IMU on a compliant arm whose
resonances sit in the prop frequency range (100-500 Hz), creeps and
loosens over crashes. Use the silicone stack grommets as IMU vibration
isolation. Steel stack screws give a constant field the compass
calibration absorbs, but place the compass mid-edge (~15 mm from the two
nearest screws) rather than in a corner (~3 mm from one), on the edge
opposite the battery/power wiring. Connector edge facing the FC
sensor_board connector for a short cable; IMU axes aligned with the frame
axes and marked on silkscreen. Spare surface: add test points for
SCK/MISO/MOSI/CS/SCL/SDA/3V3/GND.

ESP32 telemetry bridge: kept OFF this board. The sensor_board connector
has no UART so a second cable would be needed anyway; WiFi TX bursts
(350-500 mA) would load the FTDI-fed 5V rail and the FC LDO through the
sensor cable; antenna keepout plus compass keepout do not fit a 2x3 cm
board. A separate JST-SH 4-pin module on `uart1` (GND/+5/RX/TX, own LDO)
is the better shape.

## Quote history (JLCPCB, 5 boards, 2-layer, one side assembled)

| Configuration | Quote |
|---|---|
| Standard PCBA, ICM-42688-P + BMP581 + QMC5883P | 153 EUR |
| Standard PCBA, 2 boards only, same BOM | 93 EUR |
| Economic PCBA, all three sensors DNP | 16 EUR |
| Standard PCBA, LSM6DSR + SPA06 + QMC5883P | 82 EUR |

Fixed Standard costs are ~44 EUR (setup 22 + stencil 7 + feeders ~13) plus
~11 EUR X-ray; the marginal cost of boards 3 to 5 is essentially the
components. Watch the pre-selected express shipping line (~43 EUR) when
ordering; Global Standard Direct is a fraction of that.
