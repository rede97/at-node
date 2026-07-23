################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../APP/HWS/hws_adc.c \
../APP/HWS/hws_batt.c \
../APP/HWS/hws_core.c \
../APP/HWS/hws_gpio.c \
../APP/HWS/hws_i2c.c \
../APP/HWS/hws_ir.c \
../APP/HWS/hws_key.c \
../APP/HWS/hws_led.c \
../APP/HWS/hws_rtc.c \
../APP/HWS/hws_sleep.c \
../APP/HWS/hws_wdg.c 

C_DEPS += \
./APP/HWS/hws_adc.d \
./APP/HWS/hws_batt.d \
./APP/HWS/hws_core.d \
./APP/HWS/hws_gpio.d \
./APP/HWS/hws_i2c.d \
./APP/HWS/hws_ir.d \
./APP/HWS/hws_key.d \
./APP/HWS/hws_led.d \
./APP/HWS/hws_rtc.d \
./APP/HWS/hws_sleep.d \
./APP/HWS/hws_wdg.d 

OBJS += \
./APP/HWS/hws_adc.o \
./APP/HWS/hws_batt.o \
./APP/HWS/hws_core.o \
./APP/HWS/hws_gpio.o \
./APP/HWS/hws_i2c.o \
./APP/HWS/hws_ir.o \
./APP/HWS/hws_key.o \
./APP/HWS/hws_led.o \
./APP/HWS/hws_rtc.o \
./APP/HWS/hws_sleep.o \
./APP/HWS/hws_wdg.o 

DIR_OBJS += \
./APP/HWS/*.o \

DIR_DEPS += \
./APP/HWS/*.d \

DIR_EXPANDS += \
./APP/HWS/*.234r.expand \


# Each subdirectory must supply rules for building sources it contributes
APP/HWS/%.o: ../APP/HWS/%.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 $(CFG_DEFS) -I"../Startup" -I"../APP/include" -I"../StdPeriphDriver/inc" -I"../Ld" -I"../LIB" -I"../RVMSIS" -I"../APP/BLE" -I"../APP/HWS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

