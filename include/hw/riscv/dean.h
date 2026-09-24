/*
 * QEMU RISC-V VirtIO machine interface
 *
 * Copyright (c) 2017 zhang di
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_RISCV_QUARD_STAR__H
#define HW_RISCV_QUARD_STAR__H

#include "hw/riscv/riscv_hart.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define DEAN_CPUS_MAX 8
#define DEAN_SOCKETS_MAX 8

#define TYPE_RISCV_DEAN_MACHINE MACHINE_TYPE_NAME("dean-soc")
typedef struct RISCVVirtState RISCVVirtState;
DECLARE_INSTANCE_CHECKER(RISCVVirtState, RISCV_VIRT_MACHINE,
                         TYPE_RISCV_DEAN_MACHINE)

struct RISCVVirtState {
    /*< private >*/
    MachineState parent;

    /*< public >*/
    RISCVHartArrayState soc[DEAN_SOCKETS_MAX];
};

enum {
    DEAN_MROM,
    DEAN_SRAM,
    DEAN_UART0,
    DEAN_DRAM,
};

enum {
    DEAN_UART0_IRQ = 10,
};

#endif
