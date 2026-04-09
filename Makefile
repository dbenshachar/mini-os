AS = aarch64-elf-as
CC = aarch64-elf-gcc
LD = aarch64-elf-ld
OBJCOPY = aarch64-elf-objcopy
QEMU = qemu-system-aarch64

CFLAGS = -ffreestanding -fno-pic -Iinclude -Isrc -c
LDFLAGS = -nostdlib -T linker.ld

SRCS = $(wildcard src/*.c) kernel.c
OBJS = out/start.o $(patsubst %.c,out/%.o,$(SRCS))

all: kernel.bin

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary kernel.elf kernel.bin

kernel.elf: $(OBJS)
	$(LD) $(LDFLAGS) -o kernel.elf $(OBJS)

out/start.o: start.S
	@mkdir -p $(dir $@)
	$(AS) start.S -o $@

out/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

run: kernel.bin
	$(QEMU) -M virt -cpu cortex-a57 -nographic -serial mon:stdio \
	   -kernel kernel.elf \
	   -drive if=none,file=disk.img,format=raw,id=hd0 \
	   -device virtio-blk-device,drive=hd0

clean:
	rm -rf out/ kernel.elf kernel.bin

.PHONY: all run clean