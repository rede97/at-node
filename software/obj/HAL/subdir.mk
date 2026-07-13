################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../HAL/KEY.c \
../HAL/LED.c \
../HAL/MCU.c \
../HAL/RTC.c \
../HAL/SLEEP.c 

C_DEPS += \
./HAL/KEY.d \
./HAL/LED.d \
./HAL/MCU.d \
./HAL/RTC.d \
./HAL/SLEEP.d 

OBJS += \
./HAL/KEY.o \
./HAL/LED.o \
./HAL/MCU.o \
./HAL/RTC.o \
./HAL/SLEEP.o 

DIR_OBJS += \
./HAL/*.o \

DIR_DEPS += \
./HAL/*.d \

DIR_EXPANDS += \
./HAL/*.234r.expand \


# Each subdirectory must supply rules for building sources it contributes
HAL/%.o: ../HAL/%.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/Startup" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/APP/include" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/Profile/include" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/StdPeriphDriver/inc" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/HAL/include" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/Ld" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/LIB" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

