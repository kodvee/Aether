ISO_NAME = aether

.PHONY: all
all: $(ISO_NAME).iso

# General
EMU_ARGS  = -M q35 -m 2G -boot d -D qlog.txt -d int -no-reboot -M smm=off -no-shutdown -smp 24

# Host-related
EMU_ARGS += -serial stdio -monitor unix:util/qemu-monitor-socket,server,nowait

# ------------------------------------------------------------------ #
# Test framework QEMU args                                            #
#   isa-debug-exit: write 0 to port 0xF4 -> QEMU exit 1 (pass)       #
#                   write 1 to port 0xF4 -> QEMU exit 3 (fail)        #
# ------------------------------------------------------------------ #
TEST_EMU_ARGS  = -M q35 -m 2G -boot d -no-reboot -M smm=off -smp 24
TEST_EMU_ARGS += -serial stdio -display none
TEST_EMU_ARGS += -device isa-debug-exit,iobase=0xf4,iosize=0x04

# Kernel cmdline extra flags are appended via limine.cfg; we patch a
# temp cfg so the main config is not disturbed.
TEST_EXTRA_CFLAGS = -DKTEST_ENABLED

_test-iso:
	$(MAKE) -C kernel clean
	$(MAKE) -C kernel EXTRA_CFLAGS="$(TEST_EXTRA_CFLAGS) $(KTEST_CFLAGS)"
	rm -rf iso_root
	mkdir -p iso_root
	cp kernel/kernel.elf \
		limine/limine.sys limine/limine-cd.bin limine/limine-cd-efi.bin iso_root/
	printf 'TIMEOUT=0\n\n:Aether-test\n    PROTOCOL=limine\n    KERNEL_PATH=boot:///kernel.elf\n    CMDLINE=ktest%s ktest.ci ktest.headless\n' \
		"$(TEST_CMDLINE_EXTRA)" > iso_root/limine.cfg
	xorriso -as mkisofs -b limine-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-cd-efi.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO_NAME)-test.iso 2>/dev/null
	limine/limine-deploy $(ISO_NAME)-test.iso 2>/dev/null
	rm -rf iso_root

define run-test-iso
	@echo "Running tests (mode: $(1))..."
	@timeout 60 qemu-system-x86_64 $(TEST_EMU_ARGS) -cdrom $(ISO_NAME)-test.iso; \
	ec=$$?; \
	if [ $$ec -eq 1 ]; then \
		echo "  [PASSED] All tests passed."; \
	else \
		echo "  [FAILED] Tests failed (QEMU exit code: $$ec)."; \
		exit 1; \
	fi
endef

.PHONY: test
test: limine _test-iso
	$(call run-test-iso,all)

.PHONY: test-critical
test-critical: TEST_CMDLINE_EXTRA==critical
test-critical: limine _test-iso
	$(call run-test-iso,critical)

.PHONY: test-memory
test-memory: TEST_CMDLINE_EXTRA= ktest.sub=memory
test-memory: limine _test-iso
	$(call run-test-iso,memory)

.PHONY: test-panic
test-panic: TEST_CMDLINE_EXTRA==panic
test-panic: limine _test-iso
	$(call run-test-iso,panic)

.PHONY: test-smp
test-smp: TEST_CMDLINE_EXTRA==smp
test-smp: limine _test-iso
	$(call run-test-iso,smp)

.PHONY: stress
stress: TEST_CMDLINE_EXTRA==stress
stress: limine _test-iso
	$(call run-test-iso,stress)

# make test TEST_FILTER=slab  -> runs only tests whose name contains "slab"
.PHONY: test-filter
test-filter: TEST_CMDLINE_EXTRA= ktest.filter=$(TEST_FILTER)
test-filter: limine _test-iso
	$(call run-test-iso,filter=$(TEST_FILTER))

.PHONY: run run-uefi run-hdd run-hdd-uefi
run: $(ISO_NAME).iso
	qemu-system-x86_64 $(EMU_ARGS) -cdrom $(ISO_NAME).iso 

.PHONY: run-uefi
run-uefi: ovmf $(ISO_NAME).iso
	qemu-system-x86_64 $(EMU_ARGS) -bios ovmf/OVMF.fd -cdrom $(ISO_NAME).iso

limine:
	git clone https://github.com/limine-bootloader/limine.git --branch=v4.x-branch-binary --depth=1
	$(MAKE) -C limine

ovmf:
	mkdir -p ovmf
	cd ovmf && curl -Lo OVMF-X64.zip https://efi.akeo.ie/OVMF/OVMF-X64.zip && unzip OVMF-X64.zip

.PHONY: userspace
userspace:
	$(MAKE) -C userspace/init

.PHONY: kernel
kernel: userspace
	$(MAKE) -C kernel

.PHONY: docs
docs:
	doxygen

base.img:
	dd if=/dev/zero of=base.img bs=1M count=10
	mkfs.ext4 base.img
	mkdir -p base_mnt
	sudo mount base.img base_mnt
	sudo cp -r base/* base_mnt/
	sudo umount -l base_mnt
	rm -rf base_mnt

$(ISO_NAME).iso: limine kernel base.img
	rm -rf iso_root
	mkdir -p iso_root
	cp kernel/kernel.elf \
		limine.cfg limine/limine.sys limine/limine-cd.bin limine/limine-cd-efi.bin base.img iso_root/
	xorriso -as mkisofs -b limine-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-cd-efi.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO_NAME).iso
	limine/limine-deploy $(ISO_NAME).iso
	rm -rf iso_root

.PHONY: clean
clean:
	rm -rf iso_root $(ISO_NAME).iso base.img
	rm -rf documentation
	$(MAKE) -C userspace/init clean
	$(MAKE) -C kernel clean
	-rm -f qlog.txt

.PHONY: distclean
distclean: clean
	rm -rf limine ovmf
	$(MAKE) -C kernel distclean
