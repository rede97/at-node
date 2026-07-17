################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../APP/HWS/hws_batt.c \
../APP/HWS/hws_core.c \
../APP/HWS/hws_key.c \
../APP/HWS/hws_led.c \
../APP/HWS/hws_rtc.c \
../APP/HWS/hws_sleep.c 

C_DEPS += \
./APP/HWS/hws_batt.d \
./APP/HWS/hws_core.d \
./APP/HWS/hws_key.d \
./APP/HWS/hws_led.d \
./APP/HWS/hws_rtc.d \
./APP/HWS/hws_sleep.d 

OBJS += \
./APP/HWS/hws_batt.o \
./APP/HWS/hws_core.o \
./APP/HWS/hws_key.o \
./APP/HWS/hws_led.o \
./APP/HWS/hws_rtc.o \
./APP/HWS/hws_sleep.o 

DIR_OBJS += \
./APP/HWS/*.o \

DIR_DEPS += \
./APP/HWS/*.d \

DIR_EXPANDS += \
./APP/HWS/*.234r.expand \


# Each subdirectory must supply rules for building sources it contributes
APP/HWS/%.o: ../APP/HWS/%.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"e:/Projects/at_node/software/Startup" -I"e:/Projects/at_node/software/APP/include" -I"e:/Projects/at_node/software/StdPeriphDriver/inc" -I"e:/Projects/at_node/software/Ld" -I"e:/Projects/at_node/software/LIB" -I"e:/Projects/at_node/software/RVMSIS" -I"e:/Projects/at_node/software/APP/BLE" -I"e:/Projects/at_node/software/APP/HWS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

