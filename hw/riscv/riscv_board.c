/*
 * QEMU RISC-V Generic Board Support
 *
 * Author: Sagar Karandikar, sagark@eecs.berkeley.edu
 *
 * This provides a RISC-V Board with the following devices:
 *
 * 0) HTIF Syscall Proxy
 * 1) HTIF Console
 * 2) HTIF Block Device
 *
 * These are created by htif_mm_init below. Note that some of these devices
 * are no longer available for use by riscv-linux (block device) because
 * the driver was removed.
 *
 * The following "Shim" devices allow support for interrupts triggered by the
 * processor itself (writes to the MIP/SIP CSRs):
 *
 * softint0 - SSIP
 * softint1 - STIP
 * softint2 - MSIP
 *
 * These are created by softint_mm_init below.
 *
 * This board currently uses a hardcoded devicetree that indicates one hart.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/char/serial.h"
#include "hw/riscv/riscv_softint.h"
#include "hw/riscv/htif/htif.h"
#include "hw/riscv/htif/frontend.h"
#include "hw/riscv/riscv_rtc.h"
#include "hw/block/fdc.h"
#include "net/net.h"
#include "hw/boards.h"
#include "hw/i2c/smbus.h"
#include "block/block.h"
#include "hw/block/flash.h"
#include "block/block_int.h" // move later
#include "hw/riscv/riscv.h"
#include "hw/riscv/cpudevs.h"
#include "hw/pci/pci.h"
#include "sysemu/char.h"
#include "sysemu/sysemu.h"
#include "sysemu/arch_init.h"
#include "qemu/log.h"
#include "hw/riscv/bios.h"
#include "hw/ide.h"
#include "hw/loader.h"
#include "elf.h"
#include "hw/timer/mc146818rtc.h"
#include "hw/timer/i8254.h"
#include "sysemu/blockdev.h"
#include "exec/address-spaces.h"
#include "hw/sysbus.h"             /* SysBusDevice */
#include "qemu/host-utils.h"
#include "sysemu/qtest.h"
#include "qemu/error-report.h"
#include "hw/empty_slot.h"
#include "qemu/error-report.h"
#include "sysemu/block-backend.h"

#define TYPE_RISCV_BOARD "riscv-board"
#define RISCV_BOARD(obj) OBJECT_CHECK(BoardState, (obj), TYPE_RISCV_BOARD)

typedef struct {
    SysBusDevice parent_obj;
} BoardState;

static struct _loaderparams {
    int ram_size;
    const char *kernel_filename;
    const char *kernel_cmdline;
    const char *initrd_filename;
} loaderparams;

uint64_t identity_translate(void *opaque, uint64_t addr)
{
    return addr;
}

static int64_t load_kernel (void)
{
    int64_t kernel_entry, kernel_high;
    int big_endian;
    big_endian = 0;

    if (load_elf(loaderparams.kernel_filename, identity_translate, NULL,
                 (uint64_t *)&kernel_entry, NULL, (uint64_t *)&kernel_high,
                 big_endian, ELF_MACHINE, 1) < 0) {
        fprintf(stderr, "qemu: could not load kernel '%s'\n",
                loaderparams.kernel_filename);
        exit(1);
    }
    return kernel_entry;
}

static void main_cpu_reset(void *opaque)
{
    RISCVCPU *cpu = opaque;
    cpu_reset(CPU(cpu));
}

