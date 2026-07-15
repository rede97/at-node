################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../APP/BLE/battservice.c \
../APP/BLE/devinfoservice.c \
../APP/BLE/hiddev.c \
../APP/BLE/hidkbdservice.c \
../APP/BLE/scanparamservice.c 

C_DEPS += \
./APP/BLE/battservice.d \
./APP/BLE/devinfoservice.d \
./APP/BLE/hiddev.d \
./APP/BLE/hidkbdservice.d \
./APP/BLE/scanparamservice.d 

OBJS += \
./APP/BLE/battservice.o \
./APP/BLE/devinfoservice.o \
./APP/BLE/hiddev.o \
./APP/BLE/hidkbdservice.o \
./APP/BLE/scanparamservice.o 

DIR_OBJS += \
./APP/BLE/*.o \

DIR_DEPS += \
./APP/BLE/*.d \

DIR_EXPANDS += \
./APP/BLE/*.234r.expand \


# Each subdirectory must supply rules for building sources it contributes
APP/BLE/%.o: ../APP/BLE/%.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"e:/Projects/at_node/software/Startup" -I"e:/Projects/at_node/software/APP/include" -I"e:/Projects/at_node/software/StdPeriphDriver/inc" -I"e:/Projects/at_node/software/Ld" -I"e:/Projects/at_node/software/LIB" -I"e:/Projects/at_node/software/RVMSIS" -I"e:/Projects/at_node/software/APP/BLE" -I"e:/Projects/at_node/software/APP/HWS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

