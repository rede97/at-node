################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Profile/battservice.c \
../Profile/devinfoservice.c \
../Profile/hiddev.c \
../Profile/hidkbdservice.c 

C_DEPS += \
./Profile/battservice.d \
./Profile/devinfoservice.d \
./Profile/hiddev.d \
./Profile/hidkbdservice.d 

OBJS += \
./Profile/battservice.o \
./Profile/devinfoservice.o \
./Profile/hiddev.o \
./Profile/hidkbdservice.o 

DIR_OBJS += \
./Profile/*.o \

DIR_DEPS += \
./Profile/*.d \

DIR_EXPANDS += \
./Profile/*.234r.expand \


# Each subdirectory must supply rules for building sources it contributes
Profile/%.o: ../Profile/%.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/Startup" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/APP/include" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/Profile/include" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/StdPeriphDriver/inc" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/HAL/include" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/Ld" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/LIB" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

