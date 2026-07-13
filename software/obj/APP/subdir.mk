################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../APP/hidkbd.c \
../APP/hidkbd_main.c \
../APP/usb_dev.c 

C_DEPS += \
./APP/hidkbd.d \
./APP/hidkbd_main.d \
./APP/usb_dev.d 

OBJS += \
./APP/hidkbd.o \
./APP/hidkbd_main.o \
./APP/usb_dev.o 

DIR_OBJS += \
./APP/*.o \

DIR_DEPS += \
./APP/*.d \

DIR_EXPANDS += \
./APP/*.234r.expand \


# Each subdirectory must supply rules for building sources it contributes
APP/%.o: ../APP/%.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"e:/Projects/at_node/software/Startup" -I"e:/Projects/at_node/software/APP/include" -I"e:/Projects/at_node/software/Profile/include" -I"e:/Projects/at_node/software/StdPeriphDriver/inc" -I"e:/Projects/at_node/software/HAL/include" -I"e:/Projects/at_node/software/Ld" -I"e:/Projects/at_node/software/LIB" -I"e:/Projects/at_node/software/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

