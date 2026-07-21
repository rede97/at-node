################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################
-include ../makefile.init

RM := rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include StdPeriphDriver/subdir.mk
-include Startup/subdir.mk
-include RVMSIS/subdir.mk
-include APP/HWS/subdir.mk
-include APP/BLE/subdir.mk
-include APP/subdir.mk
-include subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(S_DEPS)),)
-include $(S_DEPS)
endif
ifneq ($(strip $(S_UPPER_DEPS)),)
-include $(S_UPPER_DEPS)
endif
ifneq ($(strip $(ASM_DEPS)),)
-include $(ASM_DEPS)
endif
ifneq ($(strip $(ASM_UPPER_DEPS)),)
-include $(ASM_UPPER_DEPS)
endif
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
endif

-include ../makefile.defs

# Add inputs and outputs from these tool invocations to the build variables 
SECONDARY_FLASH += \
at-node.hex \

SECONDARY_LIST += \
at-node.lst \

SECONDARY_SIZE += \
at-node.siz \


# All Target
all: 
	$(MAKE) --no-print-directory main-build 

# Firmware variant: default = keyboard (Peripheral) firmware.
#   make main-build DONGLE=1   → BLE HID receiver (dongle) firmware
# CFG_DEFS is referenced by every */subdir.mk compile rule.
# NOTE: make does not track flag changes — always `make clean` when
# switching variants (tools/ci/build_all.sh handles this for you).
ifeq ($(DONGLE),1)
CFG_DEFS := -DBLE_DONGLE=TRUE
else
CFG_DEFS :=
endif

main-build: at-node.elf secondary-outputs

# Tool invocations
at-node.elf: $(OBJS) $(USER_OBJS_ESCAPE)
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -T "../Ld/Link.ld" -nostartfiles -Xlinker --gc-sections -L".." -L"../LIB" -L"../StdPeriphDriver" -Xlinker --print-memory-usage -Wl,-Map,"at-node.map" --specs=nano.specs --specs=nosys.specs -o "at-node.elf" $(OBJS) $(LIBS) -lprintf
at-node.hex: at-node.elf
	@	riscv-none-embed-objcopy -O ihex "at-node.elf" "at-node.hex"
at-node.lst: at-node.elf
	@	riscv-none-embed-objdump --source --all-headers --demangle --line-numbers --wide "at-node.elf" > "at-node.lst"
at-node.siz: at-node.elf
	riscv-none-embed-size --format=berkeley -x --totals "at-node.elf"

# Other Targets
clean:
	-$(RM) $(DIR_OBJS) $(SECONDARY_FLASH)$(SECONDARY_LIST)$(SECONDARY_SIZE) at-node.elf
	-$(RM) $(DIR_EXPANDS) $(CALLGRAPH_DOT)
	-$(RM) $(DIR_DEPS)
secondary-outputs: $(SECONDARY_FLASH) $(SECONDARY_LIST) $(SECONDARY_SIZE)

.PHONY: all clean dependents

-include ../makefile.targets