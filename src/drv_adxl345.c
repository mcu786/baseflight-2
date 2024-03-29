#include "board.h"

// ADXL345, Alternative address mode 0x53
#define ADXL345_ADDRESS     0x53

// Registers
#define ADXL345_BW_RATE     0x2C
#define ADXL345_POWER_CTL   0x2D
#define ADXL345_INT_ENABLE  0x2E
#define ADXL345_DATA_FORMAT 0x31
#define ADXL345_DATA_OUT    0x32
#define ADXL345_FIFO_CTL    0x38

// BW_RATE values
#define ADXL345_RATE_50     0x09
#define ADXL345_RATE_100    0x0A
#define ADXL345_RATE_200    0x0B
#define ADXL345_RATE_400    0x0C
#define ADXL345_RATE_800    0x0D
#define ADXL345_RATE_1600   0x0E
#define ADXL345_RATE_3200   0x0F

// various register values
#define ADXL345_POWER_MEAS  0x08
#define ADXL345_FULL_RANGE  0x08
#define ADXL345_RANGE_2G    0x00
#define ADXL345_RANGE_4G    0x01
#define ADXL345_RANGE_8G    0x02
#define ADXL345_RANGE_16G   0x03
#define ADXL345_FIFO_STREAM 0x80


static void adxl345Init(void);
static void adxl345Read(int16_t *accelData);
static void adxl345Align(int16_t *accelData);

static bool useFifo = false;

bool adxl345Detect(drv_adxl345_config_t *init, sensor_t *acc)
{
    bool ack = false;
    uint8_t sig = 0;

    ack = i2cRead(ADXL345_ADDRESS, 0x00, 1, &sig);
    if (!ack || sig != 0xE5)
        return false;

    // use ADXL345's fifo to filter data or not
    useFifo = init->useFifo;

    acc->init = adxl345Init;
    acc->read = adxl345Read;
    acc->align = adxl345Align;
    return true;
}

static void adxl345Init(void)
{
   if (useFifo) {
        uint8_t fifoDepth = 16;
        i2cWrite(ADXL345_ADDRESS, ADXL345_POWER_CTL, ADXL345_POWER_MEAS);
        i2cWrite(ADXL345_ADDRESS, ADXL345_DATA_FORMAT, ADXL345_FULL_RANGE | ADXL345_RANGE_8G);
        i2cWrite(ADXL345_ADDRESS, ADXL345_BW_RATE, ADXL345_RATE_400);
        i2cWrite(ADXL345_ADDRESS, ADXL345_FIFO_CTL, (fifoDepth & 0x1F) | ADXL345_FIFO_STREAM);
    } else {
        i2cWrite(ADXL345_ADDRESS, ADXL345_POWER_CTL, ADXL345_POWER_MEAS);
        i2cWrite(ADXL345_ADDRESS, ADXL345_DATA_FORMAT, ADXL345_FULL_RANGE | ADXL345_RANGE_8G);
        i2cWrite(ADXL345_ADDRESS, ADXL345_BW_RATE, ADXL345_RATE_100);
    }
}

uint8_t acc_samples = 0;

static void adxl345Read(int16_t *accelData)
{
    uint8_t buf[8];

    if (useFifo) {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        uint8_t i = 0;
        uint8_t samples_remaining;

        do {
            i++;
            i2cRead(ADXL345_ADDRESS, ADXL345_DATA_OUT, 8, buf);
            x += (int16_t)(buf[0] + (buf[1] << 8));
            y += (int16_t)(buf[2] + (buf[3] << 8));
            z += (int16_t)(buf[4] + (buf[5] << 8));
            samples_remaining = buf[7] & 0x7F;
        } while ((i < 32) && (samples_remaining > 0));
        accelData[0] = x / i;
        accelData[1] = y / i;
        accelData[2] = z / i;
        acc_samples = i;
    } else {
        i2cRead(ADXL345_ADDRESS, ADXL345_DATA_OUT, 6, buf);
        accelData[0] = buf[0] + (buf[1] << 8);
        accelData[1] = buf[2] + (buf[3] << 8);
        accelData[2] = buf[4] + (buf[5] << 8);
    }
}

static void adxl345Align(int16_t *accData)
{
    // official direction is RPY, nothing to change here.
}
