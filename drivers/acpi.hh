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

#pragma pack(pop)

// 4-character table signatures.
#define ACPI_SIG_MADT "APIC"

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
