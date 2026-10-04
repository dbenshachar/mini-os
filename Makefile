AS = aarch64-elf-as
CC = aarch64-elf-gcc
LD = aarch64-elf-ld
OBJCOPY = aarch64-elf-objcopy
QEMU = qemu-system-aarch64
HOSTCC ?= cc

CFLAGS = -ffreestanding -fno-pic -mgeneral-regs-only -MMD -MP -Iinclude -Isrc -c
LDFLAGS = -nostdlib -T linker.ld

SRCS = $(wildcard src/*.c) $(wildcard src/program/*.c) kernel.c
OBJS = out/start.o $(patsubst %.c,out/%.o,$(SRCS))

-include $(OBJS:.o=.d)

.DEFAULT_GOAL := all

ACCEL ?= tcg
CPU   ?= cortex-a57
DISK  ?= disk.img

all: run

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary kernel.elf kernel.bin

kernel.elf: $(OBJS)
	$(LD) $(LDFLAGS) -o kernel.elf $(OBJS)

out/start.o: start.S
	@mkdir -p $(dir $@)
	$(AS) start.S -o $@

out/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

out/mkfat32: tools/mkfat32.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c99 -Wall -Wextra -o $@ $<

format-disk: out/mkfat32
	./out/mkfat32 disk.img

out/heap-test: src/heap.c src/heap.h test/heap_test.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c99 -Wall -Wextra -Isrc -Dmalloc=mini_malloc -Dfree=mini_free src/heap.c test/heap_test.c -o $@

example-disk: out/mkfat32
	./out/mkfat32 out/examples.img examples/features.mos examples/copy.mos examples/http.mos

test:
	bash test/test.sh

run: kernel.bin
	$(QEMU) -M virt -cpu cortex-a57 -nographic -serial mon:stdio \
	   -kernel kernel.elf \
	   -drive if=none,file=$(DISK),format=raw,id=hd0 \
	   -device virtio-blk-device,drive=hd0 \
	   -netdev user,id=net0 -device virtio-net-device,netdev=net0

bench: kernel.bin
	$(QEMU) -M virt -accel $(ACCEL) -cpu $(CPU) -m 128M -smp 1 \
	   -nodefaults -nographic -monitor none -serial stdio \
	   -kernel kernel.elf \
	   -drive if=none,file=$(DISK),format=raw,id=hd0 \
	   -device virtio-blk-device,drive=hd0 \
	   -netdev user,id=net0 -device virtio-net-device,netdev=net0 \
	   -action shutdown=poweroff

clean:
	rm -rf out/ kernel.elf kernel.bin clean_usage
	lsof -t disk.img | xargs kill -9

.PHONY: all run format-disk example-disk test clean
