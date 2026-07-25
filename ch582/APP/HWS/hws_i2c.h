/********************************** (C) COPYRIGHT *******************************
 * File Name          : hws_i2c.h
 * Author             : at-node
 * Description        : HWS I2C — master mode polling driver (AT commands).
 ********************************************************************************/

#ifndef HWS_I2C_H
#define HWS_I2C_H

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

void hws_i2c_init(void);
int  hws_i2c_probe(uint8_t addr);   /* 0 = ACK, -1 = NAK/err */
int  hws_i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len);
int  hws_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* HWS_I2C_H */
