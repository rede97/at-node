################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../APP/hidkbd.c \
../APP/hidkbd_main.c 

C_DEPS += \
./APP/hidkbd.d \
./APP/hidkbd_main.d 

OBJS += \
./APP/hidkbd.o \
./APP/hidkbd_main.o 

DIR_OBJS += \
./APP/*.o \

DIR_DEPS += \
./APP/*.d \

DIR_EXPANDS += \
./APP/*.234r.expand \


# Each subdirectory must supply rules for building sources it contributes
APP/%.o: ../APP/%.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/Startup" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/APP/include" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/Profile/include" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/StdPeriphDriver/inc" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/HAL/include" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/Ld" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/LIB" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

