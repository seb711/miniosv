/*
 * Copyright (C) 2026 miniosv contributors
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef BOOT_UEFI_EFI_HH
#define BOOT_UEFI_EFI_HH

//
// Minimal subset of the UEFI specification used by the miniosv boot stub
// (boot/uefi/stub.cc). Only the types, protocols and boot-services entries the
// stub actually touches are declared; unused boot-services slots are kept as
// void* so the struct layout (an ABI contract with the firmware) stays correct.
//
// Self-contained and freestanding: depends only on <stdint.h> so it can be
// compiled into the standalone EFI application without the kernel headers.
//

#include <stdint.h>

// UEFI calling convention. On x86_64 the firmware uses the Microsoft x64 ABI;
// on AArch64 it uses the standard AAPCS64, so the attribute is empty there.
#if defined(__x86_64__)
#define EFIAPI __attribute__((ms_abi))
#else
#define EFIAPI
#endif

typedef uint64_t UINTN;       // register-width unsigned on both LP64 targets
typedef UINTN    EFI_STATUS;
typedef void *   EFI_HANDLE;
typedef void *   EFI_EVENT;
typedef uint16_t CHAR16;
typedef uint8_t  BOOLEAN;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;

#define EFIERR(n)               (((EFI_STATUS)1 << 63) | (n))
#define EFI_SUCCESS             0
#define EFI_LOAD_ERROR          EFIERR(1)
#define EFI_INVALID_PARAMETER   EFIERR(2)
#define EFI_UNSUPPORTED         EFIERR(3)
#define EFI_BUFFER_TOO_SMALL    EFIERR(5)
#define EFI_NOT_FOUND           EFIERR(14)
#define EFI_ERROR(s)            (((EFI_STATUS)(s)) >> 63)

struct EFI_GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
};

static inline bool efi_guid_eq(const EFI_GUID &a, const EFI_GUID &b)
{
    const uint8_t *pa = reinterpret_cast<const uint8_t *>(&a);
    const uint8_t *pb = reinterpret_cast<const uint8_t *>(&b);
    for (unsigned i = 0; i < sizeof(EFI_GUID); i++)
        if (pa[i] != pb[i])
            return false;
    return true;
}

// --- memory ---------------------------------------------------------------

enum EFI_MEMORY_TYPE {
    EfiReservedMemoryType = 0,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType,
};

enum EFI_ALLOCATE_TYPE {
    AllocateAnyPages = 0,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType,
};

struct EFI_MEMORY_DESCRIPTOR {
    uint32_t             Type;
    uint32_t             Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    uint64_t             NumberOfPages;
    uint64_t             Attribute;
};

// --- table header ---------------------------------------------------------

struct EFI_TABLE_HEADER {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
};

// --- simple text output (used for early stub diagnostics) -----------------

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,
                                      CHAR16 *string);
    // remaining members unused
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    void *ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
};

// --- boot services --------------------------------------------------------

struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;

    void *RaiseTPL;
    void *RestoreTPL;

    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE type,
                                       EFI_MEMORY_TYPE mem_type,
                                       UINTN pages,
                                       EFI_PHYSICAL_ADDRESS *memory);
    void *FreePages;
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *map_size,
                                      EFI_MEMORY_DESCRIPTOR *map,
                                      UINTN *map_key,
                                      UINTN *descriptor_size,
                                      uint32_t *descriptor_version);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE pool_type,
                                      UINTN size,
                                      void **buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *buffer);

    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;

    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE handle,
                                        EFI_GUID *protocol,
                                        void **interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;

    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE image_handle,
                                          UINTN map_key);

    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN microseconds);
    void *SetWatchdogTimer;

    void *ConnectController;
    void *DisconnectController;

    EFI_STATUS (EFIAPI *OpenProtocol)(EFI_HANDLE handle,
                                      EFI_GUID *protocol,
                                      void **interface,
                                      EFI_HANDLE agent_handle,
                                      EFI_HANDLE controller_handle,
                                      uint32_t attributes);
    void *CloseProtocol;
    void *OpenProtocolInformation;

    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *protocol,
                                        void *registration,
                                        void **interface);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;

    void *CalculateCrc32;

    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
};

// --- configuration table & system table -----------------------------------

struct EFI_CONFIGURATION_TABLE {
    EFI_GUID VendorGuid;
    void    *VendorTable;
};

struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16          *FirmwareVendor;
    uint32_t         FirmwareRevision;
    EFI_HANDLE       ConsoleInHandle;
    void            *ConIn;
    EFI_HANDLE       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void            *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN            NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
};

// --- loaded image protocol (for the command line / image base) ------------

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    EFI_GUID{0x5b1b31a1, 0x9562, 0x11d2, \
             {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}

struct EFI_LOADED_IMAGE_PROTOCOL {
    uint32_t           Revision;
    EFI_HANDLE         ParentHandle;
    EFI_SYSTEM_TABLE  *SystemTable;
    EFI_HANDLE         DeviceHandle;
    void              *FilePath;
    void              *Reserved;
    uint32_t           LoadOptionsSize;
    void              *LoadOptions;
    void              *ImageBase;
    uint64_t           ImageSize;
    EFI_MEMORY_TYPE    ImageCodeType;
    EFI_MEMORY_TYPE    ImageDataType;
    void              *Unload;
};

// --- ACPI configuration-table GUIDs ---------------------------------------

#define EFI_ACPI_20_TABLE_GUID \
    EFI_GUID{0x8868e871, 0xe4f1, 0x11d3, \
             {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}}

#define EFI_ACPI_10_TABLE_GUID \
    EFI_GUID{0xeb9d2d30, 0x2d88, 0x11d3, \
             {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}

#endif /* BOOT_UEFI_EFI_HH */
