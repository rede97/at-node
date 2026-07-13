################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../RVMSIS/core_riscv.c 

C_DEPS += \
./RVMSIS/core_riscv.d 

OBJS += \
./RVMSIS/core_riscv.o 

DIR_OBJS += \
./RVMSIS/*.o \

DIR_DEPS += \
./RVMSIS/*.d \

DIR_EXPANDS += \
./RVMSIS/*.234r.expand \


# Each subdirectory must supply rules for building sources it contributes
RVMSIS/%.o: ../RVMSIS/%.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/Startup" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/APP/include" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/Profile/include" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/StdPeriphDriver/inc" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/HAL/include" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/Ld" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/LIB" -I"e:/Projects/CH582-EVT/EXAM/BLE/HID_Keyboard/software/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

