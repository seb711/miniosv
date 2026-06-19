/*
 * Copyright (C) 2013 Nodalink, SARL.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */
#ifndef _OSV_DRIVER_ACPI_HH_
#define _OSV_DRIVER_ACPI_HH_

#include <cstdint>
#include <cstring>

namespace acpi {

// Physical address of the RSDP, when handed to us directly by a PVH boot.
// Zero means we have to scan the BIOS memory for it instead.
extern uint64_t pvh_rsdp_paddr;

// True once the ACPI tables have been located and parsed.
bool is_enabled();

// Minimal subset of the ACPI table definitions that OSv actually uses. These
// used to come from the ACPICA library; we only need to parse the MADT table
// (for SMP), so the few structs are inlined here instead.
#pragma pack(push, 1)

struct table_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    char     asl_compiler_id[4];
    uint32_t asl_compiler_revision;
};

// Multiple APIC Description Table ("APIC"), a.k.a. MADT.
struct madt {
    table_header header;
    uint32_t     local_apic_address;
    uint32_t     flags;
};

struct madt_subtable {
    uint8_t type;
    uint8_t length;
};

enum madt_subtable_type {
    MADT_LOCAL_APIC   = 0,
    MADT_LOCAL_X2APIC = 9,
};

// "Processor enabled" bit in the per-cpu MADT entries.
constexpr uint32_t MADT_ENABLED = 1;

struct madt_local_apic {
    madt_subtable header;
    uint8_t  processor_id;
    uint8_t  apic_id;
    uint32_t flags;
};

struct madt_local_x2apic {
    madt_subtable header;
    uint16_t reserved;
    uint32_t apic_id;
    uint32_t flags;
    uint32_t uid;
};

// aarch64 MADT subtables describing the Generic Interrupt Controller. They are
// parsed exactly like the x86 entries above (acpi::find_table(ACPI_SIG_MADT)
// then walk the subtables), so there is a single ACPI model across both arches.
enum madt_gic_subtable_type {
    MADT_GICC     = 0x0b,   // GIC CPU interface (one per cpu)
    MADT_GICD     = 0x0c,   // GIC distributor
    MADT_GIC_MSI  = 0x0d,   // GIC MSI frame
    MADT_GICR     = 0x0e,   // GIC redistributor
    MADT_GIC_ITS  = 0x0f,   // GIC interrupt translation service
};

struct madt_gicc {
    madt_subtable header;
    uint16_t reserved;
    uint32_t cpu_interface_number;
    uint32_t acpi_processor_uid;
    uint32_t flags;
    uint32_t parking_protocol_version;
    uint32_t performance_interrupt_gsiv;
    uint64_t parked_address;
    uint64_t physical_base_address;   // GICv2 CPU interface base
    uint64_t gicv;
    uint64_t gich;
    uint32_t vgic_maintenance_interrupt;
    uint64_t gicr_base_address;       // per-cpu GICv3 redistributor
    uint64_t mpidr;
};

struct madt_gicd {
    madt_subtable header;
    uint16_t reserved;
    uint32_t gic_id;
    uint64_t physical_base_address;   // distributor base
    uint32_t system_vector_base;
    uint8_t  gic_version;             // 2 or 3
    uint8_t  reserved2[3];
};

struct madt_gicr {
    madt_subtable header;
    uint16_t reserved;
    uint64_t discovery_range_base_address;
    uint32_t discovery_range_length;
};

struct madt_gic_its {
    madt_subtable header;
    uint16_t reserved;
    uint32_t its_id;
    uint64_t physical_base_address;
    uint32_t reserved2;
};

// PCI Express memory-mapped configuration ("MCFG"). Like Nanos, we read only
// this flat table for the ECAM config base; the firmware-programmed BARs are
// used as-is (no AML/_CRS), and device interrupts use MSI-X via the GIC ITS.
struct mcfg {
    table_header header;
    uint64_t     reserved;
    // followed by one or more mcfg_alloc entries
};

struct mcfg_alloc {
    uint64_t base_address;    // ECAM base for this segment
    uint16_t pci_segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
};

#pragma pack(pop)

// 4-character table signatures.
#define ACPI_SIG_MADT "APIC"
#define ACPI_SIG_MCFG "MCFG"

// Return a pointer to the (already mapped) ACPI table with the given 4-byte
// signature, or nullptr if no such table is present.
const table_header *find_table(const char *signature);

// Attempt to power the machine off via the ACPI S5 ("soft off") state. Returns
// false if the firmware did not give us enough information to do so (in which
// case the caller should fall back to a hard reset); on success it does not
// return.
bool power_off();

}

#endif //!_OSV_DRIVER_ACPI_HH_
