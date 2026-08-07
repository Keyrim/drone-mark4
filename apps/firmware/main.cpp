/// @file
/// @brief Board bring-up firmware: 168 MHz clock, LED1 heartbeat, RTT
///        console, I2C1 scan, identification of the sensor breakout chips
///        and raw IMU samples streamed over RTT. Grows into the flight
///        composition root once the IMU feeds real SensorFrames.

#include <cstdint>

#include "platform_stm32/board.hpp"
#include "platform_stm32/i2c_bus.hpp"
#include "platform_stm32/mpu6050.hpp"
#include "platform_stm32/rtt.hpp"

namespace
{
    /// Scan bounds: 7-bit addresses outside the ranges reserved by the
    /// I2C specification.
    constexpr std::uint8_t SCAN_FIRST_ADDRESS = 0x08U;
    constexpr std::uint8_t SCAN_LAST_ADDRESS = 0x77U;

    constexpr std::uint8_t BARO_ADDRESS = 0x77U;

    constexpr std::uint32_t HEARTBEAT_PERIOD_MS = 500U;

    /// Heartbeat toggles between two IMU sample logs: one line per second.
    constexpr std::uint32_t TOGGLES_PER_SAMPLE_LOG = 2U;

    /// @brief Probes every valid 7-bit address and logs the responders.
    /// @param bus initialized I2C bus
    void scanI2cBus(mark4::I2cBus &bus)
    {
        mark4::rttWrite("i2c1: scanning 0x08..0x77\n");
        std::uint32_t found = 0U;
        for (std::uint8_t address = SCAN_FIRST_ADDRESS; address <= SCAN_LAST_ADDRESS; ++address)
        {
            if (bus.probe(address))
            {
                mark4::rttPrintf("i2c1: device at 0x%02X\n", static_cast<unsigned>(address));
                ++found;
            }
        }
        mark4::rttPrintf("i2c1: scan done, %u device(s)\n", static_cast<unsigned>(found));
    }

    /// @brief Identifies the barometer at 0x77: chips of the BMP/BME
    ///        family expose an id register, an MS5611 only answers
    ///        commands, so its PROM is read as a fallback.
    /// @param bus initialized I2C bus
    void identifyBarometer(mark4::I2cBus &bus)
    {
        constexpr std::uint8_t BMP_REG_CHIP_ID = 0xD0U;
        std::uint8_t chipId = 0U;
        if (bus.readRegisters(BARO_ADDRESS, BMP_REG_CHIP_ID, &chipId, 1U))
        {
            const char *name = "unknown";
            switch (chipId)
            {
                case 0x55U:
                    name = "BMP180";
                    break;
                case 0x58U:
                    name = "BMP280";
                    break;
                case 0x60U:
                    name = "BME280";
                    break;
                case 0x61U:
                    name = "BME680";
                    break;
                default:
                    break;
            }
            mark4::rttPrintf(
                "baro: id register 0x%02X -> %s\n", static_cast<unsigned>(chipId), name);
            if (chipId != 0U)
            {
                return;
            }
        }

        // No readable id register: reset an assumed MS5611 and read its
        // first PROM calibration word, which is never 0x0000 nor 0xFFFF on
        // a live chip.
        constexpr std::uint8_t MS5611_CMD_RESET = 0x1EU;
        constexpr std::uint8_t MS5611_CMD_READ_PROM_C1 = 0xA2U;
        constexpr std::uint32_t MS5611_RESET_DELAY_MS = 3U;
        if (!bus.write(BARO_ADDRESS, &MS5611_CMD_RESET, 1U))
        {
            mark4::rttWrite("baro: not identified (reset command failed)\n");
            return;
        }
        mark4::delayMs(MS5611_RESET_DELAY_MS);
        std::uint8_t prom[2] = {0U, 0U};
        if (!bus.write(BARO_ADDRESS, &MS5611_CMD_READ_PROM_C1, 1U) ||
            !bus.read(BARO_ADDRESS, prom, sizeof(prom)))
        {
            mark4::rttWrite("baro: not identified (PROM read failed)\n");
            return;
        }
        const std::uint32_t coefficient = (static_cast<std::uint32_t>(prom[0]) << 8U) | prom[1];
        if (coefficient != 0x0000U && coefficient != 0xFFFFU)
        {
            mark4::rttPrintf("baro: MS5611, PROM C1 = %u\n", static_cast<unsigned>(coefficient));
        }
        else
        {
            mark4::rttPrintf("baro: not identified (PROM word 0x%04X)\n",
                             static_cast<unsigned>(coefficient));
        }
    }

    /// @brief Logs one raw IMU sample plus the die temperature in
    ///        hundredths of a degree (integer math, printf has no float).
    /// @param sample raw counts as read from the chip
    void logSample(const mark4::Mpu6050Sample &sample)
    {
        const std::int32_t tempCentiC =
            ((static_cast<std::int32_t>(sample.temperature) * 100) / 340) + 3653;
        mark4::rttPrintf("imu: accel %6d %6d %6d  gyro %6d %6d %6d  temp %ld.%02ld C\n",
                         sample.accel[0],
                         sample.accel[1],
                         sample.accel[2],
                         sample.gyro[0],
                         sample.gyro[1],
                         sample.gyro[2],
                         static_cast<long>(tempCentiC / 100),
                         static_cast<long>(tempCentiC % 100));
    }
} // namespace

int main()
{
    const bool clockOk = mark4::initSystemClock();
    mark4::initCycleCounter();
    mark4::rttInit();

    mark4::rttWrite("\nmark4 bring-up\n");
    mark4::rttPrintf("clock: %lu Hz (%s)\n",
                     static_cast<unsigned long>(mark4::coreClockHz()),
                     clockOk ? "HSE + PLL" : "HSI fallback");

    mark4::initLeds();

    mark4::I2cBus bus;
    mark4::Mpu6050 imu(bus);
    bool imuReady = false;
    if (!clockOk)
    {
        mark4::rttWrite("i2c1: skipped, bus timing needs the full clock tree\n");
    }
    else if (bus.init())
    {
        scanI2cBus(bus);
        identifyBarometer(bus);
        imuReady = imu.init();
        if (imuReady)
        {
            mark4::rttWrite("mpu6050: up, aux bus bypass open, rescanning\n");
            scanI2cBus(bus);
        }
    }
    else
    {
        mark4::rttWrite("i2c1: init failed, bus stuck busy\n");
    }

    std::uint32_t toggles = 0U;
    for (;;)
    {
        mark4::toggleLed1();
        mark4::delayMs(HEARTBEAT_PERIOD_MS);
        ++toggles;
        if (imuReady && (toggles % TOGGLES_PER_SAMPLE_LOG) == 0U)
        {
            mark4::Mpu6050Sample sample;
            if (imu.readSample(sample))
            {
                logSample(sample);
            }
            else
            {
                mark4::rttWrite("imu: sample read failed\n");
            }
        }
    }
}
