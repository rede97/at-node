/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_i2c.c
 * Author             : at-node
 * Description        : HWS I2C — master mode polling driver.
 *
 *   Pins (RB_PIN_I2C=0): SCL=PB13, SDA=PB12, both pull-up inputs.
 *   Event-wait style per the STM32-compatible CH58x I2C API, with
 *   timeouts so a stuck bus can never wedge the AT task.
 ********************************************************************************/

#include "hws_i2c.h"

#if(defined(HWS_I2C)) && (HWS_I2C == TRUE)

#define I2C_TIMEOUT  100000   /* ~10 ms per event wait at 60 MHz */

static int i2c_wait(uint32_t event)
{
    uint32_t t = I2C_TIMEOUT;
    while (!I2C_CheckEvent(event))
        if (--t == 0)
            return -1;
    return 0;
}

void hws_i2c_init(void)
{
    GPIOB_ModeCfg(GPIO_Pin_12 | GPIO_Pin_13, GPIO_ModeIN_PU);
    I2C_Init(I2C_Mode_I2C, HWS_I2C_SPEED_HZ, I2C_DutyCycle_2,
             I2C_Ack_Enable, I2C_AckAddr_7bit, 0x00);
    I2C_Cmd(ENABLE);
}

int hws_i2c_probe(uint8_t addr)
{
    int rc = -1;
    I2C_GenerateSTART(ENABLE);
    if (i2c_wait(I2C_EVENT_MASTER_MODE_SELECT) == 0) {
        I2C_Send7bitAddress(addr << 1, I2C_Direction_Transmitter);
        uint32_t t = I2C_TIMEOUT;
        while (t--) {
            if (I2C_CheckEvent(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED)) { rc = 0; break; }
            if (I2C_GetLastEvent() & 0x00000400 /* AF */) break;
        }
    }
    I2C_GenerateSTOP(ENABLE);
    return rc;
}

int hws_i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len)
{
    if (len == 0)
        return -1;
    /* write register pointer */
    I2C_GenerateSTART(ENABLE);
    if (i2c_wait(I2C_EVENT_MASTER_MODE_SELECT)) goto fail;
    I2C_Send7bitAddress(addr << 1, I2C_Direction_Transmitter);
    if (i2c_wait(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED)) goto fail;
    I2C_SendData(reg);
    if (i2c_wait(I2C_EVENT_MASTER_BYTE_TRANSMITTED)) goto fail;
    /* repeated start, read */
    I2C_GenerateSTART(ENABLE);
    if (i2c_wait(I2C_EVENT_MASTER_MODE_SELECT)) goto fail;
    I2C_Send7bitAddress(addr << 1, I2C_Direction_Receiver);
    if (i2c_wait(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED)) goto fail;
    while (len--) {
        if (len == 0) {           /* NACK + STOP before last byte */
            I2C_AcknowledgeConfig(DISABLE);
            I2C_GenerateSTOP(ENABLE);
        }
        if (i2c_wait(I2C_EVENT_MASTER_BYTE_RECEIVED)) goto fail;
        *buf++ = I2C_ReceiveData();
    }
    I2C_AcknowledgeConfig(ENABLE);
    return 0;
fail:
    I2C_GenerateSTOP(ENABLE);
    I2C_AcknowledgeConfig(ENABLE);
    return -1;
}

int hws_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, uint8_t len)
{
    I2C_GenerateSTART(ENABLE);
    if (i2c_wait(I2C_EVENT_MASTER_MODE_SELECT)) goto fail;
    I2C_Send7bitAddress(addr << 1, I2C_Direction_Transmitter);
    if (i2c_wait(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED)) goto fail;
    I2C_SendData(reg);
    if (i2c_wait(I2C_EVENT_MASTER_BYTE_TRANSMITTING)) goto fail;
    while (len--) {
        I2C_SendData(*data++);
        if (i2c_wait(I2C_EVENT_MASTER_BYTE_TRANSMITTING)) goto fail;
    }
    I2C_GenerateSTOP(ENABLE);
    return 0;
fail:
    I2C_GenerateSTOP(ENABLE);
    return -1;
}

#endif /* HWS_I2C == TRUE */