static void riscv_board_init(MachineState *args)
{
    ram_addr_t ram_size = args->ram_size;
    const char *cpu_model = args->cpu_model;
    const char *kernel_filename = args->kernel_filename;
    const char *kernel_cmdline = args->kernel_cmdline;
    const char *initrd_filename = args->initrd_filename;
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *main_mem = g_new(MemoryRegion, 1);
    RISCVCPU *cpu;
    CPURISCVState *env;
    int i;

    DriveInfo *htifbd_drive;
    const char *htifbd_fname; // htif block device filename

    DeviceState *dev = qdev_create(NULL, TYPE_RISCV_BOARD);

    object_property_set_bool(OBJECT(dev), true, "realized", NULL);

    /* Make sure the first 3 serial ports are associated with a device. */
    for(i = 0; i < 3; i++) {
        if (!serial_hds[i]) {
            char label[32];
            snprintf(label, sizeof(label), "serial%d", i);
            serial_hds[i] = qemu_chr_new(label, "null", NULL);
        }
    }

    /* init CPUs */
    if (cpu_model == NULL) {
        cpu_model = "riscv-generic";
    }

    for (i = 0; i < smp_cpus; i++) {
        cpu = cpu_riscv_init(cpu_model);
        if (cpu == NULL) {
            fprintf(stderr, "Unable to find CPU definition\n");
            exit(1);
        }
        env = &cpu->env;

        /* Init internal devices */
        cpu_riscv_irq_init_cpu(env);
        cpu_riscv_clock_init(env);
        qemu_register_reset(main_cpu_reset, cpu);
    }
    cpu = RISCV_CPU(first_cpu);
    env = &cpu->env;

    /* register system main memory (actual RAM) */
    memory_region_init_ram(main_mem, NULL, "riscv_board.ram", 2147483648L + ram_size, &error_fatal);
    // for CSR_MIOBASE
    env->memsize = ram_size;
    vmstate_register_ram_global(main_mem);
    memory_region_add_subregion(system_memory, 0x0, main_mem);

    if (kernel_filename) {
        loaderparams.ram_size = ram_size;
        loaderparams.kernel_filename = kernel_filename;
        loaderparams.kernel_cmdline = kernel_cmdline;
        loaderparams.initrd_filename = initrd_filename;
        load_kernel();
    }
    // TODO: still necessary?
    // write memory amount in MiB to 0x0
    //stl_p(memory_region_get_ram_ptr(main_mem), ram_size >> 20);

    uint32_t reset_vec[8] = {
        0x297 + 0x80000000 - 0x1000, // reset vector
        0x00028067,                  // jump to DRAM_BASE
        0x00000000,                  // reserved
        0x0,                         // config string pointer
        0, 0, 0, 0                   // trap vector
    };
    reset_vec[3] = 0x1000 + sizeof(reset_vec); // config string pointer

    // part one of config string - before memory size specified
    const char * config_string1 = "platform {\n"
        "  vendor ucb;\n"
        "  arch spike;\n"
        "};\n"
        "rtc {\n"
        "  addr 0x" "40000000" ";\n"
        "};\n"
        "ram {\n"
        "  0 {\n"
        "    addr 0x" "80000000" ";\n"
        "    size 0x";
   
   
    // part two of config string - after memory size specified 
    const char * config_string2 =  ";\n"
        "  };\n"
        "};\n"
        "core {\n"
        "  0" " {\n"
          "    " "0 {\n"
          "      isa " "rv64imafd" ";\n"
          "      timecmp 0x" "40000008" ";\n"
          "      ipi 0x" "40001000" ";\n"
          "    };\n"
          "  };\n"
          "};\n";

    // build config string with supplied memory size
    uint64_t rsz = ram_size;
    char * ramsize_as_hex_str = malloc(17);
    sprintf(ramsize_as_hex_str, "%016lx", rsz);
    char * config_string = malloc(strlen(config_string1) + strlen(ramsize_as_hex_str) + strlen(config_string2) + 1);
    config_string[0] = 0;
    strcat(config_string, config_string1);
    strcat(config_string, ramsize_as_hex_str);
    strcat(config_string, config_string2);

    // copy in the reset vec and configstring
    int q;
    for (q = 0; q < sizeof(reset_vec)/sizeof(reset_vec[0]); q++) {
        stl_p(memory_region_get_ram_ptr(main_mem)+0x1000+q*4, reset_vec[q]);
    }

    int confstrlen = strlen(config_string);
    for (q = 0; q < confstrlen; q++) {
        stb_p(memory_region_get_ram_ptr(main_mem)+reset_vec[3]+q, config_string[q]);
    }

    // add serial device 0x3f8-0x3ff
    // serial_mm_init(system_memory, 0xF0000400, 0, env->irq[5], 1843200/16,
    //         serial_hds[0], DEVICE_NATIVE_ENDIAN);

    // setup HTIF Block Device if one is specified as -hda FILENAME
    // NOTE: this is no longer supported by riscv-linux
    htifbd_drive = drive_get_by_index(IF_IDE, 0);
    if (NULL == htifbd_drive) {
        htifbd_fname = NULL;
    } else {
        htifbd_fname = blk_bs(blk_by_legacy_dinfo(htifbd_drive))->filename;
        // get rid of orphaned drive warning, until htif uses the real blockdev
        htifbd_drive->is_default = true;
    }

    // add memory mapped htif registers at location specified in the symbol
    // table of the elf being loaded (thus kernel_filename is passed to the 
    // init rather than an address)
    htif_mm_init(system_memory, kernel_filename, env->irq[4], main_mem,
            htifbd_fname, kernel_cmdline, env, serial_hds[0]);

    // timer device at 0x40000000, as specified in the config string above
    timer_mm_init(system_memory, 0x40000000, env);

    // Softint "devices" for cleaner handling of CPU-triggered interrupts
    softint_mm_init(system_memory, 0xFFFFFFFFF0000020L, env->irq[1], main_mem,
            env, "SSIP");
    softint_mm_init(system_memory, 0xFFFFFFFFF0000040L, env->irq[2], main_mem,
            env, "STIP");
    softint_mm_init(system_memory, 0xFFFFFFFFF0000060L, env->irq[3], main_mem,
            env, "MSIP");

    // TODO: VIRTIO
}

static int riscv_board_sysbus_device_init(SysBusDevice *sysbusdev)
{
    return 0;
}

static void riscv_board_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);
    k->init = riscv_board_sysbus_device_init;
}

static const TypeInfo riscv_board_device = {
    .name          = TYPE_RISCV_BOARD,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BoardState),
    .class_init    = riscv_board_class_init,
};

static void riscv_board_machine_init(MachineClass *mc)
{
    mc->desc = "RISC-V Generic Board";
    mc->init = riscv_board_init;
    mc->max_cpus = 1;
    mc->is_default = 1;
}

DEFINE_MACHINE("riscv", riscv_board_machine_init)

static void riscv_board_register_types(void)
{
    type_register_static(&riscv_board_device);
}

type_init(riscv_board_register_types)
