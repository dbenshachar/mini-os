TARGET := riscv32-unknown-none
AS := riscv64-elf-as
CC := riscv64-elf-gcc
CCFLAGS := -Wall -ggdb -ffreestanding -I./include -static -mabi=ilp32 -g -O0
RISCVFLAGS := -march=rv32i 
LD := riscv64-elf-ld
VERSION := debug

SRC_DIR := src
ASM_DIR := $(SRC_DIR)/asm
OS_DIR := $(SRC_DIR)/os

BUILD_DIR := build

BOOT_ASM := $(ASM_DIR)/start.S
LINKER_SCRIPT := linker.ld

KERNEL_C := $(SRC_DIR)/main.c

BOOT_OBJ := $(BUILD_DIR)/boot.o
KERNEL_OBJ_DIR := target/$(TARGET)/$(VERSION)
KERNEL_OBJ := $(KERNEL_OBJ_DIR)/lib.a
KERNEL_ELF := kernel.elf

QEMU := qemu-system-riscv32
QEMU_FLAGS := -M virt -bios none -serial stdio -vga none -device ramfb

ASM_SRC = $(shell find $(OS_DIR) -name *.S)
USR_SRC = $(shell find $(SRC_DIR) -name *.c)

SRC := $(ASM_SRC) $(USR_SRC) 

OBJ := $(patsubst %.c,   $(BUILD_DIR)/%.o, $(filter %.c,$(SRC))) \
       $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(filter %.cpp,$(SRC))) \
       $(patsubst %.S,   $(BUILD_DIR)/%.o, $(filter %.S,$(SRC)))

all: $(KERNEL_ELF)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC)  $(CCFLAGS) $(RISCVFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC)  $(CCFLAGS) $(RISCVFLAGS) -c $< -o $@

$(BOOT_OBJ): $(BOOT_ASM)
	@mkdir -p $(dir $@)
	$(AS) $(RISCVFLAGS) -c $< -o $@

$(KERNEL_OBJ): $(KERNEL_C)
	@mkdir -p $(dir $@)
	$(CC) $(RISCVFLAGS) $(CCFLAGS) -c $< -o $@

# $(KERNEL_ELF): $(LINKER_SCRIPT) $(SRC)
# 	@mkdir -p $(dir $@)
# 	@echo $(OBJ)
# 	$(CC) $(RISCVFLAGS) $(CCFLAGS) -Wl,-T $(LINKER_SCRIPT) -Wl,-m elf32lriscv $(SRC) -o $@

$(KERNEL_ELF): $(BOOT_OBJ) $(KERNEL_OBJ) $(LINKER_SCRIPT) $(OBJ)
	@mkdir -p $(dir $@)
	@echo $(OBJ)
	$(LD) -o $(KERNEL_ELF) -T $(LINKER_SCRIPT) -m elf32lriscv $(BOOT_OBJ) $(OBJ)
 
clean:
	rm -rf target
	rm -r $(BUILD_DIR)
	rm -f $(BOOT_OBJ) $(KERNEL_ELF)

run:
	$(QEMU) $(QEMU_FLAGS) -kernel ${KERNEL_ELF}

blob:
	$(QEMU) $(QEMU_FLAGS) -machine dumpdtb=virt.dtb 
