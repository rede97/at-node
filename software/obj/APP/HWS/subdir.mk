################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../APP/HWS/KEY.c \
../APP/HWS/LED.c \
../APP/HWS/MCU.c \
../APP/HWS/RTC.c \
../APP/HWS/SLEEP.c 

C_DEPS += \
./APP/HWS/KEY.d \
./APP/HWS/LED.d \
./APP/HWS/MCU.d \
./APP/HWS/RTC.d \
./APP/HWS/SLEEP.d 

OBJS += \
./APP/HWS/KEY.o \
./APP/HWS/LED.o \
./APP/HWS/MCU.o \
./APP/HWS/RTC.o \
./APP/HWS/SLEEP.o 

DIR_OBJS += \
./APP/HWS/*.o \

DIR_DEPS += \
./APP/HWS/*.d \

DIR_EXPANDS += \
./APP/HWS/*.234r.expand \


# Each subdirectory must supply rules for building sources it contributes
APP/HWS/%.o: ../APP/HWS/%.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"e:/Projects/at_node/software/Startup" -I"e:/Projects/at_node/software/APP/include" -I"e:/Projects/at_node/software/StdPeriphDriver/inc" -I"e:/Projects/at_node/software/Ld" -I"e:/Projects/at_node/software/LIB" -I"e:/Projects/at_node/software/RVMSIS" -I"e:/Projects/at_node/software/APP/BLE" -I"e:/Projects/at_node/software/APP/HWS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

