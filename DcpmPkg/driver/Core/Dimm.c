/*
 * Copyright (c) 2018, Intel Corporation.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <Library/UefiBootServicesTableLib.h>
#include <Guid/SmBios.h>
#include <IndustryStandard/SmBios.h>
#include <Types.h>
#include <NvmDimmDriverData.h>
#include <Debug.h>
#include <Utility.h>
#include "Dimm.h"
#include "Namespace.h"
#include <Utility.h>
#include <SmbiosUtility.h>
#include "AsmCommands.h"
#include <NvmWorkarounds.h>
#include <Convert.h>
#include <NvmDimmDriver.h>
#ifdef OS_BUILD
#include <os_types.h>
#include <Common.h>
#endif

#ifndef OS_BUILD
#include "Smbus.h"
#endif

#define SMBIOS_TYPE_MEM_DEV             17
#define SMBIOS_TYPE_MEM_DEV_MAPPED_ADDR 20
#ifdef PCD_CACHE_ENABLED
int gPCDCacheEnabled = 1;
#else
int gPCDCacheEnabled = 0;
#endif
extern NVMDIMMDRIVER_DATA *gNvmDimmData;
CONST UINT64 gSupportedBlockSizes[SUPPORTED_BLOCK_SIZES_COUNT] = {
  512,  //  512 (default)
  514,  //  512+2 (DIX)
  520,  //  512+8
  528,  //  512+16
  4096, //  512*8
  4112, //  (512+2)*8 (DIX)
  4160, //  (512+8)*8
  4224  //  (512+16)*8
};
#ifndef OS_BUILD
extern EFI_GUID gDcpmmProtocolGuid;
#endif

// All possible combinations of transport and mailbox size
typedef enum _DIMM_PASSTHRU_METHOD {
  DimmPassthruDdrtLargePayload = 0,
  DimmPassthruDdrtSmallPayload = 1,
  DimmPassthruSmbusSmallPayload = 2
} DIMM_PASSTHRU_METHOD;

#ifdef OS_BUILD
/*
* Function get the ini configuration only on the first call
*
* It returns TRUE in case of large payload access is disabled and FALSE otherwise
*/
BOOLEAN ConfigIsLargePayloadDisabled()
{
  static BOOLEAN config_large_payload_initialized = FALSE;
  static UINT8 large_payload_disabled = 0;
  EFI_STATUS efi_status;
  EFI_GUID guid = { 0 };
  UINTN size;

  if (config_large_payload_initialized)
    return large_payload_disabled;

  size = sizeof(large_payload_disabled);
  efi_status = GET_VARIABLE(INI_PREFERENCES_LARGE_PAYLOAD_DISABLED, guid, &size, &large_payload_disabled);
  if ((EFI_SUCCESS != efi_status) || (large_payload_disabled > 1))
    return FALSE;

  config_large_payload_initialized = TRUE;

  return (BOOLEAN)large_payload_disabled;
}

/*
* Function get the ini configuration only on the first call
*
* It returns TRUE in case of DDRT protocol access is disabled and FALSE otherwise
*/
BOOLEAN ConfigIsDdrtProtocolDisabled()
{
  static BOOLEAN config_ddrt_protocol_initialized = FALSE;
  static UINT8 ddrt_protocol_disabled = 0;
  EFI_STATUS efi_status;
  EFI_GUID guid = { 0 };
  UINTN size;

  if (config_ddrt_protocol_initialized)
    return ddrt_protocol_disabled;

  size = sizeof(ddrt_protocol_disabled);
  efi_status = GET_VARIABLE(INI_PREFERENCES_DDRT_PROTOCOL_DISABLED, guid, &size, &ddrt_protocol_disabled);
  if ((EFI_SUCCESS != efi_status) || (ddrt_protocol_disabled > 1))
    return FALSE;

  config_ddrt_protocol_initialized = TRUE;

  return (BOOLEAN)ddrt_protocol_disabled;
}
#endif // OS_BUILD

/**
  Global pointers to the new processor assembler commands:

  gClFlush has more than one implementation and we should store here the newest that the processor supports.

  If the pointers are still NULL - the processor does not support any of the existing implementations.
**/

VOID
(*gClFlush)(
  VOID *pLinearAddress
  );

#ifndef OS_BUILD

struct {
  UINT32 Eax;
  union {
    struct {
      UINT32 Unused:23;
      BOOLEAN ClFlushOpt:1; // EBX.CLFLUSHOPT[bit 23]
      BOOLEAN ClWb:1; // EBX.CLWB[bit 24]
      UINT32 Unused2:7;
    } Separated;
    UINT32 AsUint32;
  } Ebx;
  UINT32 Ecx;
  UINT32 Edx;
} CpuInfo;

/**
  InitializeCpuCommands

  Checks what set of required instructions current processor supports and assigns proper function pointers.
  The detection of new instructions is made, following the document: Ref # 319433-022, chapter 11-1.
**/
STATIC
VOID
InitializeCpuCommands(
  )
{
  SetMem(&CpuInfo, sizeof(CpuInfo), 0x0);

  AsmCpuidEcx(CPUID_NEWMEM_FUNCTIONS_EAX, CPUID_NEWMEM_FUNCTIONS_ECX,
    (UINT32 *)&CpuInfo.Eax, (UINT32 *)&CpuInfo.Ebx, (UINT32 *)&CpuInfo.Ecx, (UINT32 *)&CpuInfo.Edx);

  if (CpuInfo.Ebx.Separated.ClFlushOpt) {
    gClFlush = &AsmClFlushOpt;
    NVDIMM_DBG("Flushing assigned to ClFlushOpt.");
  } else {
    NVDIMM_DBG("Flushing assigned to ClFlush.");
    gClFlush = &AsmFlushCl;
  }

}

#endif /** !OS_BUILD **/


STATIC EFI_STATUS PollOnArsDeviceBusy(IN DIMM *pDimm, IN UINT32 TimeoutSecs);

/**
  Get dimm by Dimm ID
  Scan the dimm list for a dimm identified by Dimm ID

  @param[in] DimmID: The SMBIOS Type 17 handle of the dimm
  @param[in] pDimms: The head of the dimm list

  @retval DIMM struct pointer if matching dimm has been found
  @retval NULL pointer if not found
**/
DIMM *
GetDimmByPid(
  IN     UINT32 DimmID,
  IN     LIST_ENTRY *pDimms
  )
{
  DIMM *pCurDimm = NULL;
  DIMM *pTargetDimm = NULL;
  LIST_ENTRY *pCurDimmNode = NULL;

  NVDIMM_ENTRY();
  for (pCurDimmNode = GetFirstNode(pDimms);
      !IsNull(pDimms, pCurDimmNode);
      pCurDimmNode = GetNextNode(pDimms, pCurDimmNode)) {

    pCurDimm = DIMM_FROM_NODE(pCurDimmNode);

    if (pCurDimm != NULL && DimmID == pCurDimm->DimmID) {
      pTargetDimm = pCurDimm;
      break;
    }
  }

  NVDIMM_EXIT();
  return pTargetDimm;
}

/**
  Get dimm by serial number
  Scan the dimm list for a dimm identified by serial number

  @param[in] pDimms The head of the dimm list
  @param[in] DimmID The serial number of the dimm

  @retval DIMM struct pointer if matching dimm has been found
  @retval NULL pointer if not found
**/
DIMM *
GetDimmByHandle(
  IN     UINT32 DeviceHandle,
  IN     LIST_ENTRY *pDimms
  )
{
  DIMM *pCurDimm = NULL;
  DIMM *pTargetDimm = NULL;
  LIST_ENTRY *pCurDimmNode = NULL;
  NVDIMM_ENTRY();
  for (pCurDimmNode = GetFirstNode(pDimms);
      !IsNull(pDimms, pCurDimmNode);
      pCurDimmNode = GetNextNode(pDimms, pCurDimmNode)) {

    pCurDimm = DIMM_FROM_NODE(pCurDimmNode);

    if (DeviceHandle == pCurDimm->DeviceHandle.AsUint32) {
      pTargetDimm = pCurDimm;
      break;
    }
  }
  NVDIMM_EXIT();
  return pTargetDimm;
}

/**
  Get dimm by serial number
  Scan the dimm list for a dimm identified by serial number
  @param[in] pDimms The head of the dimm list
  @param[in] DimmID The serial number of the dimm
  @retval DIMM struct pointer if matching dimm has been found
  @retval NULL pointer if not found
**/
DIMM *
GetDimmBySerialNumber(
  IN     LIST_ENTRY *pDimms,
  IN     UINT32 SerialNumber
  )
{
  DIMM *pCurDimm = NULL;
  DIMM *pTargetDimm = NULL;
  LIST_ENTRY *pCurDimmNode = NULL;

  NVDIMM_ENTRY();

  LIST_FOR_EACH(pCurDimmNode, pDimms) {
    pCurDimm = DIMM_FROM_NODE(pCurDimmNode);

    if (pCurDimm->SerialNumber == SerialNumber) {
      pTargetDimm = pCurDimm;
      break;
    }
  }

  NVDIMM_EXIT();
  return pTargetDimm;
}

/**
  Get dimm by its unique identifier structure
  Scan the dimm list for a dimm identified by its
  unique identifier structure

  @param[in] pDimms The head of the dimm list
  @param[in] DimmUniqueId The unique identifier structure of the dimm

  @retval DIMM struct pointer if matching dimm has been found
  @retval NULL pointer if not found
**/
DIMM *
GetDimmByUniqueIdentifier(
  IN     LIST_ENTRY *pDimms,
  IN     DIMM_UNIQUE_IDENTIFIER DimmUniqueId
  )
{
  DIMM *pCurDimm = NULL;
  DIMM *pTargetDimm = NULL;
  LIST_ENTRY *pCurDimmNode = NULL;

  NVDIMM_ENTRY();

  LIST_FOR_EACH(pCurDimmNode, pDimms) {
    pCurDimm = DIMM_FROM_NODE(pCurDimmNode);

    if ((pCurDimm->VendorId == DimmUniqueId.ManufacturerId) && (pCurDimm->SerialNumber == DimmUniqueId.SerialNumber) &&
        (pCurDimm->ManufacturingInfoValid ? ((pCurDimm->ManufacturingLocation == DimmUniqueId.ManufacturingLocation) &&
                                             (pCurDimm->ManufacturingDate == DimmUniqueId.ManufacturingDate)): TRUE)) {
      pTargetDimm = pCurDimm;
      break;
    }
  }

  NVDIMM_EXIT();
  return pTargetDimm;
}

/**
  Get DIMM by index in global structure

  @param[in] DimmIndex - Index
  @param[in] pDev - pointer to global structure

  @retval
**/
DIMM *
GetDimmByIndex(
  IN     INT32 DimmIndex,
  IN     PMEM_DEV *pDev
  )
{
  DIMM *pCurDimm = NULL;
  DIMM *pTargetDimm = NULL;
  LIST_ENTRY *pCurDimmNode = NULL;
  INT32 Index = 0;

  NVDIMM_ENTRY();

  for (pCurDimmNode = GetFirstNode(&pDev->Dimms);
      !IsNull(&pDev->Dimms, pCurDimmNode);
      pCurDimmNode = GetNextNode(&pDev->Dimms, pCurDimmNode)) {
    pCurDimm = DIMM_FROM_NODE(pCurDimmNode);
    if (Index == DimmIndex) {
      pTargetDimm = pCurDimm;
      break;
    }
    Index++;
  }

  NVDIMM_EXIT();
  return pTargetDimm;
}

/**

  Get max Dimm ID
  Scan the dimm list for a max Dimm ID

  @param[in] pDimms: The head of the dimm list

  @retval Max Dimm ID or 0 if not found
**/
UINT16
GetMaxPid(
  IN     LIST_ENTRY *pDimms
  )
{
  UINT16 MaxPid = 0;
  DIMM *pCurDimm = NULL;
  LIST_ENTRY *pCurDimmNode = NULL;

  NVDIMM_ENTRY();

  if (pDimms == NULL) {
    goto Finish;
  }

  LIST_FOR_EACH(pCurDimmNode, pDimms) {
    pCurDimm = DIMM_FROM_NODE(pCurDimmNode);
    if (pCurDimm->DimmID > MaxPid) {
      MaxPid = pCurDimm->DimmID;
    }
  }

Finish:
  NVDIMM_EXIT();
  return MaxPid;
}

/**
  Print memory map list. Use for debug purposes only

  @param[in] pMemmap: List head containing memmap range items
**/
VOID
PrintDimmMemmap(
  IN     LIST_ENTRY *pMemmap
  )
{
  LIST_ENTRY *pNode = NULL;
  MEMMAP_RANGE *pRange = NULL;
  UINT16 Index = 0;

  NVDIMM_ENTRY();

  if (pMemmap == NULL) {
    return;
  }

  NVDIMM_DBG("DIMM Memmap:");

  //display the memmap
  LIST_FOR_EACH(pNode, pMemmap) {
    pRange = MEMMAP_RANGE_FROM_NODE(pNode);
    Index++;
    NVDIMM_DBG("#%d %12llx - %12llx (%12llx) ", Index,
        pRange->RangeStartDpa,
        pRange->RangeStartDpa + pRange->RangeLength - 1,
        pRange->RangeLength);
    switch (pRange->RangeType) {
    case MEMMAP_RANGE_VOLATILE:
      NVDIMM_DBG("VOLATILE\n");
      break;
    case MEMMAP_RANGE_RESERVED:
      NVDIMM_DBG("RESERVED\n");
      break;
    case MEMMAP_RANGE_PERSISTENT:
      NVDIMM_DBG("PERSISTENT\n");
      break;
    case MEMMAP_RANGE_IS:
      NVDIMM_DBG("INTERLEAVE SET\n");
      break;
    case MEMMAP_RANGE_IS_MIRROR:
      NVDIMM_DBG("MIRRORED INTERLEAVE SET\n");
      break;
    case MEMMAP_RANGE_IS_NOT_INTERLEAVED:
      NVDIMM_DBG("IS_NOT_INTERLEAVED\n");
      break;
    case MEMMAP_RANGE_APPDIRECT_NAMESPACE:
      NVDIMM_DBG("APPDIRECT NAMESPACE\n");
      break;
    case MEMMAP_RANGE_LAST_USABLE_DPA:
      NVDIMM_DBG("LAST USABLE DPA\n");
      break;
    case MEMMAP_RANGE_FREE:
      NVDIMM_DBG("FREE\n");
      break;
    default:
      NVDIMM_DBG("UNKNOWN\n");
      break;
    }
  }

  NVDIMM_EXIT();
}

VOID
ShowDimmMemmap(
  IN     DIMM *pDimm
  )
{
  LIST_ENTRY *pMemmapList = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL) {
    goto Finish;
  }

  pMemmapList = AllocateZeroPool(sizeof(*pMemmapList));
  if (pMemmapList == NULL) {
    goto Finish;
  }
  InitializeListHead(pMemmapList);
  GetDimmMemmap(pDimm, pMemmapList);
  PrintDimmMemmap(pMemmapList);

Finish:
  if (pMemmapList != NULL) {
    FreeMemmapItems(pMemmapList);
    FREE_POOL_SAFE(pMemmapList);
  }
  NVDIMM_EXIT();
}

/**
  Add DIMM address space region to a linked list in appropriate place
  making sure target list will be already sorted by start DPA

  Function allocates memory for object with range item. It is caller
  responsibility to free this memory after it is no longer needed

  @param[in] pMemmapList Initialized list head to which region items will be added
  @param[in] pDimm Target DIMM structure pointer
  @param[in] Start Start address of a address range to be added
  @param[in] Length Length of address range to be added
  @param[in] Type of the range to be added (Interleave Set, Namespace, etc.)

  @retval EFI_INVALID_PARAMETER Invalid set of parameters provided
  @retval EFI_OUT_OF_RESOURCES Not enough free space on target
  @retval EFI_SUCCESS List correctly retrieved
**/
EFI_STATUS
AddMemmapRange(
  IN     LIST_ENTRY *pMemmapList,
  IN     DIMM *pDimm,
  IN     UINT64 Start,
  IN     UINT64 Length,
  IN     UINT32 Type
  )
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  MEMMAP_RANGE *pMemmapRange = NULL;
  MEMMAP_RANGE *pCurrentRange = NULL;
  MEMMAP_RANGE *pNextRange = NULL;
  LIST_ENTRY *pNode = NULL;
  LIST_ENTRY *pNextNode = NULL;
  BOOLEAN Added = FALSE;

  if (pMemmapList == NULL || pDimm == NULL) {
    goto Finish;
  }

  pMemmapRange = (MEMMAP_RANGE *) AllocateZeroPool(sizeof(*pMemmapRange));
  if (pMemmapRange == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pMemmapRange->Signature = MEMMAP_RANGE_SIGNATURE;
  pMemmapRange->pDimm = pDimm;
  pMemmapRange->RangeType = (UINT16)Type;
  pMemmapRange->RangeStartDpa = Start;
  pMemmapRange->RangeLength = Length;
  NVDIMM_VERB("New memmap range: start=%x length=%x", Start, Length);

  LIST_FOR_EACH(pNode, pMemmapList) {
    pCurrentRange = MEMMAP_RANGE_FROM_NODE(pNode);

    if (IsNodeAtEnd(pMemmapList, pNode)) {
      if (pMemmapRange->RangeStartDpa >= pCurrentRange->RangeStartDpa) {
        /** pMemmapRange->MemmapNode will be inserted after pNode, because pNode is treated as list head **/
        InsertHeadList(pNode, &pMemmapRange->MemmapNode);
        NVDIMM_VERB("Add after the last node.");
      } else {
        /** pMemmapRange->MemmapNode will be inserted before pNode, because pNode is treated as list head **/
        InsertTailList(pNode, &pMemmapRange->MemmapNode);
        NVDIMM_VERB("Add before the last node.");
      }
      Added = TRUE;
      break;
    }

    pNextNode = GetNextNode(pMemmapList, pNode);
    pNextRange = MEMMAP_RANGE_FROM_NODE(pNextNode);
    if (pMemmapRange->RangeStartDpa >= pCurrentRange->RangeStartDpa &&
        pMemmapRange->RangeStartDpa < pNextRange->RangeStartDpa) {
      InsertHeadList(pNode, &pMemmapRange->MemmapNode);
      NVDIMM_VERB("Added in the middle");
      Added = TRUE;
      break;
    }
  }

  if (!Added) {
    InsertTailList(pMemmapList, &pMemmapRange->MemmapNode);
    NVDIMM_VERB("Added at tail");
  }

  ReturnCode = EFI_SUCCESS;

Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Retrieve list of memory regions of a DIMM

  Regions will be delivered in a form of sorted linked list with
  items containing start DPA and length of free ranges and they may overlap.
  Last item on the list will be a last DPA marker in order to point address boundary.

  @param[in] pDimm Target DIMM structure pointer
  @param[out] pMemmap Initialized list head to which region items will be added

  @retval EFI_INVALID_PARAMETER Invalid set of parameters provided
  @retval EFI_OUT_OF_RESOURCES Not enough free space on target
  @retval EFI_SUCCESS List correctly retrieved
**/
EFI_STATUS
GetDimmMemmap(
  IN     DIMM *pDimm,
     OUT LIST_ENTRY *pMemmap
  )
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  UINT32 Index = 0;
  UINT64 Offset = 0;
  DIMM_REGION *pDimmRegion = NULL;
  NAMESPACE *pNamespace = NULL;
  LIST_ENTRY *pNode = NULL;
  LIST_ENTRY *pNode2 = NULL;
  struct _NVM_IS *pIS = NULL;
  UINT64 Length = 0;
  UINT32 RegionCount = 0;
  BOOLEAN ISetInterleaved = FALSE;
  UINT32 Type = 0;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pMemmap == NULL) {
    goto Finish;
  }

  /**
    Volatile Partition might not start at DPA 0.
    For safety let's treat area starting at DPA 0 as Reserved
  **/
  if (pDimm->VolatileStart > 0) {
    AddMemmapRange(pMemmap, pDimm, 0, pDimm->VolatileStart, MEMMAP_RANGE_RESERVED);
  }

  /** Volatile Partition **/
  if (pDimm->VolatileCapacity > 0) {
    AddMemmapRange(pMemmap, pDimm, pDimm->VolatileStart, pDimm->VolatileCapacity, MEMMAP_RANGE_VOLATILE);
  }

  /** Persistent Partition **/
  if (pDimm->PmCapacity > 0) {
    AddMemmapRange(pMemmap, pDimm, pDimm->PmStart, pDimm->PmCapacity, MEMMAP_RANGE_PERSISTENT);
  }

  /** At the end of Dimm may be reserved area **/
  Offset = pDimm->VolatileStart + pDimm->VolatileCapacity + pDimm->PmCapacity;
  Length = pDimm->RawCapacity - Offset;
  if (Length > 0) {
    AddMemmapRange(pMemmap, pDimm, Offset, Length, MEMMAP_RANGE_RESERVED);
  }

  /** Interleave Sets **/
  LIST_FOR_EACH(pNode, &gNvmDimmData->PMEMDev.ISs) {
    pIS = IS_FROM_NODE(pNode);

    ReturnCode = GetListSize(&pIS->DimmRegionList, &RegionCount);
    if (EFI_ERROR(ReturnCode) || RegionCount == 0) {
      goto Finish;
    }

    ISetInterleaved = RegionCount > 1;

    LIST_FOR_EACH(pNode2, &pIS->DimmRegionList) {
      pDimmRegion = DIMM_REGION_FROM_NODE(pNode2);
      if (pDimmRegion->pDimm != pDimm) {
        continue;
      }
      Offset = pDimm->PmStart + pDimmRegion->PartitionOffset;

      if (pIS->MirrorEnable) {
        Type = MEMMAP_RANGE_IS_MIRROR;
      } else if (ISetInterleaved) {
        Type = MEMMAP_RANGE_IS;
      } else {
        Type = MEMMAP_RANGE_IS_NOT_INTERLEAVED;
      }

      AddMemmapRange(pMemmap, pDimm, Offset, pDimmRegion->PartitionSize, Type);
    }
  }

  /** Namespaces **/
  LIST_FOR_EACH(pNode, &gNvmDimmData->PMEMDev.Namespaces) {
    pNamespace = NAMESPACE_FROM_NODE(pNode, NamespaceNode);

    for (Index = 0; Index < pNamespace->RangesCount; Index++) {
      if (pNamespace->Range[Index].pDimm != pDimm) {
        continue;
      }
      AddMemmapRange(pMemmap, pDimm,
          pNamespace->Range[Index].Dpa,
          pNamespace->Range[Index].Size,
          MEMMAP_RANGE_APPDIRECT_NAMESPACE);
    }
  }

  // Set last usable DPA to last PM partition address
  Offset = pDimm->PmStart + pDimm->PmCapacity;
  AddMemmapRange(pMemmap, pDimm, Offset, 0, MEMMAP_RANGE_LAST_USABLE_DPA);

  ReturnCode = EFI_SUCCESS;
#ifdef MDEPKG_NDEBUG
  PrintDimmMemmap(pMemmap);
#endif

Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Retrieve list of free regions of a DIMM based on capacity type

  Free regions will be delivered in a form of sorted linked list with
  items containing start DPA and length of free ranges and they don't overlap each other

  @param[in] pDimm Target DIMM structure pointer
  @param[in] FreeCapacityTypeArg Determine a type of free capacity
  @param[out] pFreemap Initialized list head to which region items will be added

  @retval EFI_INVALID_PARAMETER Invalid set of parameters provided
  @retval EFI_OUT_OF_RESOURCES Not enough free space on target
  @retval EFI_SUCCESS List correctly retrieved
**/
EFI_STATUS
GetDimmFreemap(
  IN     DIMM *pDimm,
  IN     FreeCapacityType FreeCapacityTypeArg,
     OUT LIST_ENTRY *pFreemap
  )
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  MEMMAP_RANGE *pMemmapRange = NULL;
  LIST_ENTRY *pMemmapList = NULL;
  LIST_ENTRY *pUsableRanges = NULL;
  LIST_ENTRY *pOccupiedRanges = NULL;
  LIST_ENTRY *pNode = NULL;

  NVDIMM_ENTRY();
  if (pDimm == NULL || pFreemap == NULL) {
    goto Finish;
  }

  pMemmapList = (LIST_ENTRY *) AllocateZeroPool(sizeof(*pMemmapList));
  if (pMemmapList == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }
  InitializeListHead(pMemmapList);

  pUsableRanges = (LIST_ENTRY *) AllocateZeroPool(sizeof(*pUsableRanges));
  if (pUsableRanges == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }
  InitializeListHead(pUsableRanges);

  pOccupiedRanges = (LIST_ENTRY *) AllocateZeroPool(sizeof(*pOccupiedRanges));
  if (pOccupiedRanges == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }
  InitializeListHead(pOccupiedRanges);

  ReturnCode = GetDimmMemmap(pDimm, pMemmapList);
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }

  LIST_FOR_EACH(pNode, pMemmapList) {
    pMemmapRange = MEMMAP_RANGE_FROM_NODE(pNode);

    /**
      Make list of ranges that can be used for specified mode. For example AppDirect Namespaces can be created only on
      Interleave Sets.

      Ranges may overlap and they will be sorted by DPA start address.
    **/
    if (pMemmapRange->RangeType == MEMMAP_RANGE_PERSISTENT) {
      if (FreeCapacityTypeArg == FreeCapacityForPersistentRegion) {
        AddMemmapRange(pUsableRanges, pMemmapRange->pDimm, pMemmapRange->RangeStartDpa, pMemmapRange->RangeLength,
            pMemmapRange->RangeType);
      }
    } else if (pMemmapRange->RangeType == MEMMAP_RANGE_IS_MIRROR) {
      if (FreeCapacityTypeArg == FreeCapacityForMirrorRegion ||
          FreeCapacityTypeArg == FreeCapacityForADMode) {
        AddMemmapRange(pUsableRanges, pMemmapRange->pDimm, pMemmapRange->RangeStartDpa, pMemmapRange->RangeLength,
            pMemmapRange->RangeType);
      }
    } else if (pMemmapRange->RangeType == MEMMAP_RANGE_IS ||
               pMemmapRange->RangeType == MEMMAP_RANGE_IS_NOT_INTERLEAVED) {
      if (FreeCapacityTypeArg == FreeCapacityForADMode) {
        AddMemmapRange(pUsableRanges, pMemmapRange->pDimm, pMemmapRange->RangeStartDpa, pMemmapRange->RangeLength,
            pMemmapRange->RangeType);
      }
    }

    /** Make list of used ranges for specified mode. Ranges may overlap and they will be sorted by DPA start address. **/
    if (pMemmapRange->RangeType == MEMMAP_RANGE_APPDIRECT_NAMESPACE) {
      AddMemmapRange(pOccupiedRanges, pMemmapRange->pDimm, pMemmapRange->RangeStartDpa, pMemmapRange->RangeLength,
          pMemmapRange->RangeType);
    } else if (pMemmapRange->RangeType == MEMMAP_RANGE_IS_MIRROR) {
      if (FreeCapacityTypeArg == FreeCapacityForPersistentRegion) {
        AddMemmapRange(pOccupiedRanges, pMemmapRange->pDimm, pMemmapRange->RangeStartDpa, pMemmapRange->RangeLength,
            pMemmapRange->RangeType);
      }
    }
  }
  /** Get non-overlapped free ranges **/
  ReturnCode = FindFreeRanges(pUsableRanges, pOccupiedRanges, pFreemap);
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }

  ReturnCode = EFI_SUCCESS;

Finish:
  if (pOccupiedRanges != NULL) {
    FreeMemmapItems(pOccupiedRanges);
    FREE_POOL_SAFE(pOccupiedRanges);
  }
  if (pUsableRanges != NULL) {
    FreeMemmapItems(pUsableRanges);
    FREE_POOL_SAFE(pUsableRanges);
  }
  if (pMemmapList != NULL) {
    FreeMemmapItems(pMemmapList);
    FREE_POOL_SAFE(pMemmapList);
  }
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Free resources of memmap list items

  @param[in, out] pMemmapList Memmap list that items will be freed for
**/
VOID
FreeMemmapItems(
  IN OUT LIST_ENTRY *pMemmapList
  )
{
  MEMMAP_RANGE *pMemmapRange = NULL;
  LIST_ENTRY *pNode = NULL;
  LIST_ENTRY *pNext = NULL;

  NVDIMM_ENTRY();

  if (pMemmapList == NULL) {
    goto Finish;
  }

  LIST_FOR_EACH_SAFE(pNode, pNext, pMemmapList) {
    pMemmapRange = MEMMAP_RANGE_FROM_NODE(pNode);
    RemoveEntryList(pNode);
    FREE_POOL_SAFE(pMemmapRange);
  }

Finish:
  NVDIMM_EXIT();
}

/**
  Merge overlapped ranges

  Memmap ranges may overlap each other. This function merges overlapped ranges to continuous ranges.
  Input list has to be sorted by DPA start address. Returned list will be sorted as well.

  The caller is responsible for a memory deallocation of the returned list.

  @param[in] pMemmapList  Initialized list of ranges to merge.
  @param[out] pMergedList Initialized, output list to fill with continuous ranges.

  @retval EFI_SUCCESS           Success
  @retval EFI_INVALID_PARAMETER One or more parameters are NULL
  @retval EFI_OUT_OF_RESOURCES  Memory allocation failure
**/
EFI_STATUS
MergeMemmapItems(
  IN     LIST_ENTRY *pMemmapList,
     OUT LIST_ENTRY *pMergedList
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  LIST_ENTRY *pNode = NULL;
  MEMMAP_RANGE *pMemmapRange = NULL;
  UINT32 Index = 0;
  DIMM *pDimm = NULL;
  UINT64 RangeStartDpa = 0;
  UINT64 RangeEndDpa = 0;
  UINT64 RangeLength = 0;

  NVDIMM_ENTRY();

  if (pMemmapList == NULL || pMergedList == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  if (!IsListEmpty(pMemmapList)) {
    Index = 0;
    LIST_FOR_EACH(pNode, pMemmapList) {
      pMemmapRange = MEMMAP_RANGE_FROM_NODE(pNode);

      if (Index == 0) {
        pDimm = pMemmapRange->pDimm;
        RangeStartDpa = pMemmapRange->RangeStartDpa;
        RangeLength = pMemmapRange->RangeLength;
        /**
          The End DPA will always be 1 less than the value obtained by
          adding the Range-Length to the Start DPA.
        **/
        RangeEndDpa = pMemmapRange->RangeStartDpa + pMemmapRange->RangeLength - 1;
      } else if (pMemmapRange->RangeStartDpa <= RangeEndDpa) {
        /** Merging ranges **/
        if ((pMemmapRange->RangeStartDpa + pMemmapRange->RangeLength - 1) > RangeEndDpa) {
          RangeEndDpa = pMemmapRange->RangeStartDpa + pMemmapRange->RangeLength - 1;
          RangeLength = RangeEndDpa - RangeStartDpa + 1;
        }
      } else {
        /** Separate, non-overlapped range **/
        AddMemmapRange(pMergedList, pDimm, RangeStartDpa, RangeLength, MEMMAP_RANGE_UNDEFINED);
        RangeStartDpa = pMemmapRange->RangeStartDpa;
        RangeLength = pMemmapRange->RangeLength;
        RangeEndDpa = pMemmapRange->RangeStartDpa + pMemmapRange->RangeLength - 1;
      }

      Index++;
    }

    AddMemmapRange(pMergedList, pDimm, RangeStartDpa, RangeLength, MEMMAP_RANGE_UNDEFINED);
  }

Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Find free ranges

  Take list of usable ranges and subtract occupied ranges. The result will be list of free ranges.
  Input lists have to be sorted by DPA start address. Returned list will be sorted as well.

  The caller is responsible for a memory deallocation of the returned list.

  @param[in] pUsableRangesList    Initialized list of usable ranges.
  @param[in] pOccupiedRangesList  Initialized list of occupied ranges to subtract.
  @param[out] pFreeRangesList     Initialized, output list to fill with free ranges.

  @retval EFI_SUCCESS           Success
  @retval EFI_INVALID_PARAMETER One or more parameters are NULL
  @retval EFI_OUT_OF_RESOURCES  Memory allocation failure
**/
EFI_STATUS
FindFreeRanges(
  IN     LIST_ENTRY *pUsableRangesList,
  IN     LIST_ENTRY *pOccupiedRangesList,
     OUT LIST_ENTRY *pFreeRangesList
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  LIST_ENTRY *pUsableRangesListMerged = NULL;
  LIST_ENTRY *pOccupiedRangesListMerged = NULL;
  LIST_ENTRY *pNodeUsableRange = NULL;
  LIST_ENTRY *pNodeOccupiedRange = NULL;
  MEMMAP_RANGE *pUsableRange = NULL;
  MEMMAP_RANGE *pOccupiedRange = NULL;
  BOOLEAN UsableRangeDone = FALSE;
  DIMM *pDimm = NULL;
  UINT64 FreeRangeStartDpa = 0;
  UINT64 FreeRangeEndDpa = 0;
  UINT64 UsableRangeEndDpa = 0;
  UINT64 OccupiedRangeEndDpa = 0;

  NVDIMM_ENTRY();

  if (pUsableRangesList == NULL || pOccupiedRangesList == NULL || pFreeRangesList == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pUsableRangesListMerged = (LIST_ENTRY *) AllocateZeroPool(sizeof(*pUsableRangesListMerged));
  if (pUsableRangesListMerged == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }
  InitializeListHead(pUsableRangesListMerged);

  pOccupiedRangesListMerged = (LIST_ENTRY *) AllocateZeroPool(sizeof(*pOccupiedRangesListMerged));
  if (pOccupiedRangesListMerged == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }
  InitializeListHead(pOccupiedRangesListMerged);

  /** First, merge overlapped ranges **/
  ReturnCode = MergeMemmapItems(pUsableRangesList, pUsableRangesListMerged);
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }
  ReturnCode = MergeMemmapItems(pOccupiedRangesList, pOccupiedRangesListMerged);
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }

  /** Find free ranges **/
  LIST_FOR_EACH(pNodeUsableRange, pUsableRangesListMerged) {
    pUsableRange = MEMMAP_RANGE_FROM_NODE(pNodeUsableRange);
    UsableRangeEndDpa = pUsableRange->RangeStartDpa + pUsableRange->RangeLength;

    UsableRangeDone = FALSE;
    pDimm = pUsableRange->pDimm;
    /** If there is no occupied ranges, then whole usable range is free **/
    FreeRangeStartDpa = pUsableRange->RangeStartDpa;
    FreeRangeEndDpa = pUsableRange->RangeStartDpa + pUsableRange->RangeLength;

    /** Subtract occupied ranges from usable range **/
    LIST_FOR_EACH(pNodeOccupiedRange, pOccupiedRangesListMerged) {
      pOccupiedRange = MEMMAP_RANGE_FROM_NODE(pNodeOccupiedRange);
      OccupiedRangeEndDpa = pOccupiedRange->RangeStartDpa + pOccupiedRange->RangeLength;

      if (pOccupiedRange->RangeStartDpa <= FreeRangeStartDpa) {
        /** Occupied range starts before usable range **/
        if (OccupiedRangeEndDpa >= UsableRangeEndDpa) {
          /** Usable range is inside (or equal) occupied range, so there is no free range for this usable range **/
          UsableRangeDone = TRUE;
          break;
        } else if (OccupiedRangeEndDpa > FreeRangeStartDpa) {
          /** Start free range where the occupied range ends **/
          FreeRangeStartDpa = OccupiedRangeEndDpa;
        } else {
          /** Whole occupied range is before usable range, so they don't overlap **/
        }
      } else {
        /** Occupied range starts after usable range **/
        if (pOccupiedRange->RangeStartDpa > UsableRangeEndDpa) {
          /** Whole occupied range is after usable range, so free range ends where usable range ends **/
          FreeRangeEndDpa = UsableRangeEndDpa;
        } else {
          /** Free range ends where occupied range starts **/
          FreeRangeEndDpa = pOccupiedRange->RangeStartDpa;
        }

        /** Add found free range **/
        AddMemmapRange(pFreeRangesList, pDimm, FreeRangeStartDpa, FreeRangeEndDpa - FreeRangeStartDpa,
            MEMMAP_RANGE_FREE);

        if (pOccupiedRange->RangeStartDpa >= UsableRangeEndDpa || OccupiedRangeEndDpa >= UsableRangeEndDpa) {
          /**
            Whole occupied range is after usable range, so no need to check next occupied ranges, because the list is
            sorted.
          **/
          UsableRangeDone = TRUE;
          break;
        } else {
          /** Next free range starts where occupied range ends **/
          FreeRangeStartDpa = OccupiedRangeEndDpa;
        }
      }
    }

    if (!UsableRangeDone) {
      /** The last occupied range ends before usable range end **/
      FreeRangeEndDpa = UsableRangeEndDpa - FreeRangeStartDpa;
      AddMemmapRange(pFreeRangesList, pDimm, FreeRangeStartDpa, FreeRangeEndDpa, MEMMAP_RANGE_FREE);
    }
  }

Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Remove the entire dimm inventory
  Remove the entire dimm inventory safely
  Dimms that cannot be removed safely are left in inventory

  @param[in,out] pDev: The pmem super structure
**/
EFI_STATUS
RemoveDimmInventory(
    IN OUT PMEM_DEV *pDev
)
{
  DIMM *pCurDimm = NULL;
  LIST_ENTRY *pCurDimmNode = NULL;
  LIST_ENTRY *pTempDimmNode = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  EFI_STATUS TmpReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();
  for (pCurDimmNode = GetFirstNode(&pDev->Dimms);
      !IsNull(&pDev->Dimms, pCurDimmNode) && pCurDimmNode != NULL;
      pCurDimmNode = pTempDimmNode) {
    pTempDimmNode = GetNextNode(&pDev->Dimms, pCurDimmNode);
    pCurDimm = DIMM_FROM_NODE(pCurDimmNode);

    RemoveEntryList(pCurDimmNode);

    TmpReturnCode = RemoveDimm(pCurDimm, 0);
    if (EFI_ERROR(TmpReturnCode)) {
      NVDIMM_WARN("Unable to remove NVDIMM %#x Error: %d", (NULL != pCurDimm) ? pCurDimm->DeviceHandle.AsUint32 : 0, TmpReturnCode);
      ReturnCode = TmpReturnCode;
    }
  }

  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

VOID
InitializeDimmFieldsFromAcpiTables(
  IN     NvDimmRegionMappingStructure *pNvDimmRegionTbl,
  IN     ControlRegionTbl *pControlRegionTbl,
  IN     ParsedPmttHeader *pPmttHead,
     OUT DIMM *pDimm
  )
{
  PMTT_MODULE_INFO *pPmttModuleInfo = NULL;
  pDimm->Signature = DIMM_SIGNATURE;
  pDimm->Configured = FALSE;
  pDimm->ISsNum = 0;

  if (pNvDimmRegionTbl != NULL) {
    /**
      ACPI 6.3, if BIT 31 of NfitDeviceHandle is set initialize DIMM fields from PMTT
      Previous versions of ACPI use NFIT only considering BIT 31 is zero
    **/
    if (!(pNvDimmRegionTbl->DeviceHandle.AsUint32 & BIT31) || pPmttHead == NULL) {
      pDimm->SocketId = (UINT16)NFIT_NODE_SOCKET_TO_SOCKET_INDEX(pNvDimmRegionTbl->DeviceHandle.NfitDeviceHandle.NodeControllerId,
        pNvDimmRegionTbl->DeviceHandle.NfitDeviceHandle.SocketId);
      pDimm->DimmID = pNvDimmRegionTbl->NvDimmPhysicalId;
      pDimm->DeviceHandle.AsUint32 = pNvDimmRegionTbl->DeviceHandle.AsUint32;
      pDimm->ImcId = (UINT16)pNvDimmRegionTbl->DeviceHandle.NfitDeviceHandle.MemControllerId;
      pDimm->NodeControllerID = (UINT16)pNvDimmRegionTbl->DeviceHandle.NfitDeviceHandle.NodeControllerId;
      pDimm->ChannelId = (UINT16)pNvDimmRegionTbl->DeviceHandle.NfitDeviceHandle.MemChannel;
      pDimm->ChannelPos = (UINT16)pNvDimmRegionTbl->DeviceHandle.NfitDeviceHandle.DimmNumber;
      pDimm->NvDimmStateFlags = pNvDimmRegionTbl->NvDimmStateFlags;
    } else {
      if (pPmttHead != NULL) {
        if (!IS_ACPI_HEADER_REV_MAJ_0_MIN_2(pPmttHead->pPmtt)) {
          NVDIMM_DBG("Unexpected PMTT revision!");
          return;
        }
        pPmttModuleInfo = GetDimmModuleByPidFromPmtt(pNvDimmRegionTbl->NvDimmPhysicalId, pPmttHead);
        if (pPmttModuleInfo == NULL) {
          NVDIMM_DBG("DIMM Module not found in PMTT");
          return;
        }

        pDimm->SocketId = pPmttModuleInfo->CpuId;
        pDimm->DimmID = pPmttModuleInfo->SmbiosHandle;
        pDimm->DeviceHandle.AsUint32 = pNvDimmRegionTbl->DeviceHandle.AsUint32;
        pDimm->ImcId = pPmttModuleInfo->MemControllerId;
        pDimm->NodeControllerID = SOCKET_INDEX_TO_NFIT_NODE_ID(pPmttModuleInfo->SocketId);
        pDimm->ChannelId = pPmttModuleInfo->ChannelId;
        pDimm->ChannelPos = pPmttModuleInfo->SlotId;
        pDimm->NvDimmStateFlags = pPmttModuleInfo->Header.Flags;
      }
    }
  }

  if (pControlRegionTbl != NULL) {
    pDimm->VendorId = pControlRegionTbl->VendorId;
    pDimm->DeviceId = pControlRegionTbl->DeviceId;
    pDimm->Rid = pControlRegionTbl->Rid;
    pDimm->SubsystemVendorId = pControlRegionTbl->SubsystemVendorId;
    pDimm->SubsystemDeviceId = pControlRegionTbl->SubsystemDeviceId;
    pDimm->SubsystemRid = pControlRegionTbl->SubsystemRid;
    pDimm->ManufacturingInfoValid = pControlRegionTbl->ValidFields;
    pDimm->ManufacturingLocation = pControlRegionTbl->ManufacturingLocation;
    pDimm->ManufacturingDate = pControlRegionTbl->ManufacturingDate;
    pDimm->SerialNumber = pControlRegionTbl->SerialNumber;
    // Not using the rest of the control region fields
  }
}

/**
  Populate SMBUS fields in each DCPMM
  Note: Currently only needed for SPI flash recovery scenario in UEFI only

  @param[in] pNewDimm: input dimm structure to populate
  @retval EFI_SUCCESS Success
  @retval Other errors from called function
**/
EFI_STATUS
PopulateSmbusFields(
  IN     DIMM *pNewDimm
)
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  pNewDimm->SmbusAddress.Cpu = (UINT8)(pNewDimm->DeviceHandle.NfitDeviceHandle.SocketId);
  pNewDimm->SmbusAddress.Imc = (UINT8)(pNewDimm->DeviceHandle.NfitDeviceHandle.MemControllerId);
  pNewDimm->SmbusAddress.Slot =
      (UINT8)(pNewDimm->DeviceHandle.NfitDeviceHandle.MemChannel * MAX_DIMMS_PER_CHANNEL +
      pNewDimm->DeviceHandle.NfitDeviceHandle.DimmNumber);

  //fill in fields provided by SMbus.
  pNewDimm->Signature = DIMM_SIGNATURE;

  ReturnCode = EFI_SUCCESS;
  return ReturnCode;
}
/**
  Creates the DIMM inventory
  Using the Firmware Interface Table, create an in memory representation
  of each dimm. For each unique dimm call the initialization function
  unique to the type of DIMM. As each dimm is fully initialized add it to
  the in memory list of DIMMs

  @param[in,out] pDev: The pmem super structure

  @retval EFI_SUCCESS  Success
  @retval EFI_...      Other errors from subroutines
**/
EFI_STATUS
InitializeDimmInventory(
  IN OUT PMEM_DEV *pDev
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  ParsedFitHeader *pFitHead = NULL;
  ParsedPmttHeader *pPmttHead = NULL;
  NvDimmRegionMappingStructure **ppNvDimmRegionMappingStructures = NULL;
  DIMM *pNewDimm = NULL;
  UINT32 Index = 0;

  NVDIMM_ENTRY();
  if (pDev == NULL || pDev->pFitHead == NULL || pDev->pFitHead->ppNvDimmRegionMappingStructures == NULL) {
    NVDIMM_DBG("Improperly initialized data");
    return EFI_INVALID_PARAMETER;
  }
#ifndef OS_BUILD
  InitializeCpuCommands();
#endif
  pFitHead = pDev->pFitHead;
  pPmttHead = pDev->pPmttHead;
  ppNvDimmRegionMappingStructures = pFitHead->ppNvDimmRegionMappingStructures;

  // Iterate over Region Mapping Structures (can be several per NVDIMM)
  // because they provide the NVDIMM physical ID, which is assigned by BIOS
  // and unique per boot. Could also use NFIT device handle.
  // Not iterating over control region tables (one per NVDIMM) because it
  // doesn't have any unique information other than the UID, but that isn't
  // as useful and takes longer to calculate and compare.
  for (Index = 0; Index < pFitHead->NvDimmRegionMappingStructuresNum; Index++) {
    if (GetDimmByPid(ppNvDimmRegionMappingStructures[Index]->NvDimmPhysicalId, &pDev->Dimms)) {
      // The associated NVDIMM physical ID is already in the dimms list, skip it
      continue;
    }

    // Create a new dimm struct for every NVDIMM, functional or not
    CHECK_RESULT_MALLOC(pNewDimm,(DIMM *) AllocateZeroPool(sizeof(*pNewDimm)), Finish);

    // Assume dimm is functional
    pNewDimm->NonFunctional = FALSE;

    // Fill in smbus address details
    CHECK_RESULT_CONTINUE(PopulateSmbusFields(pNewDimm));

    // Insert into dimms list. We're only inserting a pointer so we can
    // continue editing the dimm struct
    InsertTailList(&pDev->Dimms, &pNewDimm->DimmNode);

    CHECK_RESULT(InitializeDimm(pNewDimm, pFitHead, pPmttHead,
        ppNvDimmRegionMappingStructures[Index]->NvDimmPhysicalId), ErrorInitializeDimm);

    continue;

ErrorInitializeDimm:
    // If a dimm fails to initialize for any reason, it is also non-functional
    // for right now
    pNewDimm->NonFunctional = TRUE;
  }

  ReturnCode = EFI_SUCCESS;
Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command Get Viral Policy
  Execute a FW command to check the security status of a DIMM

  @param[in] pDimm The DIMM to retrieve viral policy
  @param[out] pViralPolicyPayload buffer to retrieve DIMM FW response

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER Parameter supplied is invalid
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
  @retval Various errors from FW
**/
EFI_STATUS
FwCmdGetViralPolicy(
  IN     DIMM *pDimm,
  OUT PT_VIRAL_POLICY_PAYLOAD *pViralPolicyPayload
)
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pViralPolicyPayload == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));

  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetAdminFeatures;
  pFwCmd->SubOpcode = SubopViralPolicy;
  pFwCmd->OutputPayloadSize = sizeof(*pViralPolicyPayload);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);

  NVDIMM_DBG("FW CMD Status %d", pFwCmd->Status);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending PtGetViralPolicy command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }
  CopyMem_S(pViralPolicyPayload, sizeof(*pViralPolicyPayload), pFwCmd->OutPayload, sizeof(*pViralPolicyPayload));

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Payload is the same for set and get operation
**/
EFI_STATUS
FwCmdGetOptionalConfigurationDataPolicy(
  IN     DIMM *pDimm,
     OUT PT_OPTIONAL_DATA_POLICY_PAYLOAD *pOptionalDataPolicyPayload
  )
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pOptionalDataPolicyPayload == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetFeatures;
  pFwCmd->SubOpcode = SubopConfigDataPolicy;
  pFwCmd->OutputPayloadSize = sizeof(pOptionalDataPolicyPayload->Payload);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);

  NVDIMM_DBG("FW CMD Status %d", pFwCmd->Status);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending PtGetOptionalDataPolicy command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }
  CopyMem_S(pOptionalDataPolicyPayload->Payload.Data, sizeof(pOptionalDataPolicyPayload->Payload), pFwCmd->OutPayload, sizeof(pOptionalDataPolicyPayload->Payload));
  pOptionalDataPolicyPayload->FisMajor = pDimm->FwVer.FwApiMajor;
  pOptionalDataPolicyPayload->FisMinor = pDimm->FwVer.FwApiMinor;

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Payload is the same for set and get operation
**/
EFI_STATUS
FwCmdSetOptionalConfigurationDataPolicy(
  IN     DIMM *pDimm,
  IN     PT_OPTIONAL_DATA_POLICY_PAYLOAD *pOptionalDataPolicyPayload
  )
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pOptionalDataPolicyPayload == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));

  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtSetFeatures;
  pFwCmd->SubOpcode = SubopConfigDataPolicy;
  pFwCmd->InputPayloadSize = sizeof(pOptionalDataPolicyPayload->Payload);
  CopyMem_S(pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), pOptionalDataPolicyPayload->Payload.Data, pFwCmd->InputPayloadSize);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  NVDIMM_DBG("FW CMD Status %d", pFwCmd->Status);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending PtGetOptionalDataPolicy command (Dimm(%d), RC = " FORMAT_EFI_STATUS ", Status = %d)",
        pDimm->DeviceHandle.AsUint32 ,pFwCmd->Status ,ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command get security info
  Execute a FW command to check the security status of a DIMM

  @param[in] pDimm: The DIMM to retrieve security info on
  @param[out] pSecurityPayload: Area to place the security info returned from FW
  @param[in] DimmId: The SMBIOS table type 17 handle of the Intel NVM Dimm

  @retval EFI_SUCCESS: Success
  @retval EFI_OUT_OF_RESOURCES: memory allocation failure
  @retval Various errors from FW are still TBD
**/
EFI_STATUS
FwCmdGetSecurityInfo(
  IN     DIMM *pDimm,
     OUT PT_GET_SECURITY_PAYLOAD *pSecurityPayload
  )
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();
  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));

  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetSecInfo;
  pFwCmd->SubOpcode= SubopGetSecState;
  pFwCmd->OutputPayloadSize = sizeof(*pSecurityPayload);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending PtGetSecInfo command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  CopyMem_S(pSecurityPayload, sizeof(*pSecurityPayload), pFwCmd->OutPayload, sizeof(*pSecurityPayload));

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}
/**
  Is Firmware command get security Opt-In supported
  Get security opt-in command is supported for certain
  fw versions with certain opt-in codes

  @param[in] pDimm: The DIMM to send get security opt-in command
  @param[in] OptInCode: Opt-In Code that is requested status for

  @retval BOOLEAN: return if the command is supported for opt-in code

**/
BOOLEAN IsGetSecurityOptInSupported(
  IN     DIMM *pDimm,
  IN     UINT16 OptInCode)
{
  BOOLEAN FIS_GT_2_1 = FALSE;
  BOOLEAN FIS_GTE_2_3 = FALSE;
  FIS_GT_2_1 = (2 <= pDimm->FwVer.FwApiMajor && 1 < pDimm->FwVer.FwApiMinor);
  FIS_GTE_2_3 = (2 <= pDimm->FwVer.FwApiMajor && 3 <= pDimm->FwVer.FwApiMinor);

  //If Fis is not greater than 2.1 get security opt in is not supported
  if (!FIS_GT_2_1)
  {
    return FALSE;
  }
  //If Fis is 2.2 only s3 resume is supported
  if (FIS_GT_2_1 && !FIS_GTE_2_3 && OptInCode != NVM_S3_RESUME)
  {
    return FALSE;
  }

  return TRUE;
}
/**
  Firmware command get security Opt-In
  Execute a FW command to check the security Opt-In code of a DIMM

  @param[in] pDimm: The DIMM to retrieve security info on
  @param[in] OptInCode: Opt-In Code that is requested status for
  @param[out] pSecurityOptIn: Area to place the returned from FW

  @retval EFI_SUCCESS: Success
  @retval EFI_OUT_OF_RESOURCES: memory allocation failure
  @retval Various errors from FW are still TBD
**/
EFI_STATUS
FwCmdGetSecurityOptIn(
  IN     DIMM *pDimm,
  IN     UINT16 OptInCode,
  OUT PT_OUTPUT_PAYLOAD_GET_SECURITY_OPT_IN *pSecurityOptIn
)
{
  NVM_FW_CMD *pFwCmd = NULL;
  PT_INPUT_PAYLOAD_GET_SECURITY_OPT_IN InputPayload;
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVDIMM_ENTRY();

  if (pDimm == NULL || pSecurityOptIn == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  if(!IsGetSecurityOptInSupported(pDimm,OptInCode)) {
    ReturnCode = EFI_UNSUPPORTED;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));

  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  SetMem(&InputPayload, sizeof(InputPayload), 0x0);

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetSecInfo;
  pFwCmd->SubOpcode = SubOpGetSecOptIn;
  InputPayload.OptInCode = OptInCode;
  pFwCmd->InputPayloadSize = sizeof(InputPayload);
  pFwCmd->OutputPayloadSize = sizeof(*pSecurityOptIn);

  CopyMem_S(pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), &InputPayload, pFwCmd->InputPayloadSize);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending PtGetSecOptIn command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  CopyMem_S(pSecurityOptIn, sizeof(*pSecurityOptIn), pFwCmd->OutPayload, sizeof(*pSecurityOptIn));
  if (pSecurityOptIn->OptInCode != OptInCode)
  {
    NVDIMM_DBG("Error detected when sending PtGetSecOptIn command (Requested OptInCode = %d , Received OptInCode = %d)",
      OptInCode, pSecurityOptIn->OptInCode);
    ReturnCode = EFI_NOT_FOUND;
  }

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to disable ARS

  @param[in] pDimm Pointer to the DIMM to disable ARS on

  @retval EFI_SUCCESS           Success
  @retval EFI_INVALID_PARAMETER One or more parameters are NULL
  @retval EFI_OUT_OF_RESOURCES  Memory allocation failure
  @retval Various errors from FW
**/
EFI_STATUS
FwCmdDisableARS(
  IN     DIMM *pDimm
)
{
  NVM_FW_CMD *pFwCmd = NULL;
  PT_PAYLOAD_SET_ADDRESS_RANGE_SCRUB *pARSInpugPayload = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  if (pDimm == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtSetFeatures;
  pFwCmd->SubOpcode = SubopAddressRangeScrub;

  pARSInpugPayload = (PT_PAYLOAD_SET_ADDRESS_RANGE_SCRUB*)pFwCmd->InputPayload;
  pARSInpugPayload->Enable = 0;

  pFwCmd->InputPayloadSize = sizeof(PT_PAYLOAD_SET_ADDRESS_RANGE_SCRUB);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending Firmware Set AddressRangeScrub command (RC = " FORMAT_EFI_STATUS ", Status = %d)", ReturnCode, pFwCmd->Status);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  NVDIMM_DBG("Polling ARS long op status to verify ARS disabled completed.");
  ReturnCode = PollOnArsDeviceBusy(pDimm, DISABLE_ARS_TOTAL_TIMEOUT_SEC);
  NVDIMM_DBG("Finished polling long op, return val = %x", ReturnCode);

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to retrieve the ARS status of a particular DIMM.

  @param[in] pDimm Pointer to the DIMM to retrieve ARSStatus on
  @param[out] pDimmARSStatus Pointer to the individual DIMM ARS status

  @retval EFI_SUCCESS           Success
  @retval EFI_INVALID_PARAMETER One or more parameters are NULL
  @retval EFI_OUT_OF_RESOURCES  Memory allocation failure
  @retval Various errors from FW
**/
EFI_STATUS
FwCmdGetARS(
  IN     DIMM *pDimm,
     OUT UINT8 *pDimmARSStatus
  )
{
  NVM_FW_CMD *pFwCmd = NULL;
  PT_PAYLOAD_ADDRESS_RANGE_SCRUB *pARSPayload = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pDimmARSStatus == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  *pDimmARSStatus = ARS_STATUS_UNKNOWN;
  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetFeatures;
  pFwCmd->SubOpcode = SubopAddressRangeScrub;
  pFwCmd->OutputPayloadSize = sizeof(*pARSPayload);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending Firmware Get AddressRangeScrub command (RC = " FORMAT_EFI_STATUS ", Status = %d)", ReturnCode, pFwCmd->Status);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }
  pARSPayload = (PT_PAYLOAD_ADDRESS_RANGE_SCRUB *) pFwCmd->OutPayload;

  ReturnCode = GetDimmARSStatusFromARSPayload(pARSPayload, pDimmARSStatus);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when retrieving ARSStatus from ARS Payload");
    goto Finish;
  }

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  This helper function is used to determine the ARS status for the
  particular DIMM by inspecting the firmware ARS return payload.

  @param[in] pARSPayload Pointer to the ARS return payload
  @param[out] pDimmARSStatus Pointer to the individual DIMM ARS status

  @retval EFI_SUCCESS           Success
  @retval EFI_INVALID_PARAMETER One or more parameters are NULL
**/
EFI_STATUS
GetDimmARSStatusFromARSPayload(
  IN     PT_PAYLOAD_ADDRESS_RANGE_SCRUB *pARSPayload,
     OUT UINT8 *pDimmARSStatus
)
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  if (pARSPayload == NULL || pDimmARSStatus == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  *pDimmARSStatus = ARS_STATUS_UNKNOWN;

  if ((pARSPayload->DPACurrentAddress == pARSPayload->DPAEndAddress) && !(pARSPayload->Enable)) {
    *pDimmARSStatus = ARS_STATUS_COMPLETED;
  }  else if ((pARSPayload->DPACurrentAddress > pARSPayload->DPAStartAddress) &&
             (pARSPayload->DPACurrentAddress < pARSPayload->DPAEndAddress) &&
             !(pARSPayload->Enable)) {
    *pDimmARSStatus = ARS_STATUS_ABORTED;
  } else if ((pARSPayload->DPACurrentAddress == 0x00) || (pARSPayload->DPACurrentAddress == pARSPayload->DPAStartAddress)) {
    *pDimmARSStatus = ARS_STATUS_NOT_STARTED;
  } else if ((pARSPayload->DPACurrentAddress > pARSPayload->DPAStartAddress) && (pARSPayload->Enable)) {
    *pDimmARSStatus = ARS_STATUS_IN_PROGRESS;
  } else {
    *pDimmARSStatus = ARS_STATUS_UNKNOWN;
  }

Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command Identify DIMM.
  Execute a FW command to get information about DIMM.

  @param[in] pDimm The Intel NVM Dimm to retrieve identify info on
  @param[out] pPayload Area to place the identity info returned from FW

  @retval EFI_SUCCESS: Success
  @retval EFI_OUT_OF_RESOURCES: memory allocation failure
**/
EFI_STATUS
FwCmdIdDimm (
  IN     DIMM *pDimm,
     OUT PT_ID_DIMM_PAYLOAD *pPayload
  )
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pPayload == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));

  if (!pFwCmd) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtIdentifyDimm;
  pFwCmd->SubOpcode = SubopIdentify;
  pFwCmd->OutputPayloadSize = OUT_PAYLOAD_SIZE;

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);

  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending PtIdentifyDimm command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }
  CopyMem_S(pPayload, sizeof(*pPayload), pFwCmd->OutPayload, sizeof(*pPayload));

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);

  return ReturnCode;
}

/**
  Firmware command Device Characteristics

  @param[in] pDimm The Intel NVM Dimm to retrieve device characteristics info for
  @param[out] ppPayload Area to place returned info from FW
    The caller is responsible to free the allocated memory with the FreePool function.

  @retval EFI_SUCCESS            Success
  @retval EFI_INVALID_PARAMETER  One or more input parameters are NULL
  @retval EFI_OUT_OF_RESOURCES   Memory allocation failure
**/
EFI_STATUS
FwCmdDeviceCharacteristics (
  IN     DIMM *pDimm,
     OUT PT_DEVICE_CHARACTERISTICS_OUT **ppPayload
  )
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  NVM_FW_CMD *pFwCmd = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || ppPayload == NULL) {
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  *ppPayload = AllocateZeroPool(sizeof(**ppPayload));
  if (*ppPayload == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtIdentifyDimm;
  pFwCmd->SubOpcode = SubopDeviceCharacteristics;
  pFwCmd->OutputPayloadSize = sizeof((*ppPayload)->Payload);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending Device Characteristics command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }
  CopyMem_S((*ppPayload)->Payload.Data, sizeof((*ppPayload)->Payload), pFwCmd->OutPayload, sizeof((*ppPayload)->Payload));
  (*ppPayload)->FisMajor = pDimm->FwVer.FwApiMajor;
  (*ppPayload)->FisMinor = pDimm->FwVer.FwApiMinor;

  ReturnCode = EFI_SUCCESS;

Finish:
  if (EFI_ERROR(ReturnCode) && (NULL != ppPayload)) {
    FREE_POOL_SAFE(*ppPayload);
  }
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Execute Firmware command to Get DIMM Partition Info

  @param[in]  pDimm     The DIMM to retrieve security info on
  @param[out] pPayload  Area to place the info returned from FW

  @retval EFI_INVALID_PARAMETER NULL pointer for DIMM structure provided
  @retval EFI_OUT_OF_RESOURCES  Memory allocation failure
  @retval EFI_...               Other errors from subroutines
  @retval EFI_SUCCESS           Success
**/
EFI_STATUS
FwCmdGetDimmPartitionInfo(
  IN     DIMM *pDimm,
     OUT PT_DIMM_PARTITION_INFO_PAYLOAD *pPayload
  )
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pPayload == NULL) {
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetAdminFeatures;
  pFwCmd->SubOpcode = SubopDimmPartitionInfo;
  pFwCmd->OutputPayloadSize = OUT_PAYLOAD_SIZE;

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending GetAdminFeatures command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    NVDIMM_DBG("FW CMD Status %d", pFwCmd->Status);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }
  CopyMem_S(pPayload, sizeof(*pPayload), pFwCmd->OutPayload, sizeof(*pPayload));

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command access/read Platform Config Data using small payload only.

  The function allows to specify the requested data offset and the size.
  The function is going to allocate the ppRawData buffer if it is not allocated.
  The buffer's minimal size is the size of the Partition!

  @param[in] pDimm The Intel NVM Dimm to retrieve identity info on
  @param[in] PartitionId Partition number to get data from
  @param[in] ReqOffset Data read starting point
  @param[in] ReqDataSize Number of bytes to read
  @param[out] Pointer to the buffer pointer for storing retrieved data

  @retval EFI_SUCCESS: Success, otherwise: Error
**/
EFI_STATUS
FwGetPCDFromOffsetSmallPayload(
  IN  DIMM *pDimm,
  IN  UINT8 PartitionId,
  IN  UINT32 ReqOffset,
  IN  UINT32 ReqDataSize,
  OUT UINT8 **ppRawData)
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;
  PT_INPUT_PAYLOAD_GET_PLATFORM_CONFIG_DATA InputPayload;
  UINT32 StartingPageOffset = ((ReqOffset / PCD_GET_SMALL_PAYLOAD_DATA_SIZE)*PCD_GET_SMALL_PAYLOAD_DATA_SIZE);
  UINT32 ReadOffset = 0;
  UINT32 PcdSize = 0;

  SetMem(&InputPayload, sizeof(InputPayload), 0x0);

  if (pDimm == NULL || ppRawData == NULL || 0 == ReqDataSize) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  if (PartitionId == PCD_OEM_PARTITION_ID) {
    PcdSize = pDimm->PcdOemPartitionSize;
  }
  else if (PartitionId == PCD_LSA_PARTITION_ID) {
    PcdSize = pDimm->PcdLsaPartitionSize;
  }
  else {
    ReturnCode = EFI_UNSUPPORTED;
    goto Finish;
  }

  /*
  * PcdSize is 0 if Media is disabled.
  * PcdSize was retrieved at driver load time so it is possible that since load time there
  * was a fatal media error that this would not catch.
  */
  if (PcdSize == 0) {
    ReturnCode = FwCmdGetPlatformConfigDataSize(pDimm, PartitionId, &PcdSize);
    if (EFI_ERROR(ReturnCode) || PcdSize == 0) {
      NVDIMM_DBG("FW CMD Error: %d", ReturnCode);
      goto Finish;
    }
    else if (PartitionId == PCD_OEM_PARTITION_ID) {
      pDimm->PcdOemPartitionSize = PcdSize;
    }
    else if (PartitionId == PCD_LSA_PARTITION_ID) {
      pDimm->PcdLsaPartitionSize = PcdSize;
    }
  }

  if (PcdSize < (StartingPageOffset + ReqDataSize)) {
    return EFI_BUFFER_TOO_SMALL;
  }

  if (NULL == *ppRawData)
  {
    *ppRawData = AllocateZeroPool(PcdSize);
    if (*ppRawData == NULL) {
      NVDIMM_WARN("Can't allocate memory for Platform Config Data (%d bytes)", PcdSize);
      ReturnCode = EFI_OUT_OF_RESOURCES;
      goto Finish;
    }
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  /**
    Retrieve the PCD/LSA data
  **/
  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetAdminFeatures;
  pFwCmd->SubOpcode = SubopPlatformDataInfo;
  InputPayload.PartitionId = PartitionId;
  InputPayload.CmdOptions.RetrieveOption = PCD_CMD_OPT_PARTITION_DATA;
  pFwCmd->InputPayloadSize = sizeof(InputPayload);
  /** Get PCD by small payload in loop in 128 byte chunks **/
  pFwCmd->LargeOutputPayloadSize = 0;
  pFwCmd->OutputPayloadSize = PCD_GET_SMALL_PAYLOAD_DATA_SIZE;
  InputPayload.CmdOptions.PayloadType = PCD_CMD_OPT_SMALL_PAYLOAD;
  for (ReadOffset = StartingPageOffset; ReadOffset < (ReqOffset+ReqDataSize); ReadOffset += PCD_GET_SMALL_PAYLOAD_DATA_SIZE) {
    InputPayload.Offset = ReadOffset;
    CopyMem_S(pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), &InputPayload, pFwCmd->InputPayloadSize);
    ReturnCode = PassThru(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_DBG("Error detected when sending Platform Config Data (Get Data) command (Offset = %d, RC = " FORMAT_EFI_STATUS ")", ReadOffset, ReturnCode);
      FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
      goto Finish;
    }
    CopyMem_S(*ppRawData + ReadOffset, PcdSize - ReadOffset, pFwCmd->OutPayload, PCD_GET_SMALL_PAYLOAD_DATA_SIZE);
  }

Finish:
  FREE_POOL_SAFE(pFwCmd);
  return ReturnCode;
}
/**
  Firmware command to get Partition Data using large payload.
  Execute a FW command to get information about DIMM regions and REGIONs configuration.

  The caller is responsible for a memory deallocation of the ppPlatformConfigData

  @param[in] pDimm The Intel NVM Dimm to retrieve identity info on
  @param[in] PartitionId Partition number to get data from
  @param[out] ppRawData Pointer to a new buffer pointer for storing retrieved data

  @retval EFI_SUCCESS: Success
  @retval EFI_OUT_OF_RESOURCES: memory allocation failure
**/
EFI_STATUS
FwCmdGetPcdLargePayload(
  IN     DIMM *pDimm,
  IN     UINT8 PartitionId,
  OUT UINT8 **ppRawData
)
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;
  PT_INPUT_PAYLOAD_GET_PLATFORM_CONFIG_DATA InputPayload;
  NVDIMM_ENTRY();

  SetMem(&InputPayload, sizeof(InputPayload), 0x0);

  if (pDimm == NULL || ppRawData == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  *ppRawData = AllocateZeroPool(PCD_PARTITION_SIZE);
  if (*ppRawData == NULL) {
    NVDIMM_WARN("Can't allocate memory for Platform Config Data (%u bytes)", PCD_PARTITION_SIZE);
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  /**
    Retrieve the OEM PCD data
  **/
  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetAdminFeatures;
  pFwCmd->SubOpcode = SubopPlatformDataInfo;
  InputPayload.PartitionId = PartitionId;
  InputPayload.CmdOptions.RetrieveOption = PCD_CMD_OPT_PARTITION_DATA;
  pFwCmd->InputPayloadSize = sizeof(InputPayload);

  /** Get PCD by large payload in single call **/
  pFwCmd->LargeOutputPayloadSize = PCD_PARTITION_SIZE;
  InputPayload.Offset = 0;
  InputPayload.CmdOptions.PayloadType = PCD_CMD_OPT_LARGE_PAYLOAD;

  CopyMem_S(pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), &InputPayload, pFwCmd->InputPayloadSize);
#ifdef OS_BUILD
  ReturnCode = PassThru(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
#else
  ReturnCode = PassThruWithRetryOnFwAborted(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
#endif
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending Platform Config Data (Get Data) command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }
  CopyMem_S(*ppRawData, PCD_PARTITION_SIZE, pFwCmd->LargeOutputPayload, PCD_PARTITION_SIZE);

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}
/**
  Firmware command get Platform Config Data.
  Execute a FW command to get information about DIMM regions and REGIONs configuration.

  The caller is responsible for a memory deallocation of the ppPlatformConfigData

  @param[in] pDimm The Intel NVM Dimm to retrieve identity info on
  @param[in] PartitionId Partition number to get data from
  @param[out] ppRawData Pointer to a new buffer pointer for storing retrieved data

  @retval EFI_SUCCESS: Success
  @retval EFI_UNSUPPORTED: invalid partition specified (OEM Partition not supported).
  @retval EFI_OUT_OF_RESOURCES: memory allocation failure
**/
EFI_STATUS
FwCmdGetPlatformConfigData(
  IN     DIMM *pDimm,
  IN     UINT8 PartitionId,
  OUT UINT8 **ppRawData
)
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;
  PT_INPUT_PAYLOAD_GET_PLATFORM_CONFIG_DATA InputPayload;
  UINT8 *pBuffer = NULL;
  UINT32 Offset = 0;
  UINT32 PcdSize = 0;
  BOOLEAN LargePayloadAvailable = FALSE;

  NVDIMM_ENTRY();

  // Don't support using this function to retrieve PCD OEM Config data.
  // Use FwCmdGetPcdSmallPayload
  if (PartitionId == PCD_OEM_PARTITION_ID) {
    ReturnCode = EFI_UNSUPPORTED;
    goto Finish;
  }

  SetMem(&InputPayload, sizeof(InputPayload), 0x0);

  if (pDimm == NULL || ppRawData == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  if (PartitionId == PCD_LSA_PARTITION_ID) {
    PcdSize = pDimm->PcdLsaPartitionSize;
  } else {
    ReturnCode = EFI_UNSUPPORTED;
    goto Finish;
  }

  /*
  * PcdSize is 0 if Media is disabled.
  * PcdSize was retrieved at driver load time so it is possible that since load time there
  * was a fatal media error that this would not catch. We would then be returning cached data
  * from a media disabled DIMM instead of erroring out.
  * It could also be possbile that FW was busy during driver load time, so disable the cache.
  */
  if (PcdSize == 0) {
    gPCDCacheEnabled = 0;
    ReturnCode = FwCmdGetPlatformConfigDataSize(pDimm, PartitionId, &PcdSize);
    if (EFI_ERROR(ReturnCode) || PcdSize == 0) {
      NVDIMM_DBG("FW CMD Error: %d", ReturnCode);
      goto Finish;
    } else if (PartitionId == PCD_LSA_PARTITION_ID) {
      pDimm->PcdLsaPartitionSize = PcdSize;
    }
  }

  *ppRawData = AllocateZeroPool(PcdSize);
  if (*ppRawData == NULL) {
    NVDIMM_WARN("Can't allocate memory for Platform Config Data (%d bytes)", PcdSize);
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  if (gPCDCacheEnabled) {
    if (pDimm->pPcdLsa && PartitionId == PCD_LSA_PARTITION_ID) {
      CopyMem_S(*ppRawData, PcdSize, pDimm->pPcdLsa, PcdSize);
      goto Finish;
    }
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  /**
    Retrieve the PCD/LSA data
  **/
  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetAdminFeatures;
  pFwCmd->SubOpcode = SubopPlatformDataInfo;
  InputPayload.PartitionId = PartitionId;
  InputPayload.CmdOptions.RetrieveOption = PCD_CMD_OPT_PARTITION_DATA;
  pFwCmd->InputPayloadSize = sizeof(InputPayload);

  CHECK_RESULT(IsLargePayloadAvailable(pDimm, &LargePayloadAvailable), Finish);
  if (!LargePayloadAvailable) {
    pBuffer = AllocateZeroPool(PcdSize);
    if (pBuffer == NULL) {
      NVDIMM_ERR("Can't allocate memory for PCD partition buffer (%d bytes)", PcdSize);
      ReturnCode = EFI_OUT_OF_RESOURCES;
      goto Finish;
    }
    /** Get PCD by small payload in loop in 128 byte chunks **/
    pFwCmd->LargeOutputPayloadSize = 0;
    pFwCmd->OutputPayloadSize = PCD_GET_SMALL_PAYLOAD_DATA_SIZE;
    InputPayload.CmdOptions.PayloadType = PCD_CMD_OPT_SMALL_PAYLOAD;
    for (Offset = 0; Offset < PcdSize; Offset += PCD_GET_SMALL_PAYLOAD_DATA_SIZE) {
      InputPayload.Offset = Offset;
      CopyMem_S(pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), &InputPayload, pFwCmd->InputPayloadSize);
#ifdef OS_BUILD
      ReturnCode = PassThru(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
#else
      ReturnCode = PassThruWithRetryOnFwAborted(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
#endif
      if (EFI_ERROR(ReturnCode)) {
        NVDIMM_DBG("Error detected when sending Platform Config Data (Get Data) command (Offset = %d, RC = " FORMAT_EFI_STATUS ")", Offset, ReturnCode);
        FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
        goto Finish;
      }
      CopyMem_S(pBuffer + Offset, PcdSize - Offset, pFwCmd->OutPayload, PCD_GET_SMALL_PAYLOAD_DATA_SIZE);
    }
#ifdef OS_BUILD
    gPCDCacheEnabled = 1;
#endif
  } else {
    /** Get PCD by large payload in single call **/
    pFwCmd->LargeOutputPayloadSize = PcdSize;
    InputPayload.Offset = 0;
    InputPayload.CmdOptions.PayloadType = PCD_CMD_OPT_LARGE_PAYLOAD;
    if (pFwCmd->InputPayloadSize > IN_PAYLOAD_SIZE) {
      NVDIMM_DBG("The size of command parameters is greater than the size of the small payload.");
    }
    CopyMem_S(pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), &InputPayload, pFwCmd->InputPayloadSize);
#ifdef OS_BUILD
    ReturnCode = PassThru(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
#else
    ReturnCode = PassThruWithRetryOnFwAborted(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
#endif
    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_DBG("Error detected when sending Platform Config Data (Get Data) command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
      FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
      goto Finish;
    }
#ifdef OS_BUILD
    gPCDCacheEnabled = 1;
#endif
  }
  if (gPCDCacheEnabled) {
    VOID *pTempCache = NULL;
    UINTN pTempCacheSz = 0;

    if (PartitionId == PCD_LSA_PARTITION_ID) {
      pDimm->pPcdLsa = AllocateZeroPool(pDimm->PcdLsaPartitionSize);
      pTempCache = pDimm->pPcdLsa;
      pTempCacheSz = pDimm->PcdLsaPartitionSize;
    }

    if (!LargePayloadAvailable) {
      // Check for null first
      CHECK_NOT_TRUE((NULL != *ppRawData && NULL != pBuffer), Finish);
      CopyMem_S(*ppRawData, PcdSize, pBuffer, PcdSize);
      if (NULL != pTempCache) {
        CopyMem_S(pTempCache, pTempCacheSz, pBuffer, PcdSize);
      }
    } else {
      CopyMem_S(*ppRawData, PcdSize, pFwCmd->LargeOutputPayload, PcdSize);
      if (NULL != pTempCache) {
        CopyMem_S(pTempCache, pTempCacheSz, pFwCmd->LargeOutputPayload, PcdSize);
      }
    }
    goto Finish;
  }
  if (!LargePayloadAvailable) {
    CHECK_NOT_TRUE((NULL != *ppRawData && NULL != pBuffer), Finish);
    CopyMem_S(*ppRawData, PcdSize, pBuffer, PcdSize);
  } else {
    CopyMem_S(*ppRawData, PcdSize, pFwCmd->LargeOutputPayload, PcdSize);
  }
Finish:
  FREE_POOL_SAFE(pFwCmd);
  FREE_POOL_SAFE(pBuffer);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to get the PCD size

  @param[in] pDimm The target DIMM
  @param[in] PartitionId The partition ID of the PCD
  @param[out] pPcdSize Pointer to the PCD size

  @retval EFI_INVALID_PARAMETER Invalid parameter passed
  @retval EFI_OUT_OF_RESOURCES Could not allocate memory
  @retval EFI_SUCCESS Command successfully run
**/
EFI_STATUS
FwCmdGetPlatformConfigDataSize (
  IN     DIMM *pDimm,
  IN     UINT8 PartitionId,
     OUT UINT32 *pPcdSize
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;
  PT_INPUT_PAYLOAD_GET_PLATFORM_CONFIG_DATA InputPayload;
  PT_OUTPUT_PAYLOAD_GET_PLATFORM_CONFIG_DATA_SIZE OutputPcdSize;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pPcdSize == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  ZeroMem(&InputPayload, sizeof(InputPayload));
  ZeroMem(&OutputPcdSize, sizeof(OutputPcdSize));

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  /**
    Retrieve the PCD/LSA data
  **/
  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetAdminFeatures;
  pFwCmd->SubOpcode = SubopPlatformDataInfo;
  InputPayload.PartitionId = PartitionId;
  InputPayload.CmdOptions.RetrieveOption = PCD_CMD_OPT_PARTITION_SIZE;
  pFwCmd->InputPayloadSize = sizeof(InputPayload);

  pFwCmd->LargeOutputPayloadSize = 0;
  pFwCmd->OutputPayloadSize = PCD_GET_SMALL_PAYLOAD_DATA_SIZE;
  InputPayload.CmdOptions.PayloadType = PCD_CMD_OPT_SMALL_PAYLOAD;
  CopyMem_S(pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), &InputPayload, pFwCmd->InputPayloadSize);
  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending Platform Config Data (Get Data) command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }
  CopyMem_S(&OutputPcdSize, sizeof(OutputPcdSize), pFwCmd->OutPayload, PCD_GET_SMALL_PAYLOAD_DATA_SIZE);

  *pPcdSize = OutputPcdSize.Size;

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
Validate the PCD Oem Config Header.

@param[in]  pOemHeader    Pointer to NVDIMM Configuration Header

@retval EFI_INVALID_PARAMETER NULL pointer for DIMM structure provided
@retval EFI_VOLUME_CORRUPTED  Header is invalid. Signature or checksum failed.
@retval EFI_NO_MEDIA          Size of the header exceeds allowed capacity
@retval EFI_SUCCESS           Success - Valid config header
**/
EFI_STATUS ValidatePcdOemHeader(
  IN  NVDIMM_CONFIGURATION_HEADER *pOemHeader)
{
  if (NULL == pOemHeader) {
    return EFI_INVALID_PARAMETER;
  }

  // Check for corruption first
  if (pOemHeader->Header.Signature != NVDIMM_CONFIGURATION_HEADER_SIG) {
    NVDIMM_WARN("Incorrect signature of the DIMM Configuration Header table");
    return EFI_VOLUME_CORRUPTED;
  }
  if (pOemHeader->Header.Length > PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE) {
    NVDIMM_WARN("Length of PCD header is greater than PCD OEM partition size");
    return EFI_VOLUME_CORRUPTED;
  }
  if (!IsChecksumValid(pOemHeader, pOemHeader->Header.Length)) {
    NVDIMM_WARN("The DIMM Configuration table checksum is invalid.");
    return EFI_VOLUME_CORRUPTED;
  }

  // If there's no corruption, then everything that follows should only
  // be based on some incompatibility with BIOS

  // BIOS revision too old or too new
  if (IS_NVDIMM_CONFIGURATION_HEADER_REV_INVALID(pOemHeader)) {
    NVDIMM_WARN("Unsupported revision of the DIMM Configuration Header table");
    // This should be more descriptive (like EFI_INCOMPATIBLE_VERSION) but
    // unfortunately anything other than EFI_VOLUME_CORRUPTED prevents
    // ipmctl delete -pcd from working. Too much headache to fix right now.
    return EFI_VOLUME_CORRUPTED;
  }

  return EFI_SUCCESS;
}

/**
Determine if PCD Header is all zeros.

@param[in]  pOemHeader    Pointer to NVDIMM Configuration Header
@param[out] bIsZero       Pointer to boolean. True if Config Header is zero.

@retval EFI_INVALID_PARAMETER NULL pointer for DIMM structure provided
@retval EFI_SUCCESS           Success
**/
EFI_STATUS IsPcdOemHeaderZero(
  IN  NVDIMM_CONFIGURATION_HEADER *pOemHeader,
  OUT BOOLEAN *bIsZero)
{
  int i = 0;

  if (NULL == bIsZero || NULL == pOemHeader) {
    return EFI_INVALID_PARAMETER;
  }

  *bIsZero = TRUE;

  for (i = 0; i < sizeof(NVDIMM_CONFIGURATION_HEADER); i++) {
    if (((UINT8*)pOemHeader)[i] != 0) {
      *bIsZero = FALSE;
      break;
    }
  }

  return EFI_SUCCESS;
}

/**
Determine the total size of PCD Config Data area by finding the largest
offset any of the 3 data sets.

@param[in]  pOemHeader    Pointer to NVDIMM Configuration Header
@param[out] pOemDataSize  Size of the PCD Config Data

@retval EFI_INVALID_PARAMETER NULL pointer for DIMM structure provided
@retval EFI_SUCCESS           Success
**/
EFI_STATUS GetPcdOemDataSize(NVDIMM_CONFIGURATION_HEADER *pOemHeader, UINT32 *pOemDataSize)
{
  if (NULL == pOemHeader || NULL == pOemDataSize) {
    return EFI_INVALID_PARAMETER;
  }

  UINT32 MaxCur = pOemHeader->CurrentConfStartOffset + pOemHeader->CurrentConfDataSize;
  UINT32 MaxIn = pOemHeader->ConfInputStartOffset + pOemHeader->ConfInputDataSize;
  UINT32 MaxOut = pOemHeader->ConfOutputStartOffset + pOemHeader->ConfOutputDataSize;

  // At least return the size of the header...
  *pOemDataSize = MAX(sizeof(NVDIMM_CONFIGURATION_HEADER), MAX(MaxOut, MAX(MaxCur, MaxIn)));
  NVDIMM_DBG("GetPcdOemDataSize. MaxOemDataSize: %d.\n", *pOemDataSize);

  // Prevent any crazy large values...
  if (*pOemDataSize > PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE) {
    NVDIMM_DBG("GetPcdOemDataSize. MaxOemDataSize is unexpectedly LARGE: %d.\n", *pOemDataSize);
    return EFI_VOLUME_CORRUPTED;
  }

  return EFI_SUCCESS;
}

/**
Retrieve Pcd data using small payload method only. Data is retrieved in 128
byte chunks.

@param[in]  pDimm       The DIMM to retrieve security info on
@param[in]  PartitionId The partition ID of the PCD
@param[in]  Offset      Offset of data to be read from PCD region
@param[in,out] pData    Pointer to a buffer used to retrieve PCD data. Must be at least 128 bytes.
@param[in]  DataSize    Size of the pData buffer in bytes.

@retval EFI_INVALID_PARAMETER NULL pointer for DIMM structure provided
@retval EFI_OUT_OF_RESOURCES  Memory allocation failure
@retval EFI_...               Other errors from subroutines
@retval EFI_SUCCESS           Success
**/
EFI_STATUS
FwCmdGetPcdSmallPayload(
  IN     DIMM   *pDimm,
  IN     UINT8  PartitionId,
  IN     UINT32 Offset,
  IN OUT UINT8  *pData,
  IN     UINT8  DataSize
)
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  PT_INPUT_PAYLOAD_GET_PLATFORM_CONFIG_DATA *pInputPayload = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pData == NULL) {
    goto Finish;
  }

  // Don't let try to read outside PCD or buffer
  if (((Offset + PCD_GET_SMALL_PAYLOAD_DATA_SIZE) > PCD_PARTITION_SIZE) ||
    (DataSize > PCD_GET_SMALL_PAYLOAD_DATA_SIZE) ||
    (0 == DataSize)) {
    goto Finish;
  }


  /*
  * PcdSize is 0 if Media is disabled or FW is busy.
  * PcdSize was retrieved at driver load time so it is possible that since load time there
  * was a fatal media error that this would not catch. We would then be returning cached data
  * from a media disabled DIMM instead of erroring out.
  * It could also be possbile that FW was busy during driver load time, so disable the cache.
  */
  if (gPCDCacheEnabled && pDimm->PcdOemPartitionSize == 0) {
    gPCDCacheEnabled = 0;
  }
  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pInputPayload = (PT_INPUT_PAYLOAD_GET_PLATFORM_CONFIG_DATA*) pFwCmd->InputPayload;

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetAdminFeatures;
  pFwCmd->SubOpcode = SubopPlatformDataInfo;
  pFwCmd->InputPayloadSize = sizeof(*pInputPayload);
  pFwCmd->LargeOutputPayloadSize = 0;
  pFwCmd->OutputPayloadSize = PCD_GET_SMALL_PAYLOAD_DATA_SIZE;
  pInputPayload->PartitionId = PartitionId;
  pInputPayload->CmdOptions.RetrieveOption = PCD_CMD_OPT_PARTITION_DATA;
  pInputPayload->CmdOptions.PayloadType = PCD_CMD_OPT_SMALL_PAYLOAD;
  pInputPayload->Offset = Offset;

#ifdef OS_BUILD
  ReturnCode = PassThru(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
#else
  ReturnCode = PassThruWithRetryOnFwAborted(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
#endif
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending Platform Config Data (Get Data) command (Offset = %d, RC = " FORMAT_EFI_STATUS ")", Offset, ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  CopyMem_S(pData, DataSize, pFwCmd->OutPayload, DataSize);

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
Firmware command get Platform Config Data via small payload only.
For OEM Config Data, small payload via ASL is faster than large payload via SMM.
Execute a FW command to get information about DIMM regions and REGIONs configuration.

The caller is responsible for a memory deallocation of the ppRawData

@param[in] pDimm The Intel NVM Dimm to retrieve identity info on
@param[out] ppRawData Pointer to a new buffer pointer for storing retrieved data
@param[out] pRawDataSize Pointer to size of the data retrieved.

@retval EFI_SUCCESS: Success
@retval EFI_OUT_OF_RESOURCES: memory allocation failure
@retval EFI_NO_MEDIA: PCD Partition size reported as 0. Can't read data.
@retval EFI_NOT_FOUND: No valid PCD Config Data Header. Maybe all zero's.
@retval EFI_VOLUME_CORRUPTED: PCD Config Data header is invalid/corrupted
**/
EFI_STATUS
GetPcdOemConfigDataUsingSmallPayload(
  IN     DIMM *pDimm,
  OUT UINT8 **ppRawData,
  OUT UINT32 *pRawDataSize
)
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  UINT8 *pBuffer = NULL;
  UINT32 Offset = 0;
  UINT8 TmpBuf[PCD_GET_SMALL_PAYLOAD_DATA_SIZE];
  NVDIMM_ENTRY();

  if (pDimm == NULL || ppRawData == NULL || pRawDataSize == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

// Disable the cache when media is disabled or when the fw is busy
  if (gPCDCacheEnabled && pDimm->PcdOemPartitionSize == 0) {
    gPCDCacheEnabled = 0;
  }

  // Return the cached data
  if (gPCDCacheEnabled && pDimm->pPcdOem) {
    *ppRawData = AllocateZeroPool(pDimm->PcdOemSize);
    if (*ppRawData == NULL) {
      NVDIMM_WARN("Can't allocate memory for Platform Config Data (%d bytes)", pDimm->PcdOemSize);
      ReturnCode = EFI_OUT_OF_RESOURCES;
      goto Finish;
    }

    CopyMem_S(*ppRawData, pDimm->PcdOemSize, pDimm->pPcdOem, pDimm->PcdOemSize);
    *pRawDataSize = pDimm->PcdOemSize;
    goto Finish;
  }

  // Read first block which includes config header
  ReturnCode = FwCmdGetPcdSmallPayload(pDimm, PCD_OEM_PARTITION_ID, 0, TmpBuf, sizeof(TmpBuf));
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }

  // Validate the Header
  NVDIMM_CONFIGURATION_HEADER *pOemHeader = (NVDIMM_CONFIGURATION_HEADER*) TmpBuf;

  ReturnCode = ValidatePcdOemHeader(pOemHeader);
  if (EFI_ERROR(ReturnCode)) {
    BOOLEAN IsZero = TRUE;
    EFI_STATUS tmpRc = IsPcdOemHeaderZero(pOemHeader, &IsZero);
    if ((EFI_SUCCESS == tmpRc) && (TRUE == IsZero)) {
      ReturnCode = EFI_NOT_FOUND;
    }

    goto Finish;
  }

  // Get size of OEM Config Data
  UINT32 OemDataSize = 0;
  /*Instead of making one more Passthru call to get the PCD size, get it from the OemHeader*/
  ReturnCode = GetPcdOemDataSize(pOemHeader, &OemDataSize);
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }

  // Ensure buffer size is rounded up to next PCD_GET_SMALL_PAYLOAD_DATA_SIZE boundary
  UINT32 BufferSize = ((OemDataSize / PCD_GET_SMALL_PAYLOAD_DATA_SIZE) + 1) * PCD_GET_SMALL_PAYLOAD_DATA_SIZE;
  pDimm->PcdOemPartitionSize = OemDataSize;
  pBuffer = AllocateZeroPool(BufferSize);
  if (pBuffer == NULL) {
    NVDIMM_ERR("Can't allocate memory for PCD partition buffer (%d bytes)", BufferSize);
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  // Save the first 128 bytes already read
  CopyMem_S(pBuffer, BufferSize, TmpBuf, PCD_GET_SMALL_PAYLOAD_DATA_SIZE);

  /** Get PCD by small payload in loop in 128 byte chunks **/
  for (Offset = PCD_GET_SMALL_PAYLOAD_DATA_SIZE; Offset < OemDataSize; Offset += PCD_GET_SMALL_PAYLOAD_DATA_SIZE) {

    ReturnCode = FwCmdGetPcdSmallPayload(pDimm, PCD_OEM_PARTITION_ID, Offset, pBuffer + Offset, (UINT8)PCD_GET_SMALL_PAYLOAD_DATA_SIZE);
    if (EFI_ERROR(ReturnCode)) {
      goto Finish;
    }
  }


  if ( gPCDCacheEnabled && OemDataSize > 0) {
    VOID *pTempCache = NULL;

    // Save data cache info
    pDimm->PcdOemSize = OemDataSize;
    pDimm->pPcdOem = AllocateZeroPool(PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE);
    pTempCache = pDimm->pPcdOem;
    if ((NULL != pTempCache) && (OemDataSize <= PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE)) {
      CopyMem_S(pTempCache, PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE, pBuffer, OemDataSize);
    }
  }
  //Assign new data to the requester data pointer
  *ppRawData = pBuffer;
  *pRawDataSize = OemDataSize;

Finish:
  if (EFI_ERROR(ReturnCode)) {
    // If error, free the buffer
    FREE_POOL_SAFE(pBuffer);
    if(NULL != ppRawData)
      *ppRawData = NULL;
  }

  NVDIMM_EXIT_I64(ReturnCode);

  return ReturnCode;
}

/**
  Firmware command access/write Platform Config Data using small payload only.

  The function allows to specify the requested data offset and the size.
  The buffer's minimal size is the size of the Partition!
  The offset and the data size needs to be aligned to the SET_SMALL_PAYLOAD_DATA_SIZE
  which is 64 bytes.

  @param[in] pDimm The Intel NVM Dimm to send Platform Config Data to
  @param[in] PartitionId Partition number for data to be send to
  @param[in] pRawData Pointer to a data buffer that will be sent to the DIMM
  @param[in] ReqOffset Data write starting point
  @param[in] ReqDataSize Number of bytes to write

  @retval EFI_SUCCESS: Success, otherwise: Error
**/
EFI_STATUS
FwSetPCDFromOffsetSmallPayload(
  IN  DIMM *pDimm,
  IN  UINT8 PartitionId,
  IN  UINT8 *pRawData,
  IN  UINT32 ReqOffset,
  IN  UINT32 ReqDataSize)
{

  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;
  PT_INPUT_PAYLOAD_SET_DATA_PLATFORM_CONFIG_DATA InPayloadSetData;
  UINT32 StartingPageOffset = ((ReqOffset / PCD_SET_SMALL_PAYLOAD_DATA_SIZE)*PCD_SET_SMALL_PAYLOAD_DATA_SIZE);
  UINT32 WriteOffset = 0;

  SetMem(&InPayloadSetData, sizeof(InPayloadSetData), 0x0);

  if ((pDimm == NULL) || (pRawData == NULL) ||
    ((ReqOffset+ReqDataSize) > PCD_PARTITION_SIZE) ||
    (0 == ReqDataSize) ||
    (ReqOffset % PCD_SET_SMALL_PAYLOAD_DATA_SIZE) ||
    (ReqDataSize % PCD_SET_SMALL_PAYLOAD_DATA_SIZE)) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  if (PartitionId == PCD_OEM_PARTITION_ID) {
    // Using small payload transactions.
    // Only allow up to 64kb to protect upper 64kb for OEM data.
    if ((ReqOffset + ReqDataSize) > PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE) {
      ReturnCode = EFI_BUFFER_TOO_SMALL;
      goto Finish;
    }
    // If partition size is 0, then prevent write
    if (0 == pDimm->PcdOemPartitionSize) {
      ReturnCode = EFI_BAD_BUFFER_SIZE;
      goto Finish;
    }
  }
  else if (PartitionId == PCD_LSA_PARTITION_ID) {
    // If partition size is 0, then prevent write
    if (0 == pDimm->PcdLsaPartitionSize) {
      ReturnCode = EFI_BAD_BUFFER_SIZE;
      goto Finish;
    }
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  /**
    Set the Platform Config Data
  **/
  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtSetAdminFeatures;
  pFwCmd->SubOpcode = SubopPlatformDataInfo;
  InPayloadSetData.PartitionId = PartitionId;
  pFwCmd->InputPayloadSize = sizeof(InPayloadSetData);
  /** Set PCD by small payload in loop in 64 byte chunks **/
  InPayloadSetData.PayloadType = PCD_CMD_OPT_SMALL_PAYLOAD;
  pFwCmd->LargeInputPayloadSize = 0;
  for (WriteOffset = StartingPageOffset; WriteOffset < (ReqOffset+ReqDataSize); WriteOffset += PCD_SET_SMALL_PAYLOAD_DATA_SIZE) {
    InPayloadSetData.Offset = WriteOffset;
    CopyMem_S(InPayloadSetData.Data, sizeof(InPayloadSetData.Data), pRawData + (WriteOffset - StartingPageOffset), PCD_SET_SMALL_PAYLOAD_DATA_SIZE);
    CopyMem_S(pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), &InPayloadSetData, pFwCmd->InputPayloadSize);
    pFwCmd->OutputPayloadSize = 0;
    ReturnCode = PassThru(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_DBG("Error detected when sending Platform Config Data (Offset=%d ReturnCode=" FORMAT_EFI_STATUS ", FWStatus=%d)", WriteOffset, ReturnCode, pFwCmd->Status);
      FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
      goto Finish;
    }
  }

Finish:
  FREE_POOL_SAFE(pFwCmd);
  return ReturnCode;
}

/**
  Firmware command set Platform Config Data.
  Execute a FW command to send REGIONs configuration to the Platform Config Data.

  @param[in] pDimm The Intel NVM Dimm to send Platform Config Data to
  @param[in] PartitionId Partition number for data to be send to
  @param[in] pRawData Pointer to a data buffer that will be sent to the DIMM
  @param[in] RawDataSize Size of pRawData in bytes

  @retval EFI_SUCCESS: Success
  @retval EFI_OUT_OF_RESOURCES: memory allocation failure
**/
EFI_STATUS
FwCmdSetPlatformConfigData (
  IN     DIMM *pDimm,
  IN     UINT8 PartitionId,
  IN     UINT8 *pRawData,
  IN     UINT32 RawDataSize
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;
  PT_INPUT_PAYLOAD_SET_DATA_PLATFORM_CONFIG_DATA InPayloadSetData;
  UINT8 *pPartition = NULL;
  UINT32 Offset = 0;
  UINT32 PcdSize = 0;
  VOID *pTempCache = NULL;
  UINTN pTempCacheSz = 0;
  UINT8 *pOEMPartitionData = NULL;
  BOOLEAN LargePayloadAvailable = FALSE;

  NVDIMM_ENTRY();

  SetMem(&InPayloadSetData, sizeof(InPayloadSetData), 0x0);

  if ((pDimm == NULL) || (pRawData == NULL) ||
    (RawDataSize > PCD_PARTITION_SIZE) ||
    (0 == RawDataSize)) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  if (PartitionId == PCD_OEM_PARTITION_ID) {
    // Using small payload transactions.
    // Only allow up to 64kb to protect upper 64kb for OEM data.
    if (RawDataSize > PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE) {
      ReturnCode = EFI_INVALID_PARAMETER;
      goto Finish;
    }
    if (gPCDCacheEnabled) {
      if (NULL == pDimm->pPcdOem) {
        pDimm->pPcdOem = AllocateZeroPool(PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE);
      }
      pDimm->PcdOemSize = RawDataSize;
      pTempCache = pDimm->pPcdOem;
      pTempCacheSz = PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE;
    }
    // If partition size is 0, then prevent write
    if (0 == pDimm->PcdOemPartitionSize) {
      ReturnCode = EFI_INVALID_PARAMETER;
      goto Finish;
    }
    PcdSize = RawDataSize;
  } else if (PartitionId == PCD_LSA_PARTITION_ID) {
    if (gPCDCacheEnabled) {
      if (NULL == pDimm->pPcdLsa) {
        pDimm->pPcdLsa = AllocateZeroPool(pDimm->PcdLsaPartitionSize);
      }
      pTempCache = pDimm->pPcdLsa;
      pTempCacheSz = pDimm->PcdLsaPartitionSize;
    }
    PcdSize = pDimm->PcdLsaPartitionSize;
  }
  if (PcdSize == 0) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  if (RawDataSize > PcdSize) {
    NVDIMM_DBG("Partition's data is greater than the size of partition.");
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pPartition = AllocateZeroPool(PcdSize);
  if (pPartition == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  /** Copy the data to 128KB partition. If the data is smaller, the rest of partition will be empty (filled with 0) **/
  CopyMem_S(pPartition, PcdSize, pRawData, RawDataSize);

  /**
    Set the Platform Config Data
  **/
  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtSetAdminFeatures;
  pFwCmd->SubOpcode = SubopPlatformDataInfo;
  InPayloadSetData.PartitionId = PartitionId;
  pFwCmd->InputPayloadSize = sizeof(InPayloadSetData);

  if (pFwCmd->InputPayloadSize > IN_PAYLOAD_SIZE) {
    NVDIMM_DBG("Size of command parameters is greater than the size of the small payload.");
  }

  CHECK_RESULT(IsLargePayloadAvailable(pDimm, &LargePayloadAvailable), Finish);
  if (!LargePayloadAvailable) {
    /** Set PCD by small payload in loop in 64 byte chunks **/
    InPayloadSetData.PayloadType = PCD_CMD_OPT_SMALL_PAYLOAD;
    pFwCmd->LargeInputPayloadSize = 0;

    for (Offset = 0; Offset < PcdSize; Offset += PCD_SET_SMALL_PAYLOAD_DATA_SIZE) {
      InPayloadSetData.Offset = Offset;
      CopyMem_S(InPayloadSetData.Data, sizeof(InPayloadSetData.Data), pPartition + Offset, PCD_SET_SMALL_PAYLOAD_DATA_SIZE);
      CopyMem_S(pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), &InPayloadSetData, pFwCmd->InputPayloadSize);
      pFwCmd->OutputPayloadSize = 0;
      ReturnCode = PassThru(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
      if (EFI_ERROR(ReturnCode)) {
        NVDIMM_DBG("Error detected when sending Platform Config Data (Offset=%d ReturnCode=" FORMAT_EFI_STATUS ", FWStatus=%d)", Offset, ReturnCode, pFwCmd->Status);
        FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
        goto Finish;
      } else if (gPCDCacheEnabled){
        if (pTempCache) {
          CopyMem_S((INT8*)(pTempCache) + Offset, pTempCacheSz - Offset, InPayloadSetData.Data, PCD_SET_SMALL_PAYLOAD_DATA_SIZE);
        }
      }
    }
  } else {
    // If it is OEM_PARTITION_ID we need to read entire
    // partition, copy over OEM Data and write
    // back entire partition
    if (PartitionId == PCD_OEM_PARTITION_ID) {
      CHECK_RESULT(FwCmdGetPcdLargePayload(pDimm, PCD_OEM_PARTITION_ID, &pOEMPartitionData), Finish);
      CopyMem_S(pFwCmd->LargeInputPayload + PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE,
                 sizeof(pFwCmd->LargeInputPayload)- PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE,
                 pOEMPartitionData + PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE,
                 PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE);
      pFwCmd->LargeInputPayloadSize = PCD_PARTITION_SIZE;
    }
    else {
      pFwCmd->LargeInputPayloadSize = PcdSize;
    }
    /** Set PCD by large payload in single call **/
    InPayloadSetData.Offset = 0;
    InPayloadSetData.PayloadType = PCD_CMD_OPT_LARGE_PAYLOAD;
    CopyMem_S(pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), &InPayloadSetData, pFwCmd->InputPayloadSize);

    /** Save 128KB partition to Large Payload **/
    CopyMem_S(pFwCmd->LargeInputPayload, sizeof(pFwCmd->LargeInputPayload), pPartition, PcdSize);
#ifdef OS_BUILD
    ReturnCode = PassThru(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
#else
    ReturnCode = PassThruWithRetryOnFwAborted(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
#endif
    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_WARN("Error detected when sending Platform Config Data (ReturnCode=" FORMAT_EFI_STATUS ", FWStatus=%d)", ReturnCode, pFwCmd->Status);
      FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    } else if (gPCDCacheEnabled) {
      if (pTempCache) {
        CopyMem_S(pTempCache, pTempCacheSz, pPartition, PcdSize);
      }
    }
  }

Finish:
  FREE_POOL_SAFE(pPartition);
  FREE_POOL_SAFE(pFwCmd);
  FREE_POOL_SAFE(pOEMPartitionData);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to get Alarm Thresholds

  @param[in] pDimm The Intel NVM Dimm to retrieve Alarm Thresholds
  @param[out] ppPayloadAlarmThresholds Area to place the Alarm Thresholds data returned from FW.
    The caller is responsible to free the allocated memory with the FreePool function.

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER pDimm or ppPayloadAtarmThresholds is NULL
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
 **/
EFI_STATUS
FwCmdGetAlarmThresholds (
  IN     DIMM *pDimm,
     OUT PT_PAYLOAD_ALARM_THRESHOLDS **ppPayloadAlarmThresholds
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || ppPayloadAlarmThresholds == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetFeatures;
  pFwCmd->SubOpcode = SubopAlarmThresholds;
  pFwCmd->OutputPayloadSize = sizeof(PT_PAYLOAD_ALARM_THRESHOLDS);

  *ppPayloadAlarmThresholds = AllocateZeroPool(sizeof(**ppPayloadAlarmThresholds));
  if (*ppPayloadAlarmThresholds == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto FinishAfterFwCmdAlloc;
  }

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Error detected when sending AlarmThresholds command (RC = " FORMAT_EFI_STATUS ", Status = %d)", ReturnCode, pFwCmd->Status);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto FinishAfterPayloadAlloc;
  }
  CopyMem_S(*ppPayloadAlarmThresholds, sizeof(**ppPayloadAlarmThresholds), pFwCmd->OutPayload, sizeof(**ppPayloadAlarmThresholds));

FinishAfterPayloadAlloc:
  if (EFI_ERROR(ReturnCode)){
    FREE_POOL_SAFE(*ppPayloadAlarmThresholds);
  }
FinishAfterFwCmdAlloc:
  FREE_POOL_SAFE(pFwCmd);
Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to set Alarm Thresholds

  @param[in] pDimm The Intel NVM Dimm to set Alarm Thresholds
  @param[in] ppPayloadAlarmThresholds Alarm Thresholds data to set
  @param[out] pFwReturnCode

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER pDimm or ppPayloadAlarmThresholds is NULL
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
 **/
EFI_STATUS
FwCmdSetAlarmThresholds (
  IN     DIMM *pDimm,
  IN     PT_PAYLOAD_ALARM_THRESHOLDS *pPayloadAlarmThresholds
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pPayloadAlarmThresholds == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtSetFeatures;
  pFwCmd->SubOpcode = SubopAlarmThresholds;
  pFwCmd->InputPayloadSize = sizeof(PT_PAYLOAD_ALARM_THRESHOLDS);
  CopyMem_S(pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), pPayloadAlarmThresholds, pFwCmd->InputPayloadSize);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Error detected when sending AlarmThresholds command (RC = " FORMAT_EFI_STATUS ", Status = %d)", ReturnCode, pFwCmd->Status);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto FinishAfterFwCmdAlloc;
  }

FinishAfterFwCmdAlloc:
  FreePool(pFwCmd);
Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Runs and handles errors errors for firmware update over both large and
  small payloads.

  @param[in] pDimm Pointer to DIMM
  @param[in] pImageBuffer Pointer to fw image buffer
  @param[in] ImageBufferSize Size in bytes of fw image buffer
  @param[out] pNvmStatus Pointer to Nvm status variable to set on error
  @param[out] pCommandStatus OPTIONAL structure containing detailed NVM error codes

  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR if failed to open PassThru protocol
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
**/
EFI_STATUS
FwCmdUpdateFw(
  IN     DIMM *pDimm,
  IN     CONST VOID *pImageBuffer,
  IN     UINTN ImageBufferSize,
     OUT NVM_STATUS *pNvmStatus,
     OUT COMMAND_STATUS *pCommandStatus OPTIONAL
)
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;
  FW_SMALL_PAYLOAD_UPDATE_PACKET *pInputPayload = NULL;
  UINT64 ChunkSize = 0;
  UINT64 BytesToCopy = 0;
  UINT64 BytesWrittenTotal = 0;
  UINT16 PacketOffset = 0;
  UINT8 CurrentRetryCount = 0;
  UINT8 *InputPayloadBuffer = NULL;
  UINT8 ArsStatus = 0;
  UINT8 Percent = 0;
  BOOLEAN LargePayloadAvailable = FALSE;
  BOOLEAN RetryDueToARS = FALSE;

  if (NULL == pDimm || NULL == pImageBuffer || NULL == pNvmStatus) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  CHECK_RESULT_MALLOC(pFwCmd, AllocateZeroPool(sizeof(*pFwCmd)), Finish);

  pFwCmd->Opcode = PtUpdateFw;       //!< Firmware update category
  pFwCmd->SubOpcode = SubopUpdateFw; //!< Execute the firmware image
  pFwCmd->InputPayloadSize = sizeof(*pInputPayload);
  pInputPayload = (FW_SMALL_PAYLOAD_UPDATE_PACKET *) &pFwCmd->InputPayload;

  // Limited number of bytes in small payload packet
  CHECK_RESULT(IsLargePayloadAvailable(pDimm, &LargePayloadAvailable), Finish);
  if (!LargePayloadAvailable) {
    ChunkSize = UPDATE_FIRMWARE_SMALL_PAYLOAD_DATA_PACKET_SIZE;
    pInputPayload->PayloadTypeSelector = FW_UPDATE_SMALL_PAYLOAD_SELECTOR;
    InputPayloadBuffer = pInputPayload->Data;
  } else {
    ChunkSize = ImageBufferSize;
    pInputPayload->PayloadTypeSelector = FW_UPDATE_LARGE_PAYLOAD_SELECTOR;
    pFwCmd->LargeInputPayloadSize = (UINT32)ImageBufferSize;
    InputPayloadBuffer = pFwCmd->LargeInputPayload;
  }

  // Send new firmware image in chunks
  PacketOffset = 0;
  BytesWrittenTotal = 0;
  BytesToCopy = ChunkSize;
  // Large payload will only execute the loop once (one big chunk)
  // and only call INIT_TRANSFER.
  // Small payload will call all of INIT, CONTINUE, and END TRANSFER
  // during chunking.
  while (BytesWrittenTotal < ImageBufferSize) {
    Percent = (UINT8)((BytesWrittenTotal*100)/ImageBufferSize);
    if (NULL != pCommandStatus) {
      SetObjProgress(pCommandStatus, pDimm->DeviceHandle.AsUint32, Percent);
    }

    pInputPayload->PacketNumber = PacketOffset;
    if (BytesWrittenTotal == 0) {
      // Needs to run at the beginning
      pInputPayload->TransactionType = FW_UPDATE_INIT_TRANSFER;
    } else if (BytesWrittenTotal < ImageBufferSize - BytesToCopy) {
      // Should run in the middle. Won't occur for large payload
      pInputPayload->TransactionType = FW_UPDATE_CONTINUE_TRANSFER;
    } else {
      // Runs at end for small payload only, it includes final chunk.
      // Not needed for large payload
      pInputPayload->TransactionType = FW_UPDATE_END_TRANSFER;
    }

    // The chunksize won't change for small payload (fw image size was already
    // enforced to be a multiple of 64 bytes), but it potentially could for
    // large payload at some point.
    BytesToCopy = MIN(ImageBufferSize - BytesWrittenTotal, ChunkSize);


    NVDIMM_DBG("BytesToCopy: %d %d / %d. TT: 0x%x", BytesToCopy, BytesWrittenTotal, ImageBufferSize, pInputPayload->TransactionType);
    CopyMem_S(InputPayloadBuffer, BytesToCopy, (UINT8 *)pImageBuffer + BytesWrittenTotal, BytesToCopy);

    ReturnCode = PassThru(pDimm, pFwCmd, PT_UPDATEFW_TIMEOUT_INTERVAL);

    if (EFI_ERROR(ReturnCode)) {
      // Try to cancel Address Range Scrub (ARS) if it is in progress
      // on the DCPMM (and is returning FW_DEVICE_BUSY as a result).
      // However for Purley BIOS, it can intercept a failed fw update command
      // due to ARS, so it will return DSM_RETRY_SUGGESTED. We should honor
      // that as well and retry after cancelling ARS.
      RetryDueToARS = pFwCmd->Status == FW_DEVICE_BUSY;
#ifdef OS_BUILD
      RetryDueToARS |= pFwCmd->DsmStatus == DSM_RETRY_SUGGESTED;
#endif
      if (RetryDueToARS) {
        if (++CurrentRetryCount >= MAX_FW_UPDATE_RETRY_ON_DEV_BUSY) {
          *pNvmStatus = NVM_ERR_BUSY_DEVICE;
          ReturnCode = EFI_ABORTED;
          goto Finish;
        }

        // If there's an issue getting ARS information or canceling ARS
        // we don't need to abort because of it. At least two scenarios:
        //   * Can't cancel ARS for some reason. Retry MAX_FW_UPDATE_RETRY_ON_DEV_BUSY times, it's fine
        //   * Cancel ARS fails because ARS just completed naturally in between
        //     receiving IN_PROGRESS and sending disable ARS (Linux kernel can restart ARS
        //     if cancelled). Also not an issue. Continue.
        CHECK_RESULT_CONTINUE(FwCmdGetARS(pDimm, &ArsStatus));

        if (ARS_STATUS_IN_PROGRESS == ArsStatus) {
          NVDIMM_DBG("ARS in progress. Disabling ARS.\n");
          CHECK_RESULT_CONTINUE(FwCmdDisableARS(pDimm));
        }
        // Retry current packet
        continue;
      }
      else if (pFwCmd->Status == FW_UPDATE_ALREADY_OCCURED) {
        NVDIMM_DBG("FW Update failed, FW already occured\n");
        *pNvmStatus = NVM_ERR_FIRMWARE_ALREADY_LOADED;
        goto Finish;
      }
      else {
        *pNvmStatus = NVM_ERR_OPERATION_FAILED;
        goto Finish;
      }
    }

    PacketOffset++;
    BytesWrittenTotal += BytesToCopy;
  }

  pDimm->RebootNeeded = TRUE;

  *pNvmStatus = NVM_SUCCESS_FW_RESET_REQUIRED;
  ReturnCode = EFI_SUCCESS;

Finish:
  if (NULL != pCommandStatus && NULL != pDimm) {
    ClearNvmStatus(GetObjectStatus(pCommandStatus, pDimm->DeviceHandle.AsUint32), NVM_OPERATION_IN_PROGRESS);
  }
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}


 /**
  Firmware command to get debug logs size in MB

  @param[in] pDimm Target DIMM structure pointer
  @param[out] pLogSizeInMb - number of MB of Logs to be fetched

  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR if failed to open PassThru protocol
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
**/
EFI_STATUS
FwCmdGetFwDebugLogSize(
  IN     DIMM *pDimm,
     OUT UINT64 *pLogSizeInMb
  )
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  NVM_FW_CMD *pFwCmd = NULL;
  PT_OUTPUT_PAYLOAD_FW_DEBUG_LOG *pDbgSmallOutPayload = NULL;
  PT_INPUT_PAYLOAD_FW_DEBUG_LOG *pInputPayload = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pLogSizeInMb == NULL) {
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetLog;
  pFwCmd->SubOpcode = SubopFwDbg;
  pFwCmd->InputPayloadSize = sizeof(PT_INPUT_PAYLOAD_FW_DEBUG_LOG);
  pFwCmd->OutputPayloadSize = sizeof(PT_OUTPUT_PAYLOAD_FW_DEBUG_LOG);
  pInputPayload = (PT_INPUT_PAYLOAD_FW_DEBUG_LOG *) &pFwCmd->InputPayload;
  pInputPayload->LogAction = ActionRetrieveDbgLogSize;

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);

  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Failed to get FW debug log size");
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }
  pDbgSmallOutPayload = (PT_OUTPUT_PAYLOAD_FW_DEBUG_LOG *)pFwCmd->OutPayload;
  *pLogSizeInMb = pDbgSmallOutPayload->LogSize;

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to get a specified debug log

  @param[in]  pDimm Target DIMM structure pointer
  @param[in]  LogSource Debug log source buffer to retrieve
  @param[out] ppDebugLogBuffer - an allocated buffer containing the raw debug logs
  @param[out] pDebugLogBufferSize - the size of the raw debug log buffer
  @param[out] pCommandStatus structure containing detailed NVM error codes

  Note: The caller is responsible for freeing the returned buffers

  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR if failed to open PassThru protocol
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
**/
EFI_STATUS
FwCmdGetFwDebugLog (
  IN     DIMM *pDimm,
  IN     UINT8 LogSource,
     OUT VOID **ppDebugLogBuffer,
     OUT UINTN *pDebugLogBufferSize,
     OUT COMMAND_STATUS *pCommandStatus
  )
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  UINT32 LogPageOffset = 0;
  UINT64 CurrentDebugLogSizeInMbs = 0;
  UINT64 LogSizeBytesToFetch = 0;
  PT_INPUT_PAYLOAD_FW_DEBUG_LOG *pInputPayload = NULL;
  UINT64 ChunkSize = 0;
  UINT64 BytesToCopy = 0;
  UINT64 BytesReadTotal = 0;
  UINT8 LogAction = 0;
  UINT8 *OutputPayload = NULL;
  BOOLEAN LargePayloadAvailable = FALSE;

  NVDIMM_ENTRY();

  if (pDimm == NULL || ppDebugLogBuffer == NULL || pDebugLogBufferSize == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  // Populate log size bytes to fetch
  switch (LogSource)
  {
    case FW_DEBUG_LOG_SOURCE_MEDIA:
      LogAction = ActionGetDbgLogPage;
      ReturnCode = FwCmdGetFwDebugLogSize(pDimm, &CurrentDebugLogSizeInMbs);
      if (EFI_ERROR(ReturnCode)) {
        if (ReturnCode == EFI_SECURITY_VIOLATION) {
          SetObjStatusForDimm(pCommandStatus, pDimm, NVM_ERR_INVALID_SECURITY_STATE);
        } else {
          SetObjStatusForDimm(pCommandStatus, pDimm, NVM_ERR_FW_DBG_LOG_FAILED_TO_GET_SIZE);
        }
        goto Finish;
      }
      LogSizeBytesToFetch = MIB_TO_BYTES(CurrentDebugLogSizeInMbs);
      break;
    case FW_DEBUG_LOG_SOURCE_SRAM:
      LogAction = ActionGetSramLogPage;
      LogSizeBytesToFetch = SRAM_LOG_PAGE_SIZE_BYTES;
      break;
    case FW_DEBUG_LOG_SOURCE_SPI:
      LogAction = ActionGetSpiLogPage;
      LogSizeBytesToFetch = SPI_LOG_PAGE_SIZE_BYTES;
      break;
    default:
      ReturnCode = EFI_INVALID_PARAMETER;
      goto Finish;
  }

  if (LogSizeBytesToFetch == 0)
  {
    SetObjStatusForDimm(pCommandStatus, pDimm, NVM_INFO_FW_DBG_LOG_NO_LOGS_TO_FETCH);
    ReturnCode = EFI_NOT_STARTED;
    goto Finish;
  }

  *ppDebugLogBuffer = AllocateZeroPool(LogSizeBytesToFetch);
  if (*ppDebugLogBuffer == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->Opcode = PtGetLog;
  pFwCmd->SubOpcode = SubopFwDbg;
  pFwCmd->InputPayloadSize = sizeof(*pInputPayload);
  pInputPayload = (PT_INPUT_PAYLOAD_FW_DEBUG_LOG *) &pFwCmd->InputPayload;
  pInputPayload->LogAction = LogAction;

  // Default for DDRT large payload transactions. 128 bytes for smbus
  CHECK_RESULT(IsLargePayloadAvailable(pDimm, &LargePayloadAvailable), Finish);
  if (!LargePayloadAvailable) {
    ChunkSize = SMALL_PAYLOAD_SIZE;
    OutputPayload = pFwCmd->OutPayload;
    pInputPayload->PayloadType = DEBUG_LOG_PAYLOAD_TYPE_SMALL;
    pFwCmd->OutputPayloadSize = SMALL_PAYLOAD_SIZE;
    pFwCmd->LargeOutputPayloadSize = 0;
  } else {
    ChunkSize = MIB_TO_BYTES(1);
    OutputPayload = pFwCmd->LargeOutputPayload;
    pInputPayload->PayloadType = DEBUG_LOG_PAYLOAD_TYPE_LARGE;
    pFwCmd->OutputPayloadSize = 0;
    pFwCmd->LargeOutputPayloadSize = OUT_MB_SIZE;
  }

  /** Fetch whole buffer, iterate by chunk size **/
  LogPageOffset = 0;
  BytesReadTotal = 0;
  while (BytesReadTotal < LogSizeBytesToFetch) {

    pInputPayload->LogPageOffset = LogPageOffset;
    ReturnCode = PassThru(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);

    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_WARN("Failed to get firmware debug log, LogPageOffset = %d\n", LogPageOffset);
      FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
      goto Finish;
    }

    BytesToCopy = MIN(LogSizeBytesToFetch - BytesReadTotal, ChunkSize);
    CopyMem_S((UINT8 *)*ppDebugLogBuffer + BytesReadTotal, BytesToCopy, OutputPayload, BytesToCopy);
    LogPageOffset++;
    BytesReadTotal += BytesToCopy;
  }
  *(pDebugLogBufferSize) = BytesReadTotal;


Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to get Error logs

  Small and large payloads are optional, but at least one has to be provided.

  @param[in] pDimm Target DIMM structure pointer
  @param[in] pInputPayload - filled input payload
  @param[out] pOutputPayload - small payload result data of get error log operation
  @param[in] OutputPayloadSize - size of small payload
  @param[out] pLargeOutputPayload - large payload result data of get error log operation
  @param[in] LargeOutputPayloadSize - size of large payload

  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR if failed to open PassThru protocol
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
**/
EFI_STATUS
FwCmdGetErrorLog (
  IN     DIMM *pDimm,
  IN     PT_INPUT_PAYLOAD_GET_ERROR_LOG *pInputPayload,
     OUT VOID *pOutputPayload OPTIONAL,
  IN     UINT32 OutputPayloadSize OPTIONAL,
     OUT VOID *pLargeOutputPayload OPTIONAL,
  IN     UINT32 LargeOutputPayloadSize OPTIONAL
  )
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  if (pInputPayload == NULL || (pOutputPayload == NULL && pLargeOutputPayload == NULL)) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetLog;
  pFwCmd->SubOpcode = SubopErrorLog;
  pFwCmd->InputPayloadSize = sizeof(*pInputPayload);
  pFwCmd->OutputPayloadSize = OutputPayloadSize;
  pFwCmd->LargeOutputPayloadSize = LargeOutputPayloadSize;
  CopyMem_S(&pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), pInputPayload, pFwCmd->InputPayloadSize);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_LONG_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Failed to get error log\n");
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  if (pOutputPayload != NULL && OutputPayloadSize > 0) {
    CopyMem_S(pOutputPayload, OutputPayloadSize, &pFwCmd->OutPayload, OutputPayloadSize);
  }

  if (pLargeOutputPayload != NULL && LargeOutputPayloadSize > 0) {
    CopyMem_S(pLargeOutputPayload, LargeOutputPayloadSize, &pFwCmd->LargeOutputPayload, LargeOutputPayloadSize);
  }

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to get Command Effect Log Entries

  Large payload is optional, small payload is required.

  @param[in] pDimm Target DIMM structure pointer
  @param[in] pInputPayload - filled input payload
  @param[out] pOutputPayload - small payload result data of get command effect log operation
  @param[in] OutputPayloadSize - size of small payload
  @param[out] pLargeOutputPayload - large payload result data of command effect log operation
  @param[in] LargeOutputPayloadSize - size of large payload

  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR if failed to open PassThru protocol
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
**/
EFI_STATUS
FwCmdGetCommandEffectLog(
  IN      DIMM  *pDimm,
  IN      PT_INPUT_PAYLOAD_GET_COMMAND_EFFECT_LOG *pInputPayload,
      OUT VOID *pOutputPayload,
  IN      UINT32 OutputPayloadSize,
      OUT VOID *pLargeOutputPayload OPTIONAL,
  IN      UINT32 LargeOutputPayloadSize OPTIONAL
)
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pInputPayload == NULL || pOutputPayload == NULL || (pLargeOutputPayload == NULL && LargeOutputPayloadSize > 0)) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetLog;
  pFwCmd->SubOpcode = SubopCommandEffectLog;
  pFwCmd->InputPayloadSize = sizeof(*pInputPayload);
  pFwCmd->OutputPayloadSize = OutputPayloadSize;
  pFwCmd->LargeOutputPayloadSize = LargeOutputPayloadSize;
  CopyMem_S(&pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), pInputPayload, pFwCmd->InputPayloadSize);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending Command Effect Log command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  if (pOutputPayload != NULL && OutputPayloadSize > 0) {
    CopyMem_S(pOutputPayload, OutputPayloadSize, &pFwCmd->OutPayload, OutputPayloadSize);
  }

  if (pLargeOutputPayload != NULL && LargeOutputPayloadSize > 0) {
    CopyMem_S(pLargeOutputPayload, LargeOutputPayloadSize, &pFwCmd->LargeOutputPayload, LargeOutputPayloadSize);
  }

  ReturnCode = EFI_SUCCESS;
  goto Finish;

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to get SMART and Health Info

  @param[in] pDimm The Intel NVM Dimm to retrieve SMART and Health Info
  @param[out] ppPayloadSmartAndHealth Area to place SMART and Health Info data
    The caller is responsible to free the allocated memory with the FreePool function.

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER pDimm or ppPayloadSmartAndHealth is NULL
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
**/
EFI_STATUS
FwCmdGetSmartAndHealth (
  IN     DIMM *pDimm,
     OUT PT_PAYLOAD_SMART_AND_HEALTH **ppPayloadSmartAndHealth
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || ppPayloadSmartAndHealth == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetLog;
  pFwCmd->SubOpcode = SubopSmartHealth;
  pFwCmd->OutputPayloadSize = sizeof(**ppPayloadSmartAndHealth);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Error detected when sending SmartAndHealth command (RC = " FORMAT_EFI_STATUS ", Status = %d)", ReturnCode, pFwCmd->Status);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  *ppPayloadSmartAndHealth = AllocateZeroPool(sizeof(**ppPayloadSmartAndHealth));
  if (*ppPayloadSmartAndHealth == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }
  CopyMem_S(*ppPayloadSmartAndHealth, sizeof(**ppPayloadSmartAndHealth), pFwCmd->OutPayload, sizeof(**ppPayloadSmartAndHealth));

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Command to send a pass-through firmware command to retrieve a specified memory info page

  @param[in] pDimm Dimm to retrieve the specified memory info page from
  @param[in] PageNum The specific memory info page
  @param[in] PageSize The size of memory info page, which is 128 bytes
  @param[out] ppPayloadMemoryInfoPage Area to place the retrieved memory info page contents
    The caller is responsible to free the allocated memory with the FreePool function.

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER pDimm or ppPayloadMediaErrorsInfo is NULL
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
**/
EFI_STATUS
FwCmdGetMemoryInfoPage (
  IN     DIMM *pDimm,
  IN     CONST UINT8 PageNum,
  IN     CONST UINT32 PageSize,
     OUT VOID **ppPayloadMemoryInfoPage
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;
  PT_INPUT_PAYLOAD_MEMORY_INFO InputPayload;

  NVDIMM_ENTRY();

  SetMem(&InputPayload, sizeof(InputPayload), 0x0);

  if (pDimm == NULL || ppPayloadMemoryInfoPage == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  if (MEMORY_INFO_PAGE_4 == PageNum && pDimm->FwVer.FwApiMajor < 2) {
    ReturnCode = EFI_UNSUPPORTED;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  InputPayload.MemoryPage = PageNum;

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetLog;
  pFwCmd->SubOpcode = SubopMemInfo;
  pFwCmd->InputPayloadSize  = sizeof(InputPayload);
  pFwCmd->OutputPayloadSize = PageSize;

  CopyMem_S(pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), &InputPayload, pFwCmd->InputPayloadSize);
  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Error detected when sending MemoryInfoPage command (RC = " FORMAT_EFI_STATUS ", Status = %d)", ReturnCode, pFwCmd->Status);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto FinishAfterFwCmdAlloc;
  }

  *ppPayloadMemoryInfoPage = AllocateZeroPool(PageSize);
  if (*ppPayloadMemoryInfoPage == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto FinishAfterFwCmdAlloc;
  }
  CopyMem_S(*ppPayloadMemoryInfoPage, PageSize, pFwCmd->OutPayload, pFwCmd->OutputPayloadSize);

FinishAfterFwCmdAlloc:
  FREE_POOL_SAFE(pFwCmd);
Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to get Firmware Image Info

  @param[in] pDimm Dimm to retrieve Firmware Image Info for
  @param[out] ppPayloadFwImage Area to place Firmware Image Info data
    The caller is responsible to free the allocated memory with the FreePool function.

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER pDimm or ppPayloadFwImage is NULL
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
**/
EFI_STATUS
FwCmdGetFirmwareImageInfo (
  IN     DIMM *pDimm,
     OUT PT_PAYLOAD_FW_IMAGE_INFO **ppPayloadFwImage
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || ppPayloadFwImage == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetLog;
  pFwCmd->SubOpcode = SubopFwImageInfo;
  pFwCmd->OutputPayloadSize = sizeof(**ppPayloadFwImage);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Error detected when sending FirmwareImageInfo command (RC = " FORMAT_EFI_STATUS ", Status = %d)", ReturnCode, pFwCmd->Status);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto FinishError;
  }

  *ppPayloadFwImage = AllocateZeroPool(sizeof(**ppPayloadFwImage));
  if (*ppPayloadFwImage == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto FinishError;
  }
  CopyMem_S(*ppPayloadFwImage, sizeof(**ppPayloadFwImage), pFwCmd->OutPayload, sizeof(**ppPayloadFwImage));

FinishError:
  FREE_POOL_SAFE(pFwCmd);
Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to get Power Management Policy Info (for FIS 1.3+)

  @param[in] pDimm The Intel Persistent Memory Module to retrieve Power Management Policy Info
  @param[out] ppPayloadPowerManagementPolicy Area to place Power Management Policy Info data
    The caller is responsible to free the allocated memory with the FreePool function.

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER pDimm or ppPayloadPowerManagementPolicy is NULL
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
**/
EFI_STATUS
FwCmdGetPowerManagementPolicy(
  IN     DIMM *pDimm,
     OUT PT_POWER_MANAGEMENT_POLICY_OUT **ppPayloadPowerManagementPolicy
  )
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  NVM_FW_CMD *pFwCmd = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || ppPayloadPowerManagementPolicy == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  *ppPayloadPowerManagementPolicy = AllocateZeroPool(sizeof(**ppPayloadPowerManagementPolicy));
  if (NULL == *ppPayloadPowerManagementPolicy) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetFeatures;
  pFwCmd->SubOpcode = SubopPolicyPowMgmt;
  pFwCmd->OutputPayloadSize = sizeof((*ppPayloadPowerManagementPolicy)->Payload);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Error detected when sending PowerManagementPolicy command (RC = " FORMAT_EFI_STATUS ", Status = %d)", ReturnCode, pFwCmd->Status);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  CopyMem_S((*ppPayloadPowerManagementPolicy)->Payload.Data, sizeof((*ppPayloadPowerManagementPolicy)->Payload), pFwCmd->OutPayload, sizeof((*ppPayloadPowerManagementPolicy)->Payload));
  (*ppPayloadPowerManagementPolicy)->FisMajor = pDimm->FwVer.FwApiMajor;
  (*ppPayloadPowerManagementPolicy)->FisMinor = pDimm->FwVer.FwApiMinor;

  ReturnCode = EFI_SUCCESS;

Finish:
  if (EFI_ERROR(ReturnCode) && (NULL != ppPayloadPowerManagementPolicy)) {
    FREE_POOL_SAFE(*ppPayloadPowerManagementPolicy);
  }
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

#ifdef OS_BUILD

/**
  Firmware command to get PMON Info

  @param[in] pDimm The Intel Persistent Memory Module to retrieve PMON Info
  @param[out] pPayloadPMONRegisters Area to place PMON Registers data
    The caller is responsible to free the allocated memory with the FreePool function.

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER pDimm or pPayloadPMONRegisters is NULL
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
**/
EFI_STATUS
FwCmdGetPMONRegisters(
  IN     DIMM *pDimm,
  IN     UINT8 SmartDataMask,
  OUT    PMON_REGISTERS *pPayloadPMONRegisters
  )
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  NVM_FW_CMD *pFwCmd = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pPayloadPMONRegisters == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetFeatures;
  pFwCmd->SubOpcode = SubopPMONRegisters;
  pFwCmd->InputPayload[0] = SmartDataMask;
  pFwCmd->InputPayloadSize = sizeof(SmartDataMask);
  pFwCmd->OutputPayloadSize = sizeof(*pPayloadPMONRegisters);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Error detected when sending PMONRegisters command (RC = " FORMAT_EFI_STATUS ", Status = %d)", ReturnCode, pFwCmd->Status);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  CopyMem_S(pPayloadPMONRegisters, sizeof(*pPayloadPMONRegisters), pFwCmd->OutPayload, sizeof(*pPayloadPMONRegisters));

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to set PMON Info

  @param[in] pDimm The Intel Persistent Memory Module to retrieve PMON Info
  @param[out] PMONGroupEnable  Specifies which PMON Group to enable.
    The caller is responsible to free the allocated memory with the FreePool function.

  @retval EFI_SUCCESS Success
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
**/
EFI_STATUS
FwCmdSetPMONRegisters(
  IN     DIMM *pDimm,
  IN     UINT8 PMONGroupEnable
  )
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  NVM_FW_CMD *pFwCmd = NULL;


  NVDIMM_ENTRY();

/**
...Valid  PMON groups -0xA -0xF
**/
  if (pDimm == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtSetFeatures;
  pFwCmd->SubOpcode = SubopPMONRegisters;
  pFwCmd->InputPayload[0] = PMONGroupEnable;
  pFwCmd->InputPayloadSize = sizeof(PMONGroupEnable);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Error detected when sending PMONRegisters command (RC = " FORMAT_EFI_STATUS ", Status = %d)", ReturnCode, pFwCmd->Status);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}
#endif
/**
  Firmware command to get package sparing policy

  @param[in] pDimm The Intel NVM Dimm to retrieve Package Sparing policy
  @param[out] ppPayloadPackageSparingPolicy Area to place Package Sparing policy data
    The caller is responsible to free the allocated memory with the FreePool function.

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER pDimm or ppPayloadPackageSparingPolicy is NULL
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
**/
EFI_STATUS
FwCmdGetPackageSparingPolicy (
  IN     DIMM *pDimm,
     OUT PT_PAYLOAD_GET_PACKAGE_SPARING_POLICY **ppPayloadPackageSparingPolicy
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || ppPayloadPackageSparingPolicy == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetFeatures;
  pFwCmd->SubOpcode = SubopPolicyPackageSparing;
  pFwCmd->OutputPayloadSize = sizeof(**ppPayloadPackageSparingPolicy);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Error detected when sending GetPackageSparingPolicy command (RC = " FORMAT_EFI_STATUS ", Status = %d)", ReturnCode, pFwCmd->Status);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto FinishAfterFwCmdAlloc;
  }

  *ppPayloadPackageSparingPolicy = AllocateZeroPool(sizeof(**ppPayloadPackageSparingPolicy));
  if (*ppPayloadPackageSparingPolicy == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto FinishAfterFwCmdAlloc;
  }
  CopyMem_S(*ppPayloadPackageSparingPolicy, sizeof(**ppPayloadPackageSparingPolicy), pFwCmd->OutPayload, sizeof(**ppPayloadPackageSparingPolicy));

FinishAfterFwCmdAlloc:
  FreePool(pFwCmd);
Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Get long operation status FW command

  @param[in] pDimm Dimm to retrieve long operation status from
  @param[out] pFwStatus FW status returned by dimm.
  @param[out] pLongOpStatus Filled payload with data

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER One or more pamaters are NULL
  @retval EFI_OUT_OF_RESOURCES Memory allocation failure
**/
EFI_STATUS
FwCmdGetLongOperationStatus(
  IN     DIMM *pDimm,
     OUT UINT8 *pFwStatus,
     OUT PT_OUTPUT_PAYLOAD_FW_LONG_OP_STATUS *pLongOpStatus
  )
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  NVM_FW_CMD *pFwCmd = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pFwStatus == NULL || pLongOpStatus == NULL) {
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetLog;
  pFwCmd->SubOpcode = SubopLongOperationStat;
  pFwCmd->OutputPayloadSize = sizeof(*pLongOpStatus);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  *pFwStatus = pFwCmd->Status;
  if (EFI_ERROR(ReturnCode)) {
    /** FW_INTERNAL_DEVICE_ERROR or FW_DATA_NOT_SET occurs when there is no long operation at this moment. Which one depends on FIS**/
    if (!(pDimm->FwVer.FwApiMajor == 1 && pDimm->FwVer.FwApiMinor <= 4 && pFwCmd->Status == FW_INTERNAL_DEVICE_ERROR) &&
      pFwCmd->Status != FW_DATA_NOT_SET) {
      NVDIMM_WARN("Error detected when sending LongOperationStatus command (RC = " FORMAT_EFI_STATUS ", Status = %d)",
          ReturnCode, pFwCmd->Status);
    }
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  CopyMem_S(pLongOpStatus, sizeof(*pLongOpStatus), pFwCmd->OutPayload, sizeof(*pLongOpStatus));

  ReturnCode = EFI_SUCCESS;

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Free memory for a single block window
  Frees the resources held by a block window

  @param[out] bw: The block window to free
**/
VOID
FreeBlockWindow(
     OUT BLOCK_WINDOW *pBw
  )
{
  NVDIMM_ENTRY();
  if (pBw != NULL) {
    FREE_POOL_SAFE(pBw->ppBwApt);
  }
  FREE_POOL_SAFE(pBw);
  NVDIMM_EXIT();
}

/**
  Assign spa address to a given mailbox or block window field.

  @param Rdpa Device Region Physical Address to convert
  @param pNvDimmRegionTable The NVDIMM region that helps describe this region of memory
  @param pIntTbl Interleave table
  @param ppField mailbox or block window field to assign to

  @retval EFI_SUCCESS
  @retval EFI_INVALID_PARAMETER when pIntTbl has 0 values in fields line_size or lines_described
**/
EFI_STATUS
AssignSpaAddress(
  IN     UINT64 Rdpa,
  IN     NvDimmRegionMappingStructure *pNvDimmRegionTable,
  IN     SpaRangeTbl *pSpaRangeTable,
  IN     InterleaveStruct *pIntTbl OPTIONAL,
     OUT VOID **ppField
  )
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  UINT64 SpaAddr = 0;

  if (pNvDimmRegionTable == NULL || pSpaRangeTable == NULL) {
    goto Finish;
  }

  ReturnCode = RdpaToSpa(Rdpa, pNvDimmRegionTable, pSpaRangeTable, pIntTbl, &SpaAddr);
  if (!EFI_ERROR(ReturnCode)) {
    *ppField = (VOID *) SpaAddr;
  }

Finish:
  return ReturnCode;
}

static
UINT16
ParseFwBuild(
  IN     UINT8 Mbs,
  IN     UINT8 Lsb
  )
{
  return (BCD_TO_TWO_DEC(Mbs) * 100) + BCD_TO_TWO_DEC(Lsb);
}

/**
  Parse Firmware Version
  Parse the FW version returned by the FW into a CPU format
  FW Payload has the FW version encoded in a binary coded decimal format

  @param[in] Fwr - Firmware revision in BCD format

  @retval Parsed firmware version as friendly FIRMWARE_VERSION structure
**/
FIRMWARE_VERSION
ParseFwVersion(
  IN     UINT8 Fwr[FW_BCD_VERSION_LEN]
  )
{
  FIRMWARE_VERSION FwVer;

  NVDIMM_ENTRY();

  ZeroMem(&FwVer, sizeof(FwVer));

  if (Fwr == NULL) {
    goto Finish;
  }

  FwVer.FwProduct  = BCD_TO_TWO_DEC(Fwr[FWR_PRODUCT_VERSION_OFFSET]);
  FwVer.FwRevision  = BCD_TO_TWO_DEC(Fwr[FWR_REVISION_VERSION_OFFSET]);
  FwVer.FwSecurityVersion = BCD_TO_TWO_DEC(Fwr[FWR_SECURITY_VERSION_OFFSET]);
  FwVer.FwBuild = ParseFwBuild(Fwr[FWR_BUILD_VERSION_HI_OFFSET],
    Fwr[FWR_BUILD_VERSION_LOW_OFFSET]);

Finish:
  NVDIMM_EXIT();
  return FwVer;
}

/**
  Parse the BCD formatted FW API version into major and minor

  @param[out] pDimm
  @param[in] pPayload
**/
VOID
ParseFwApiVersion(
     OUT DIMM *pDimm,
  IN     PT_ID_DIMM_PAYLOAD *pPayload
  )
{
  NVM_API_VERSION ApiVersion;

  NVDIMM_ENTRY();

  ZeroMem(&ApiVersion, sizeof(ApiVersion));

  ApiVersion.Version = pPayload->ApiVer;

  pDimm->FwVer.FwApiMajor = BCD_TO_TWO_DEC(ApiVersion.Byte.Digit1);
  pDimm->FwVer.FwApiMinor = BCD_TO_TWO_DEC(ApiVersion.Byte.Digit2);

  ZeroMem(&ApiVersion, sizeof(ApiVersion));
  ApiVersion.Version = pPayload->ActiveApiVer;
  pDimm->FwActiveApiVersionMajor = BCD_TO_TWO_DEC(ApiVersion.Byte.Digit1);
  pDimm->FwActiveApiVersionMinor = BCD_TO_TWO_DEC(ApiVersion.Byte.Digit2);

  NVDIMM_EXIT();
}

/**
  This function performs a DIMM information refresh through the
  DIMM Information FV command.

  @param[in,out] pDimm the DIMM that we want to refresh.

  @retval EFI_SUCCESS - the DIMM was refreshed successfully.
  @retval EFI_INVALID_PARAMETER - pDimm is NULL.
  @retval EFI_OUT_OF_RESOURCES - the memory allocation failed.
**/
EFI_STATUS
RefreshDimm(
  IN OUT DIMM *pDimm
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  PT_ID_DIMM_PAYLOAD *pPayload = NULL;
  UINT32 Index = 0;
  /** @todo DE9699 Remove FIS 1.2 backwards compatibility workaround **/
  UINT16 IfcExtra = 0x201;

  NVDIMM_ENTRY();
  if (pDimm == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pPayload = AllocatePool(sizeof(PT_ID_DIMM_PAYLOAD));

  if (pPayload == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  ReturnCode = FwCmdIdDimm(pDimm, pPayload);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("FW CMD Error: " FORMAT_EFI_STATUS "", ReturnCode);
    goto Finish;
  }

  for (Index = 0; Index < pDimm->FmtInterfaceCodeNum; Index++) {
    if (pDimm->FmtInterfaceCode[Index] != pPayload->Ifc && pDimm->FmtInterfaceCode[Index] != IfcExtra) {
      NVDIMM_WARN("FIT and FW Interface Code mismatch");
      ReturnCode = EFI_DEVICE_ERROR;
      goto Finish;
    }
  }

  pDimm->FwVer = ParseFwVersion(pPayload->Fwr);
  ParseFwApiVersion(pDimm, pPayload);

Finish:
  if (pPayload != NULL) {
    FreePool(pPayload);
  }
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Create and configure block window
  Create the block window structure. This includes locating
  each part of the block window in the system physical address space, and
  mapping each part into the virtual address space.

  @param[in, out] pDimm: DIMM to create the Bw for
  @param[in] PFitHead: Parsed Fit Head
  @parma[in] pMbITbl: the interleave table for mailbox
  @parma[in] pBwITbl: the interleave table for block window

  @retval EFI_SUCCESS
  @retval EFI_OUT_OF_RESOURCES in case of allocate memory error
**/
EFI_STATUS
EFIAPI
CreateBw(
  IN OUT DIMM *pDimm,
  IN     ParsedFitHeader *pFitHead,
  IN     InterleaveStruct *pMbITbl OPTIONAL,
  IN     InterleaveStruct *pBwITbl OPTIONAL
  )
{
  BLOCK_WINDOW *pBw = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  ControlRegionTbl *pControlRegTbl = NULL;
  BWRegionTbl *pBlockDataWindowTable = NULL;
  UINT32 Index = 0;
  UINT64 SpaAddr = 0;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pDimm->pBlockDataRegionMappingStructure == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pBw = (BLOCK_WINDOW *)AllocateZeroPool(sizeof(*pBw));

  if (pBw == NULL) {
    NVDIMM_WARN("Unable to allocate block windows memory");
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  // Getting Control Region Table with all needed BW values
  ReturnCode = GetControlRegionTableForNvDimmRegionTable(pFitHead,
      pDimm->pBlockDataRegionMappingStructure, &pControlRegTbl);
  if (pControlRegTbl == NULL || EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Unable to get Control region table. Returned: " FORMAT_EFI_STATUS "", ReturnCode);
    ReturnCode = EFI_ABORTED;
    goto Finish;
  }

  // Getting Block Data Windows Region Description table to get the misc offsets
  ReturnCode = GetBlockDataWindowRegDescTabl(pFitHead, pControlRegTbl, &pBlockDataWindowTable);
  if (pBlockDataWindowTable == NULL || EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Unable to get Block Data Window table. Returned: " FORMAT_EFI_STATUS "", ReturnCode);
    ReturnCode = EFI_ABORTED;
    goto Finish;
  }

  /** Control Register **/
  ReturnCode = AssignSpaAddress(pControlRegTbl->CommandRegisterOffsetInBlockControlWindow,
      pDimm->pRegionMappingStructure, pDimm->pCtrlSpaTbl, pMbITbl, (VOID *) &(pBw->pBwCmd));
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }
  NVDIMM_DBG("BW Command address = %p", pBw->pBwCmd);

  /** BW Status **/
  ReturnCode = AssignSpaAddress(pControlRegTbl->StatusRegisterOffsetInBlockControlWindow,
      pDimm->pRegionMappingStructure, pDimm->pCtrlSpaTbl, pMbITbl, (VOID *) &(pBw->pBwStatus));
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }
  NVDIMM_DBG("BW Status address = %p", pBw->pBwStatus);

  /** Aperture **/
  if (pBwITbl != NULL) {
    pBw->LineSizeOfApt = pBwITbl->LineSize;
    pBw->NumSegmentsOfApt = BW_APERTURE_LENGTH / pBwITbl->LineSize;
  } else {
    pBw->LineSizeOfApt = BW_APERTURE_LENGTH;
    pBw->NumSegmentsOfApt = 1;
  }
  pBw->ppBwApt = (volatile VOID **) AllocateZeroPool(pBw->NumSegmentsOfApt * sizeof(*pBw->ppBwApt));

  if (pBw->ppBwApt == NULL) {
    NVDIMM_WARN("Unable to allocate aperture in block window");
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  for (Index = 0; Index < pBw->NumSegmentsOfApt; Index++) {
    ReturnCode = RdpaToSpa(pBlockDataWindowTable->BlockDataWindowStartLogicalOffset + (Index * pBw->LineSizeOfApt),
      pDimm->pBlockDataRegionMappingStructure, pDimm->pBlockDataSpaTbl, pBwITbl, &SpaAddr);
    pBw->ppBwApt[Index] = (VOID *) SpaAddr;
    if (EFI_ERROR(ReturnCode)) {
      goto Finish;
    }
  }
  NVDIMM_DBG("First interleaved BW Aperture address = %p", pBw->ppBwApt[0]);
  pDimm->pBw = pBw;

Finish:
  if (EFI_ERROR(ReturnCode)) {
    FreeBlockWindow(pBw);
  }
  NVDIMM_EXIT_I(ReturnCode);
  return ReturnCode;
}

/**
  Set Block Window Command to read/write operation

  @param[in] Dpa - Memory DPA
  @param[in] Length - The transfer size is the number of cache lines (Cache line = 64 bytes)
  @param[in] BwOperation - Read/Write command
  @param[out] pBwCommand - 64-bit Command Register buffer

                  BW Command & Address Register

   | RESERVED | CMD | SIZE | Reserved  |    BW ADDRESS    |
   |64      58| 57  |56  49|48       38|37              0|
                                       |43  DPA ADDRESS  6|5    0|
**/
VOID
PrepareBwCommand(
  IN     UINT64 Dpa,
  IN     UINT8 Length,
  IN     UINT8 BwOperation,
     OUT UINT64 *pCommand
  )
{
  NVDIMM_ENTRY();

  if (pCommand == NULL) {
    return;
  }

  union {
    struct {
      UINT64 Dpa : 37;
      UINT64 Rsvd : 11;
      UINT64 WinSize : 8;
      UINT64 RwLock : 1;
      UINT64 Rsvd2 : 7;
    } Command;
    UINT64 AsUint64;
  } CommandTemp;

  CommandTemp.Command.RwLock = BwOperation;
  CommandTemp.Command.WinSize = Length;
  CommandTemp.Command.Dpa = (Dpa >> BW_DPA_RIGHT_SHIFT);

  *pCommand = CommandTemp.AsUint64;
  NVDIMM_EXIT();
}

/**
  Check Block Input Parameters

  @param[in] pDimm: DIMM to check block window pointers

  @retval EFI_INVALID_PARAMETER if pDimm or some internal Block Window pointer is NULL (pBw, pBw->pBwCmd,
  pBw->pBwApt, pBw->pBwStatus)
  @
**/
EFI_STATUS
EFIAPI
CheckBlockInputParameters (
  IN     DIMM *pDimm
  )
{
  if (pDimm == NULL) {
    NVDIMM_WARN("DIMM not initialized.");
    return EFI_INVALID_PARAMETER;
  }

  if (pDimm->pBw == NULL) {
    NVDIMM_WARN("Block Window not initialized.");
    return EFI_INVALID_PARAMETER;
  }

  if (pDimm->pBw->pBwCmd == NULL) {
    NVDIMM_WARN("Block Window command register not initialized.");
    return EFI_INVALID_PARAMETER;
  }

  if (pDimm->pBw->ppBwApt == NULL) {
    NVDIMM_WARN("Block Window aperture register not initialized.");
    return EFI_INVALID_PARAMETER;
  }

  if (pDimm->pBw->pBwStatus == NULL) {
    NVDIMM_WARN("Block Window status register not initialized.");
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Poll Firmware Command Completion
  Poll the status register of the BW waiting for the status register complete bit to be set.

  @param[in] pDimm - Dimm with block window with submitted command
  @param[in] Timeout The timeout, in 100ns units, to use for the execution of the BW command.
             A Timeout value of 0 means that this function will wait infinitely for the command to execute.
             If Timeout is greater than zero, then this function will return EFI_TIMEOUT if the time required to execute
             the receive data command is greater than Timeout.
  @param[out] pStatus returned Status from BW status register

  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR FW error received
  @retval EFI_TIMEOUT A timeout occurred while waiting for the command to execute.
**/
EFI_STATUS
EFIAPI
CheckBwCmdTimeout (
  IN     DIMM *pDimm,
  IN     UINT64 Timeout,
     OUT UINT32 *pStatus
  )
{
  UINT32 ReadStatus = 0;
  EFI_STATUS ReturnCode = EFI_DEVICE_ERROR;

  NVDIMM_ENTRY();
  ReturnCode = CheckBlockInputParameters(pDimm);
  if (EFI_ERROR(ReturnCode)) {
    return ReturnCode;
  }
  if (pStatus == NULL) {
    NVDIMM_DBG("Invalid parameter.");
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  /** Get Command Status, save it to local variable **/
  ReadStatus = *(pDimm->pBw->pBwStatus);
  if (ReadStatus & BW_PENDING_MASK) {
    NVDIMM_WARN("BW register status has pending bit lit up.");
    /**
       @todo: Waiting for Cspec that covers this case.

       gBS->RestoreTPL(TPL_APPLICATION);
       gBS->CreateEvent(EVT_TIMER, 0, NULL, NULL, TimeoutEvent);
       gBS->SetTimer(TimeoutEvent, TimerRelative, Timeout);
       gBS->WaitForEvent(1, TimeoutEvent, &TimeoutEventTmpVar);
       gBS->RaiseTPL(TPL_CALLBACK);
    **/
  }
  *pStatus = ReadStatus;
  ReturnCode = EFI_SUCCESS;
Finish:
  NVDIMM_EXIT_I(ReturnCode);
  return ReturnCode;
}

/**
  Get command status from command status register

  @param[in] pDimm - pointer to DIMM with Block Window

  @retval EFI_SUCCESS
  @retval EFI_INVALID_PARAMETER if some pointer is NULL
  @retval other - error code matching to status register
**/
EFI_STATUS
EFIAPI
GetBwCommandStatus (
  IN     DIMM *pDimm
  )
{
  UINT32 Status = 0;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  ReturnCode = CheckBlockInputParameters(pDimm);
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }

  ReturnCode = CheckBwCmdTimeout(pDimm, PT_TIMEOUT_INTERVAL, &Status);
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }

  /** DPA Address specified in the BW Address Register is not a valid address for the NVDIMM **/
  if (Status & BW_INVALID_ADRESS_MASK) {
    NVDIMM_WARN("DPA Address specified in the BW Address Register is not a valid address for the NVDIMM");
    ReturnCode = EFI_DEVICE_ERROR;
    goto Finish;
  }

  /** An uncorrectable error occurred upon NVDIMM access to the given BW Address **/
  if (Status & BW_ACCESS_ERROR) {
    NVDIMM_WARN("An uncorrectable error occurred upon NVDIMM access to the given BW Address");
    ReturnCode = EFI_DEVICE_ERROR;
    goto Finish;
  }

  /** BW request attempts to access a locked Persistent Memory region of the NVDIMM **/
  if (Status & BW_PM_ACCESS_ERROR) {
    NVDIMM_WARN("BW request attempted to access a locked Persistent Memory region of the NVDIMM");
    ReturnCode = EFI_ACCESS_DENIED;
    goto Finish;
  }

  /** BW request attempts to access a locked or disabled Block Window region of the NVDIMM **/
  if (Status & BW_REGION_ACCESS_ERROR) {
    NVDIMM_WARN("BW request attempted to access a locked or disabled Block Window region of the NVDIMM");
    ReturnCode = EFI_ACCESS_DENIED;
    goto Finish;
  }

Finish:
  NVDIMM_EXIT_I(ReturnCode);
  return ReturnCode;
}

/**
  Read back the BW address register and that would ensure the programming has completed.
**/
VOID
BlockWindowProgrammingDelay(volatile UINT64 *pBlockWindowCmdRegAddress)
{
  volatile UINT64 DummyBuffer = 0;

  /** Read back the BW address register and that would ensure the programming has completed. **/
  DummyBuffer = *pBlockWindowCmdRegAddress;
  if (DummyBuffer == 0) {
    NVDIMM_DBG("BW address register is zero");
  }
}

/**
  Get error logs for given dimm parse it and save in common error log structure

  @param[in] pDimm - pointer to DIMM to get errors
  @param[in] ThermalError - is thermal error (if not it is media error)
  @param[in] HighLevel - high level if true, low level otherwise
  @param[in] SequenceNumber - sequence number of error to fetch in queue
  @param[in] MaxErrorsToSave - max number of new error entries that can be saved in output array
  @param[out] pErrorsFetched - number of new error entries saved in output array
  @param[out] pErrorLogs - output array of errors

  @retval EFI_SUCCESS
  @retval EFI_INVALID_PARAMETER if some pointer is NULL
  @retval other - error code matching to status register
**/
EFI_STATUS
GetAndParseFwErrorLogForDimm(
  IN     DIMM *pDimm,
  IN     CONST BOOLEAN ThermalError,
  IN     CONST BOOLEAN HighLevel,
  IN     CONST UINT16 SequenceNumber,
  IN     UINT32 MaxErrorsToSave,
     OUT UINT32 *pErrorsFetched,
     OUT ERROR_LOG_INFO *pErrorLogs
  )
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  UINT32 Index = 0;
  PT_OUTPUT_PAYLOAD_GET_ERROR_LOG_THERMAL_ENTRY *pThermalLogEntry = NULL;
  PT_OUTPUT_PAYLOAD_GET_ERROR_LOG_MEDIA_ENTRY *pMediaLogEntry = NULL;
  MEDIA_ERROR_LOG_INFO *pMediaErrorInfo = NULL;
  THERMAL_ERROR_LOG_INFO *pThermalErrorInfo = NULL;
  PT_INPUT_PAYLOAD_GET_ERROR_LOG InputPayload;
  VOID *pLargeOutputPayload = NULL;
  PT_OUTPUT_PAYLOAD_GET_ERROR_LOG OutPayloadGetErrorLog;
  LOG_INFO_DATA_RETURN OutPayloadGetErrorLogInfoData;
  UINT16 ReturnCount = 0;
  TEMPERATURE Temperature;
  BOOLEAN LargePayloadAvailable = FALSE;

  ZeroMem(&InputPayload, sizeof(InputPayload));
  ZeroMem(&OutPayloadGetErrorLog, sizeof(OutPayloadGetErrorLog));
  ZeroMem(&OutPayloadGetErrorLogInfoData, sizeof(OutPayloadGetErrorLogInfoData));
  ZeroMem(&Temperature, sizeof(Temperature));

  if (pDimm == NULL || pErrorsFetched == NULL || pErrorLogs == NULL) {
    goto Finish;
  }

  pLargeOutputPayload = AllocateZeroPool(OUT_MB_SIZE);
  if (pLargeOutputPayload == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  InputPayload.LogParameters.Separated.LogLevel = HighLevel ? ErrorLogHighPriority : ErrorLogLowPriority;
  InputPayload.LogParameters.Separated.LogType = ThermalError ? ErrorLogTypeThermal : ErrorLogTypeMedia;
  InputPayload.SequenceNumber = SequenceNumber;
  InputPayload.RequestCount = (MaxErrorsToSave >= MAX_UINT16) ? MAX_UINT16 : (UINT16) MaxErrorsToSave;

  CHECK_RESULT(IsLargePayloadAvailable(pDimm, &LargePayloadAvailable), Finish);
  if (!LargePayloadAvailable) {
    InputPayload.LogParameters.Separated.LogInfo = ErrorLogInfoData;

    ReturnCode = FwCmdGetErrorLog(pDimm, &InputPayload, &OutPayloadGetErrorLogInfoData, sizeof(OutPayloadGetErrorLogInfoData),
      pLargeOutputPayload, 0);

    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_WARN("Failed to fetch error log for Dimm %x\n", pDimm->DeviceHandle.AsUint32);
      ReturnCode = EFI_DEVICE_ERROR;
      goto Finish;
    }

    UINT16 PayloadsProcessed = 0;
    InputPayload.LogParameters.Separated.LogInfo = ErrorLogInfoEntries;
    InputPayload.LogParameters.Separated.LogEntriesPayloadReturn = ErrorLogSmallPayload;
    InputPayload.SequenceNumber = OutPayloadGetErrorLogInfoData.OldestSequenceNum;
    UINT16 LogEntrySize = ThermalError ? sizeof(PT_OUTPUT_PAYLOAD_GET_ERROR_LOG_THERMAL_ENTRY) : sizeof(PT_OUTPUT_PAYLOAD_GET_ERROR_LOG_MEDIA_ENTRY);
    UINT16 SmallPayloadRawSize = 0;
    UINT64 LargeOutputOffset = (UINT64)pLargeOutputPayload;

    while (ReturnCount < OutPayloadGetErrorLogInfoData.MaxLogEntries) {
      ReturnCode = FwCmdGetErrorLog(pDimm, &InputPayload, &OutPayloadGetErrorLog, sizeof(OutPayloadGetErrorLog),
        pLargeOutputPayload, 0);

      if (EFI_ERROR(ReturnCode)) {
        NVDIMM_WARN("Failed to fetch error log for Dimm %x\n", pDimm->DeviceHandle.AsUint32);
        ReturnCode = EFI_DEVICE_ERROR;
        goto Finish;
      }

      if (0 == OutPayloadGetErrorLog.ReturnCount) {
        break;
      }

      SmallPayloadRawSize = (LogEntrySize * OutPayloadGetErrorLog.ReturnCount);
      CopyMem_S((VOID *)LargeOutputOffset,
        SmallPayloadRawSize,
        OutPayloadGetErrorLog.LogEntries,
        SmallPayloadRawSize);

      if (OUT_MB_SIZE >= LargeOutputOffset + SmallPayloadRawSize - (UINT64)pLargeOutputPayload) {
        LargeOutputOffset += SmallPayloadRawSize;
      }
      else {
        NVDIMM_WARN("Buffer limit reached while fetching error log for Dimm %x\n", pDimm->DeviceHandle.AsUint32);
        break;
      }

      InputPayload.SequenceNumber += OutPayloadGetErrorLog.ReturnCount;
      ReturnCount += OutPayloadGetErrorLog.ReturnCount;
      PayloadsProcessed++;
    }
  }
  else {
    InputPayload.LogParameters.Separated.LogEntriesPayloadReturn = ErrorLogLargePayload;

    ReturnCode = FwCmdGetErrorLog(pDimm, &InputPayload, &OutPayloadGetErrorLog, sizeof(OutPayloadGetErrorLog),
      pLargeOutputPayload, OUT_MB_SIZE);

    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_WARN("Failed to fetch error log for Dimm %x\n", pDimm->DeviceHandle.AsUint32);
      ReturnCode = EFI_DEVICE_ERROR;
      goto Finish;
    }

    ReturnCount = OutPayloadGetErrorLog.ReturnCount;
  }

  if (ReturnCount > 0) {
    if (ThermalError) {
      pThermalLogEntry = (PT_OUTPUT_PAYLOAD_GET_ERROR_LOG_THERMAL_ENTRY *) pLargeOutputPayload;
    } else {
      pMediaLogEntry = (PT_OUTPUT_PAYLOAD_GET_ERROR_LOG_MEDIA_ENTRY *) pLargeOutputPayload;
    }

    for (Index = 0; Index < ReturnCount; ++Index) {
      pErrorLogs[Index].DimmID = pDimm->DimmID;

      if (ThermalError) {
        pErrorLogs[Index].ErrorType = THERMAL_ERROR;
        pErrorLogs[Index].SystemTimestamp = pThermalLogEntry->SystemTimestamp;

        pThermalErrorInfo = (THERMAL_ERROR_LOG_INFO *) &pErrorLogs[Index].OutputData;

        Temperature.Separated.Sign = (UINT16) pThermalLogEntry->HostReportedTempData.Separated.Sign;
        Temperature.Separated.TemperatureValue = (UINT16) pThermalLogEntry->HostReportedTempData.Separated.Temperature;
        pThermalErrorInfo->Temperature = TransformFwTempToRealValue(Temperature);

        pThermalErrorInfo->Reported = (UINT8)pThermalLogEntry->HostReportedTempData.Separated.Reported;
        pThermalErrorInfo->Type = (UINT8)pThermalLogEntry->HostReportedTempData.Separated.Type;
        pThermalErrorInfo->SequenceNum = pThermalLogEntry->SequenceNum;

        pThermalLogEntry++;
      } else {
        pErrorLogs[Index].ErrorType = MEDIA_ERROR;
        pErrorLogs[Index].SystemTimestamp = pMediaLogEntry->SystemTimestamp;

        pMediaErrorInfo = (MEDIA_ERROR_LOG_INFO *) &pErrorLogs[Index].OutputData;
        pMediaErrorInfo->Dpa = pMediaLogEntry->Dpa;
        pMediaErrorInfo->Pda = pMediaLogEntry->Pda;
        pMediaErrorInfo->Range = pMediaLogEntry->Range;
        pMediaErrorInfo->ErrorType = pMediaLogEntry->ErrorType;
        pMediaErrorInfo->PdaValid = pMediaLogEntry->ErrorFlags.Spearated.PdaValid;
        pMediaErrorInfo->DpaValid = pMediaLogEntry->ErrorFlags.Spearated.DpaValid;
        pMediaErrorInfo->Interrupt = pMediaLogEntry->ErrorFlags.Spearated.Interrupt;
        pMediaErrorInfo->Viral = pMediaLogEntry->ErrorFlags.Spearated.Viral;
        pMediaErrorInfo->TransactionType = pMediaLogEntry->TransactionType;
        pMediaErrorInfo->SequenceNum = pMediaLogEntry->SequenceNum;

        pMediaLogEntry++;
      }
    }
  }

  *pErrorsFetched = ReturnCount;

  ReturnCode = EFI_SUCCESS;
Finish:
  FREE_POOL_SAFE(pLargeOutputPayload);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Prepare and send command register command

  @param[in] pDimm: DIMM to read from
  @param[in] Length: Number of bytes to read
  @param[in] Index: Aperture chunk index
  @param[in] Offset: offset from the start of the region this mem type uses
  @param[in] BwCommandCode: Read or write operation.

  @retval EFI_ACCESS_DENIED if BW request attempts to access a locked or disabled BW or PM region
  @retval EFI_DEVICE_ERROR If DIMM DPA address is invalid or uncorrectable access error occurred
  @retval EFI_INVALID_PARAMETER If pDimm, pBuffer or some internal BW pointer is NULL
  @retval EFI_TIMEOUT A timeout occurred while waiting for the command to execute.
  @retval EFI_OUT_OF_RESOURCES in case of failed region allocation
**/
VOID
PrepareAndSendCommandRegisterCmd(
  IN     DIMM *pDimm,
  IN     UINT32 Length,
  IN     UINT16 Index,
  IN     UINT64 Offset,
  IN     BW_COMMAND_CODE BwCommandCode
  )
{
  CONST UINT64 ChunkOffset = Offset + Index * BW_APERTURE_LENGTH;
  CONST UINT32 CacheLinesToTransfer = Length / CACHE_LINE_SIZE;
  UINT64 Command = 0;

  NVDIMM_ENTRY();

  /** Prepare command register command **/
  PrepareBwCommand((UINT64)ChunkOffset, (UINT8)CacheLinesToTransfer, BwCommandCode, &Command);

  /** Send command register command **/
  *pDimm->pBw->pBwCmd = Command;

  if (pDimm->ControlWindowLatch) {
    // Assure that proper delay between accesses was made
    BlockWindowProgrammingDelay(pDimm->pBw->pBwCmd);
  }

  DimmWPQFlush(pDimm);
  AsmSfence();

  NVDIMM_EXIT();
  return;
}
#ifndef OS_BUILD
/**
  Read a number of bytes from a DIMM

  @param[in] pDimm: DIMM to read from
  @param[in] Offset: offset from the start of the region this mem type uses
  @param[in] Nbytes: Number of bytes to read
  @param[out] pBuffer: Buffer to place bytes into
  @retval EFI_ACCESS_DENIED if BW request attempts to access a locked or disabled BW or PM region
  @retval EFI_DEVICE_ERROR If DIMM DPA address is invalid or uncorrectable access error occurred
  @retval EFI_INVALID_PARAMETER If pDimm, pBuffer or some internal BW pointer is NULL
  @retval EFI_TIMEOUT A timeout occurred while waiting for the command to execute.
  @retval EFI_OUT_OF_RESOURCES in case of failed region allocation
**/
EFI_STATUS
EFIAPI
ApertureRead (
  IN     DIMM *pDimm,
  IN     UINT64 Offset,
  IN     UINT64 Nbytes,
     OUT CHAR8 *pBuffer
  )
{
  /** Prepare control register command **/
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  UINT16 Index = 0;
  UINT32 Length = BW_APERTURE_LENGTH;
  UINT64 NotAlignedNbytes = 0;
  CHAR8 *pDstChunk = NULL;
  CHAR8 *pAlignChunk = NULL;
  UINT64 NoApertureChunks = 0;
  UINTN AlignChunkSz = 0;
  NVDIMM_ENTRY();

  if (pBuffer == NULL) {
    NVDIMM_DBG("Output buffer not initialized.");
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  ReturnCode = CheckBlockInputParameters(pDimm);
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }

  /**
    For read buffer that size is not a multiple of cache line size there is need to
    create temporary buffer for last chunk, aligned to cache line size.
  **/
  if (Nbytes % CACHE_LINE_SIZE != 0) {
    NotAlignedNbytes = Nbytes;
    Nbytes += CACHE_LINE_SIZE - Nbytes % CACHE_LINE_SIZE;
    AlignChunkSz = Nbytes % BW_APERTURE_LENGTH;
    pAlignChunk = AllocatePool(AlignChunkSz);
    if (pAlignChunk == NULL) {
      ReturnCode = EFI_OUT_OF_RESOURCES;
      goto Finish;
    }
  }

  NoApertureChunks = (Nbytes / BW_APERTURE_LENGTH);

  for (Index = 0; Index <= NoApertureChunks; ++Index) {
    if (Index == NoApertureChunks) {
      // Last chunk, calculate lasted bytes
      Length = Nbytes % BW_APERTURE_LENGTH;
    }

    PrepareAndSendCommandRegisterCmd(pDimm, Length, Index, Offset, BwRead);

    if (pDimm->FlushRequired) {
      FlushInterleavedBuffer((VOID **) pDimm->pBw->ppBwApt, pDimm->pBw->LineSizeOfApt, Length);
    }

    /** Copy buffer from aperture **/
    ReturnCode = GetBwCommandStatus(pDimm);

    if (EFI_ERROR(ReturnCode)) {
      goto Finish;
    }

    DimmWPQFlush(pDimm);
    AsmSfence();
    pDstChunk = pBuffer + Index * BW_APERTURE_LENGTH;
    // For last chunk use temporary, aligned buffer
    if (Index == NoApertureChunks && NotAlignedNbytes != 0) {
      ReadFromInterleavedBuffer((VOID *)pAlignChunk, AlignChunkSz,(VOID **) pDimm->pBw->ppBwApt, pDimm->pBw->LineSizeOfApt, Length);
      CopyMem(pDstChunk, pAlignChunk, NotAlignedNbytes % BW_APERTURE_LENGTH);
    } else {
      ReadFromInterleavedBuffer((VOID *)pDstChunk, Nbytes, (VOID **) pDimm->pBw->ppBwApt, pDimm->pBw->LineSizeOfApt, Length);
    }
  }
Finish:
  FREE_POOL_SAFE(pAlignChunk);
  NVDIMM_EXIT_I(ReturnCode);
  return ReturnCode;
}

/**
  Write a number of bytes to a DIMM

  @param[out] pDimm: DIMM to write to
  @param[in] Offset: offset from the start of the region this mem type uses
  @param[in] Nbytes: Number of bytes to write (less or equal to buffer size).
  @param[in] pBuffer: Buffer containing data to write

  @retval EFI_ACCESS_DENIED if BW request attempts to access a locked or disabled BW or PM region
  @retval EFI_DEVICE_ERROR If DIMM DPA address is invalid or uncorrectable access error occurred
  @retval EFI_INVALID_PARAMETER If pDimm, pBuffer or some internal BW pointer is NULL
  @retval EFI_TIMEOUT A timeout occurred while waiting for the command to execute.
  @retval EFI_OUT_OF_RESOURCES in case of failed region allocation
**/
EFI_STATUS
EFIAPI
ApertureWrite (
     OUT DIMM *pDimm,
  IN     UINT64 Offset,
  IN     UINT64 Nbytes,
  IN     CHAR8 *pBuffer
  )
{
  UINT16 Index = 0;
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  UINT16 Length = BW_APERTURE_LENGTH;
  UINT64 NotAlignedNbytes = 0;
  UINT64 NoApertureChunks = 0;
  CHAR8 *pAlignChunk = NULL;

  NVDIMM_ENTRY();

  if (pBuffer == NULL) {
    NVDIMM_DBG("Input buffer not initialized.");
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  ReturnCode = CheckBlockInputParameters(pDimm);
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }

  /**
    For write buffer that size is not a multiple of cache line size there is need to
    create temporary buffer for last chunk, aligned to cache line size.
    Additional bytes have to be zeroed.
  **/
  if (Nbytes % CACHE_LINE_SIZE != 0) {
    NotAlignedNbytes = Nbytes;
    Nbytes += CACHE_LINE_SIZE - Nbytes % CACHE_LINE_SIZE;
    pAlignChunk = AllocateZeroPool(Length);
    if (pAlignChunk == NULL) {
      ReturnCode = EFI_OUT_OF_RESOURCES;
      goto Finish;
    }
  }

  NoApertureChunks = (Nbytes / BW_APERTURE_LENGTH);

  for (Index = 0; Index <= NoApertureChunks; ++Index) {
    if (Index == NoApertureChunks) {
      // Last chunk, calculate lasted bytes
      Length = Nbytes % BW_APERTURE_LENGTH;
    }

    PrepareAndSendCommandRegisterCmd(pDimm, Length, Index, Offset, BwWrite);

    /** Copy buffer to aperture **/
    if (Index == NoApertureChunks && NotAlignedNbytes != 0) {
      CopyMem(pAlignChunk, pBuffer + Index * BW_APERTURE_LENGTH, NotAlignedNbytes % BW_APERTURE_LENGTH);
      WriteToInterleavedBuffer((VOID *)pAlignChunk, (VOID **) pDimm->pBw->ppBwApt, pDimm->pBw->LineSizeOfApt, Length);
    } else {
      WriteToInterleavedBuffer((VOID *)(pBuffer + Index * BW_APERTURE_LENGTH), (VOID **) pDimm->pBw->ppBwApt,
        pDimm->pBw->LineSizeOfApt, Length);
    }

    FlushInterleavedBuffer((VOID **) pDimm->pBw->ppBwApt, pDimm->pBw->LineSizeOfApt, Length);
    DimmWPQFlush(pDimm);
    AsmSfence();

#ifdef WA_MEDIA_WRITES_DELAY
    gBS->Stall(WA_MEDIA_WRITES_DELAY);
#endif

    ReturnCode = GetBwCommandStatus(pDimm);
    if (EFI_ERROR(ReturnCode)) {
      goto Finish;
    }
  }

Finish:
  FREE_POOL_SAFE(pAlignChunk);
  NVDIMM_EXIT_I(ReturnCode);
  return ReturnCode;
}
#else
/**
Read a number of bytes from a DIMM

@param[in] pDimm: DIMM to read from
@param[in] Offset: offset from the start of the region this mem type uses
@param[in] Nbytes: Number of bytes to read
@param[out] pBuffer: Buffer to place bytes into

@retval EFI_ACCESS_DENIED if BW request attempts to access a locked or disabled BW or PM region
@retval EFI_DEVICE_ERROR If DIMM DPA address is invalid or uncorrectable access error occurred
@retval EFI_INVALID_PARAMETER If pDimm, pBuffer or some internal BW pointer is NULL
@retval EFI_TIMEOUT A timeout occurred while waiting for the command to execute.
@retval EFI_OUT_OF_RESOURCES in case of failed region allocation
**/
EFI_STATUS
EFIAPI
ApertureRead(
  IN     DIMM *pDimm,
  IN     UINT64 Offset,
  IN     UINT64 Nbytes,
  OUT CHAR8 *pBuffer
)
{
  return EFI_UNSUPPORTED;
}

/**
Write a number of bytes to a DIMM

@param[out] pDimm: DIMM to write to
@param[in] Offset: offset from the start of the region this mem type uses
@param[in] Nbytes: Number of bytes to write (less or equal to buffer size).
@param[in] pBuffer: Buffer containing data to write

@retval EFI_ACCESS_DENIED if BW request attempts to access a locked or disabled BW or PM region
@retval EFI_DEVICE_ERROR If DIMM DPA address is invalid or uncorrectable access error occurred
@retval EFI_INVALID_PARAMETER If pDimm, pBuffer or some internal BW pointer is NULL
@retval EFI_TIMEOUT A timeout occurred while waiting for the command to execute.
@retval EFI_OUT_OF_RESOURCES in case of failed region allocation
**/
EFI_STATUS
EFIAPI
ApertureWrite(
  OUT DIMM *pDimm,
  IN     UINT64 Offset,
  IN     UINT64 Nbytes,
  IN     CHAR8 *pBuffer
)
{
  return EFI_UNSUPPORTED;
}


#endif //OS_BUILD
/**
  Create DIMM
  Perform all functions needed for DIMM initialization this includes:
  setting up mailbox structure
  retrieving and recording security status
  retrieving and recording the FW version
  retrieving and recording partition information
  setting up block windows

  @param[in] pNewDimm: input dimm structure to populate
  @param[in] pFitHead: fully populated NVM Firmware Interface Table
  @param[in] pPmttHead: fully populated Platform Memory Topology Table
  @param[in] Pid: SMBIOS Dimm ID of the DIMM to create

  @retval EFI_SUCCESS          - Success
  @retval EFI_OUT_OF_RESOURCES - AllocateZeroPool failure
  @retval EFI_DEVICE_ERROR     - Other errors
**/
EFI_STATUS
InitializeDimm (
  IN     DIMM *pNewDimm,
  IN     ParsedFitHeader *pFitHead,
  IN     ParsedPmttHeader *pPmttHead,
  IN     UINT16 Pid
  )
{
  EFI_STATUS ReturnCode = EFI_OUT_OF_RESOURCES;
  EFI_STATUS ReturnCodeInterfaceSelection = EFI_OUT_OF_RESOURCES;
  UINT32 Index = 0;
  InterleaveStruct *pMbITbl = NULL;
  InterleaveStruct *pBwITbl = NULL;
  ControlRegionTbl *pControlRegTbl = NULL;
  FlushHintTbl *pFlushHintTable = NULL;
  PT_ID_DIMM_PAYLOAD *pThrowawayPayload = NULL;
  PT_ID_DIMM_PAYLOAD *pPayload = NULL;
  PT_DIMM_PARTITION_INFO_PAYLOAD *pPartitionInfoPayload = NULL;
  PT_GET_SECURITY_PAYLOAD *pDimmSecurityPayload = NULL;
  ControlRegionTbl *pControlRegTbls[MAX_IFC_NUM];
  UINT32 ControlRegTblsNum = MAX_IFC_NUM;
  UINT32 PcdSize = 0;
  ZeroMem(pControlRegTbls, sizeof(pControlRegTbls));
  EFI_DCPMM_CONFIG2_PROTOCOL *pNvmDimmConfigProtocol = NULL;
  EFI_DCPMM_CONFIG_TRANSPORT_ATTRIBS AttribsOrig;
  EFI_DCPMM_CONFIG_TRANSPORT_ATTRIBS AttribsTemp;
  UINT16 TempBootStatusBitmask = DIMM_BOOT_STATUS_NORMAL;
  NVDIMM_ENTRY();

  // We don't need a mailbox to talk to the dimm
  CHECK_RESULT(GetNvDimmRegionMappingStructureForPid(pFitHead, Pid, NULL, FALSE,
      0, &pNewDimm->pRegionMappingStructure), Finish);

  ReturnCode = GetControlRegionTableForNvDimmRegionTable(pFitHead, pNewDimm->pRegionMappingStructure, &pControlRegTbl);
  if ((EFI_ERROR(ReturnCode) || (pControlRegTbl == NULL))) {
    NVDIMM_WARN("Unable to initialize Intel NVM Dimm. Control Region is missing in NFIT.");
    ReturnCode = EFI_DEVICE_ERROR;
    goto Finish;
  }

  /**
    If we fail to get the Flush Hint Table, we ignore it and assume WPQ flush is not required
  **/
  ReturnCode = GetFlushHintTableForNvDimmRegionTable(pFitHead, pNewDimm->pRegionMappingStructure, &pFlushHintTable);
  if (!EFI_ERROR(ReturnCode) && pFlushHintTable != NULL) {
    // Found the Flush Hint Table
    for (Index = 0; Index < pFlushHintTable->NumberOfFlushHintAddresses; Index++) {
      if (pFlushHintTable->FlushHintAddress[Index] != MAX_UINT64_VALUE) { // Entry equal to MAX_UINT64_VALUE is not valid
        pNewDimm->pFlushAddress = (UINT64 *)pFlushHintTable->FlushHintAddress[Index]; // Assign the first valid address
        /**
          The FlushHint Table can have more than one Flush Hint Address but we should need only one
          to execute a WPQ flush.
        **/
        break;
      }
    }
  }

  InitializeDimmFieldsFromAcpiTables(pNewDimm->pRegionMappingStructure, pControlRegTbl, pPmttHead, pNewDimm);

  ReturnCode = GetControlRegionTablesForPID(pFitHead, Pid, pControlRegTbls, &ControlRegTblsNum);
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }

  if (ControlRegTblsNum > MAX_IFC_NUM) {
    NVDIMM_ERR("The ControlRegTblsNum value greater than %d", MAX_IFC_NUM);
    ReturnCode = EFI_BUFFER_TOO_SMALL;
    goto Finish;
  }

  for (Index = 0; Index < ControlRegTblsNum; Index++) {
    pNewDimm->FmtInterfaceCode[Index] = pControlRegTbls[Index]->RegionFormatInterfaceCode;
  }
  pNewDimm->FmtInterfaceCodeNum = ControlRegTblsNum;

  pNewDimm->NvDimmStateFlags = pNewDimm->pRegionMappingStructure->NvDimmStateFlags;

  if (pNewDimm->pRegionMappingStructure->InterleaveStructureIndex != 0) {
    ReturnCode = GetInterleaveTable(pFitHead, pNewDimm->pRegionMappingStructure->InterleaveStructureIndex, &pMbITbl);

    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_WARN("No Interleave Table found for mailbox but the index exists.");
      ReturnCode = EFI_DEVICE_ERROR;
      goto Finish;
    }
  }
  if (pNewDimm->pRegionMappingStructure->SpaRangeDescriptionTableIndex != 0) {
    ReturnCode = GetSpaRangeTable(pFitHead, pNewDimm->pRegionMappingStructure->SpaRangeDescriptionTableIndex, &pNewDimm->pCtrlSpaTbl);

    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_WARN("No spa range table found for mailbox but the index exists.");
      ReturnCode = EFI_DEVICE_ERROR;
      goto Finish;
    }
  }

  if ((SPD_INTEL_VENDOR_ID == pNewDimm->SubsystemVendorId) &&
    IsSubsystemDeviceIdSupported(pNewDimm)) {
    CHECK_RESULT_MALLOC(pThrowawayPayload, AllocateZeroPool(sizeof(*pThrowawayPayload)), Finish);

    // Initialize boot status bitmask
    pNewDimm->BootStatusBitmask = DIMM_BOOT_STATUS_UNKNOWN;

    // ************
    // *Determine what interfaces are accessible*
    //
    // The main reliable way to determine if DDRT/smbus are accessible or not is
    // to try a command over that interface. The only way currently to force the
    // interface is to use the global "-ddrt"/"-smbus" flags. Since the user
    // setting these flags on the CLI conflict with this operation, save off
    // the previous state of these flags so we can force which interface to use
    // and then restore the original interface flags afterwards.
    // This does mean that the flags are not honored for 1-2 commands, but
    // it is a lot simpler than other methods.
    // Note: DDRT large payload accessibility (DIMM_BOOT_STATUS_MEDIA_*)
    // is determined the normal way by reading the BSR directly in
    // PopulateDimmBsrAndBootStatusBitmask() (2nd half of this code).

    // Save off previous state to AttribsOrig
    CHECK_RESULT(OpenNvmDimmProtocol(gNvmDimmConfigProtocolGuid, (VOID **)&pNvmDimmConfigProtocol, NULL), Finish);
    CHECK_RESULT(pNvmDimmConfigProtocol->GetFisTransportAttributes(pNvmDimmConfigProtocol, &AttribsOrig), Finish);

    // Set "-ddrt" and "-spmb" flags
    AttribsTemp.Protocol = FisTransportDdrt;
    AttribsTemp.PayloadSize = FisTransportSizeSmallMb;
    CHECK_RESULT(pNvmDimmConfigProtocol->SetFisTransportAttributes(pNvmDimmConfigProtocol, AttribsTemp), RestoreAttribs);
    // Send identify dimm over ddrt small payload
    ReturnCode = FwCmdIdDimm(pNewDimm, pThrowawayPayload);

    if (EFI_ERROR(ReturnCode)) {
      // If we get an error running over DDRT, then set
      // DDRT_NOT_READY.
      pNewDimm->BootStatusBitmask = DIMM_BOOT_STATUS_DDRT_NOT_READY;

      // We try checking the smbus interface only if DDRT fails.
      // Big performance penalty in OS currently if we use smbus.

      // Set "-smbus" and "-spmb" flags
      AttribsTemp.Protocol = FisTransportSmbus;
      AttribsTemp.PayloadSize = FisTransportSizeSmallMb;
      CHECK_RESULT(pNvmDimmConfigProtocol->SetFisTransportAttributes(pNvmDimmConfigProtocol, AttribsTemp), RestoreAttribs);
      // Try identify dimm over smbus small payload
      ReturnCode = FwCmdIdDimm(pNewDimm, pThrowawayPayload);
      if (EFI_ERROR(ReturnCode)) {
        // If we fail the call over smbus, then the DCPMM is unresponsive
        // and we don't need to do any more initialization.
        // Also set a bit to indicate to PassThru() that we can't send any
        // commands to the DCPMM firmware
        pNewDimm->BootStatusBitmask |= DIMM_BOOT_STATUS_MAILBOX_NOT_READY;
        goto RestoreAttribs;
      }
    }
RestoreAttribs:
    // Save off return code from above section
    ReturnCodeInterfaceSelection = ReturnCode;

    // Restore the previous state of the CLI interface flags
    // Go to Finish if error
    CHECK_RESULT(pNvmDimmConfigProtocol->SetFisTransportAttributes(pNvmDimmConfigProtocol, AttribsOrig), Finish);

    // Populate some more boot status bitmask bits.
    // Ignore return code, as this is an optional step
    CHECK_RESULT_CONTINUE(PopulateDimmBsrAndBootStatusBitmask(pNewDimm,
      &pNewDimm->Bsr, &TempBootStatusBitmask));
    // If we're successful in getting BSR and BootStatusBitmask, OR in the
    // findings into previous findings for BootStatusBitmask bits
    if (EFI_SUCCESS == ReturnCode) {
      pNewDimm->BootStatusBitmask |= TempBootStatusBitmask;
    }

    // Since a BSR bit can affect MAILBOX_NOT_READY, run this after
    // PopulateDimmBsrAndBootStatusBitmask()
    if (pNewDimm->BootStatusBitmask & DIMM_BOOT_STATUS_DDRT_NOT_READY ||
      pNewDimm->BootStatusBitmask & DIMM_BOOT_STATUS_MAILBOX_NOT_READY) {
      // Setting as non-functional is not appropriate for only DDRT down, but
      // needed temporarily to not create new defects until future changes
      // are integrated. (lots of commands currently rely on this field for
      // filtering proper/improper dimms. We'll change this going forward and
      // hopefully remove the NonFunctional field!)
      pNewDimm->NonFunctional = TRUE;
    }

    // If there was an unhandled error in running interface selection, abort initialization
    ReturnCode = ReturnCodeInterfaceSelection;
    CHECK_RETURN_CODE(ReturnCode, Finish);

    // ************

    // Run identify dimm with user specified -ddrt/-smbus options if applicable
    CHECK_RESULT_MALLOC(pPayload, AllocateZeroPool(sizeof(*pPayload)), Finish);
    ReturnCode = FwCmdIdDimm(pNewDimm, pPayload);
    NVDIMM_DBG("IdentifyDimm data:\n");
    NVDIMM_DBG("Raw Capacity (4k multiply): %d\n", pPayload->Rc);
    pNewDimm->FlushRequired = (pPayload->Fswr & BIT0) != 0;
    pNewDimm->ControlWindowLatch = (pPayload->Fswr & BIT1) != 0;

    /** pPayload->ReturnCode in 4KiB multiples **/
    pNewDimm->RawCapacity = (UINT64)pPayload->Rc * (4 * 1024);
    pNewDimm->Manufacturer = pPayload->Mf;
    pNewDimm->SkuInformation = *((SKU_INFORMATION *)&pPayload->DimmSku);
    CopyMem_S(pNewDimm->PartNumber, sizeof(pNewDimm->PartNumber), pPayload->Pn, sizeof(pPayload->Pn));
    pNewDimm->PartNumber[PART_NUMBER_LEN - 1] = '\0';

    NVDIMM_DBG("String length is %d", AsciiStrLen(pPayload->Pn));
    pNewDimm->FwVer = ParseFwVersion(pPayload->Fwr);
    ParseFwApiVersion(pNewDimm, pPayload);

    pNewDimm->ControllerRid = pPayload->Rid;
  }

  if (IsDimmManageable(pNewDimm) && IsDimmInSupportedConfig(pNewDimm)) {
    pPartitionInfoPayload = AllocateZeroPool(sizeof(*pPartitionInfoPayload));
    if (pPartitionInfoPayload == NULL) {
      ReturnCode = EFI_OUT_OF_RESOURCES;
      goto Finish;
    }

    ReturnCode = FwCmdGetDimmPartitionInfo(pNewDimm, pPartitionInfoPayload);
    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_DBG("FW CMD Error: %d", ReturnCode);
      if (ReturnCode == EFI_NO_MEDIA || ReturnCode == EFI_NO_RESPONSE) {
        /** Return success if error from FW is Media Disabled **/
        ReturnCode = EFI_SUCCESS;
      } else {
        goto Finish;
      }
    }

    if (pPartitionInfoPayload != NULL) {
      /** Capacity from Partition Info Payload in 4KiB multiples**/
      pNewDimm->VolatileCapacity = (UINT64) pPartitionInfoPayload->VolatileCapacity * 4096;
      pNewDimm->VolatileStart = (UINT64) pPartitionInfoPayload->VolatileStart;
      pNewDimm->PmCapacity = (UINT64) pPartitionInfoPayload->PersistentCapacity * 4096;
      pNewDimm->PmStart = pPartitionInfoPayload->PersistentStart;
    }

    ReturnCode = GetNvDimmRegionMappingStructureForPid(pFitHead, pNewDimm->DimmID,
      &gSpaRangeBlockDataWindowRegionGuid, FALSE, 0, &pNewDimm->pBlockDataRegionMappingStructure);
    if (EFI_ERROR(ReturnCode) || pNewDimm->pBlockDataRegionMappingStructure == NULL) {
      NVDIMM_WARN("No NVDIMM region table found for block window on dimm: 0x%x.", pNewDimm->DeviceHandle.AsUint32);
      ReturnCode = EFI_SUCCESS;
    } else {
      if (pNewDimm->pBlockDataRegionMappingStructure->SpaRangeDescriptionTableIndex != 0) {
        ReturnCode = GetSpaRangeTable(pFitHead,
            pNewDimm->pBlockDataRegionMappingStructure->SpaRangeDescriptionTableIndex, &pNewDimm->pBlockDataSpaTbl);

        if (EFI_ERROR(ReturnCode)) {
          NVDIMM_WARN("No spa range table found for block aperture but the index exists.");
          ReturnCode = EFI_DEVICE_ERROR;
          goto Finish;
        }
      }
    }

    ReturnCode = FwCmdGetPlatformConfigDataSize(pNewDimm, PCD_OEM_PARTITION_ID, &PcdSize);
    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_DBG("FW CMD Error: %d", ReturnCode);
      if (ReturnCode == EFI_NO_MEDIA || ReturnCode == EFI_NO_RESPONSE) {
        /** Return success if error from FW is Media Disabled **/
        ReturnCode = EFI_SUCCESS;
      } else {
        goto Finish;
      }
    }
    pNewDimm->PcdOemPartitionSize = PcdSize;
    PcdSize = 0;

    ReturnCode = FwCmdGetPlatformConfigDataSize(pNewDimm, PCD_LSA_PARTITION_ID, &PcdSize);
    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_DBG("FW CMD Error: %d", ReturnCode);
      if (ReturnCode == EFI_NO_MEDIA || ReturnCode == EFI_NO_RESPONSE) {
        /** Return success if error from FW is Media Disabled **/
        ReturnCode = EFI_SUCCESS;
      } else {
        goto Finish;
      }
    }
    pNewDimm->PcdLsaPartitionSize = PcdSize;

    pNewDimm->InaccessibleVolatileCapacity = 0;
    pNewDimm->InaccessiblePersistentCapacity = 0;

    pNewDimm->GoalConfigStatus = GOAL_CONFIG_STATUS_NO_GOAL_OR_SUCCESS;

    pDimmSecurityPayload = AllocateZeroPool(sizeof(*pDimmSecurityPayload));
    if (pDimmSecurityPayload == NULL) {
      ReturnCode = EFI_OUT_OF_RESOURCES;
      goto Finish;
    }
    ReturnCode = FwCmdGetSecurityInfo(pNewDimm, pDimmSecurityPayload);
    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_WARN("Failed to get the security state for dimm: 0x%x.", pNewDimm->DeviceHandle.AsUint32);
      /**
        If we can't get the security state, we are still trying to manage the DIMM,
        We assume that the security is disabled and hopefully all of the features will work.
        Otherwise we will get more errors on each feature that we will try to use.
      **/
      ReturnCode = EFI_SUCCESS;
      pDimmSecurityPayload->SecurityStatus.AsUint32 = 0;
    }

    pNewDimm->EncryptionEnabled = (BOOLEAN) pDimmSecurityPayload->SecurityStatus.Separated.SecurityEnabled;

    if (pNewDimm->pBlockDataRegionMappingStructure != NULL && pNewDimm->pBlockDataRegionMappingStructure->InterleaveStructureIndex != 0) {
      ReturnCode = GetInterleaveTable(pFitHead, pNewDimm->pBlockDataRegionMappingStructure->InterleaveStructureIndex, &pBwITbl);

      if (EFI_ERROR(ReturnCode)) {
        NVDIMM_WARN("No Interleave Table found for block window but the index exists.");
        ReturnCode = EFI_DEVICE_ERROR;
        goto Finish;
      }
    }
  }

Finish:
  FREE_POOL_SAFE(pThrowawayPayload);
  FREE_POOL_SAFE(pPayload);
  FREE_POOL_SAFE(pPartitionInfoPayload);
  FREE_POOL_SAFE(pDimmSecurityPayload);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Check if the DIMM containing the specified DIMM ID is
  manageable by our software

  @param[in] UINT16 Dimm ID to check

  @retval BOOLEAN whether or not dimm is manageable
**/
BOOLEAN
IsDimmIdManageable(
  IN     UINT16 DimmID
  )
{
  DIMM *pCurDimm = NULL;
  LIST_ENTRY *pCurDimmNode = NULL;
  LIST_ENTRY *pDimms = &gNvmDimmData->PMEMDev.Dimms;
  BOOLEAN Manageable = FALSE;

  for (pCurDimmNode = GetFirstNode(pDimms);
      !IsNull(pDimms, pCurDimmNode);
      pCurDimmNode = GetNextNode(pDimms, pCurDimmNode)) {
    pCurDimm = DIMM_FROM_NODE(pCurDimmNode);
    if (DimmID == pCurDimm->DimmID) {
      Manageable = IsDimmManageable(pCurDimm);
        break;
    }
  }

  return Manageable;
}

/**
  Free dimm
  Free the memory resources associated with a DIMM

  @param[out] pDimm - the DIMM to free
**/
VOID
FreeDimm(
     OUT DIMM *pDimm
  )
{
  NVDIMM_ENTRY();
  if (pDimm == NULL) {
    return;
  }
  FreeBlockWindow(pDimm->pBw);
  FREE_POOL_SAFE(pDimm);
  NVDIMM_EXIT();
}

/**
  Remove a DIMM
  Perform all functions needed for when a DIMM is to be removed from the
  platform.
  This may include things like removing memory from regions, deallocating
  mailboxes, deallocating block windows, and verifying that this DIMM
  is able to be removed from the system.

  @param[out] pDimm: DIMM to remove
  @param[in] Force: If true, forcefully remove the DIMM from the system(Very destructive)

  @retval EFI_SUCCESS: Success
  @retval EFI_INVALID_PARAMETER: No DIMM structure to remove
  @retval EFI_NOT_READY: if unable to remove DIMM from inventory gracefully
**/
EFI_STATUS
RemoveDimm(
     OUT DIMM *pDimm,
  IN     INT32 Force
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();
  if (pDimm == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
  } else {
    /**
      test if DIMM is used in any volume
      if yes and not force then return EFI_NOT_READY
      if yes and force then call delete volume for
      each volume found to have part of the dimm
    **/

    /**
      find regions that contain the dimm
      For interleaved regions break them up
      For non pm regions delete the region
    **/
    FreeDimm(pDimm);
  }

  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Flushes date from the iMC buffers to the DIMM.

  The flushing is made by writing to the Flush Hint addresses.
  If there is no Flush Hint Table for the provided DIMM,
  The assumption is made that WPQ flush is not supported and not required.

  @param[in] pDimm: DIMM to flush the data into.
**/
VOID
DimmWPQFlush(
  IN     DIMM *pDimm
  )
{
  NVDIMM_ENTRY();

  if (pDimm != NULL && pDimm->pFlushAddress != NULL) {
    *pDimm->pFlushAddress = 1; // Write any data for the flush hint address to perform the flush
  }

  NVDIMM_EXIT();
}

/**
  Copy data from an interleaved buffer to a regular buffer.

  Both buffers have to be equal or greater than NumOfBytes.

  @param[out] pRegularBuffer output regular buffer
  @param[in] RegularBufferSz size of the RegualrBuffer
  @param[in] ppInterleavedBuffer input interleaved buffer
  @param[in] LineSize line size of interleaved buffer
  @param[in] NumOfBytes number of bytes to copy
**/
VOID
ReadFromInterleavedBuffer(
     OUT VOID *pRegularBuffer,
  IN     UINTN RegularBufferSz,
  IN     VOID **ppInterleavedBuffer,
  IN     UINT32 LineSize,
  IN     UINT32 NumOfBytes
  )
{
  UINT8 *pTo = NULL;
  UINT32 NumOfSegments = 0;
  UINT32 Remain = 0;
  UINT32 Index = 0;


  NVDIMM_ENTRY();

  if (pRegularBuffer == NULL || ppInterleavedBuffer == NULL || LineSize == 0) {
    NVDIMM_DBG("Invalid input parameter.");
    goto Finish;
  }

  NumOfSegments = NumOfBytes / LineSize;
  Remain = NumOfBytes % LineSize;

  pTo = pRegularBuffer;
  for (Index = 0; Index < NumOfSegments; Index++) {
    CopyMem_S(pTo, RegularBufferSz, ppInterleavedBuffer[Index], LineSize);
    pTo += LineSize;
    RegularBufferSz -= LineSize;
  }

  if (Remain > 0) {
    CopyMem_S(pTo, RegularBufferSz, ppInterleavedBuffer[Index], Remain);
  }

Finish:
  NVDIMM_EXIT();
}

/**
  Flush data from an interleaved buffer.

  The InterleavedBuffer needs to be at least NumOfBytes.

  @param[in] ppInterleavedBuffer input interleaved buffer
  @param[in] LineSize line size of interleaved buffer
  @param[in] NumOfBytes number of bytes to copy
**/
VOID
FlushInterleavedBuffer(
  IN     VOID **ppInterleavedBuffer,
  IN     UINT32 LineSize,
  IN     UINT32 NumOfBytes
  )
{
  UINT32 NumOfSegments = 0;
  UINT32 Remain = 0;
  UINT32 Index = 0;
  UINT32 Index2 = 0;

  NVDIMM_ENTRY();

  if (ppInterleavedBuffer == NULL || LineSize == 0) {
    NVDIMM_DBG("Incorrect input parameter.");
    goto Finish;
  }

  if (gClFlush == NULL) {
    NVDIMM_WARN("The CPU commands were not initialized.");
    goto Finish;
  }

  NumOfSegments = NumOfBytes / LineSize;
  Remain = NumOfBytes % LineSize;

  for (Index = 0; Index < NumOfSegments; Index++) {
    for (Index2 = 0; Index2 < ROUNDUP(LineSize, CACHE_LINE_SIZE) / CACHE_LINE_SIZE; Index2++) {
      gClFlush(((UINT8*)ppInterleavedBuffer[Index]) + (Index2 * CACHE_LINE_SIZE));
    }
  }

  if (Remain > 0) {
    for (Index2 = 0; Index2 < ROUNDUP(Remain, CACHE_LINE_SIZE) / CACHE_LINE_SIZE; Index2++) {
      gClFlush(((UINT8*)ppInterleavedBuffer[Index]) + (Index2 * CACHE_LINE_SIZE));
    }
  }

Finish:
  NVDIMM_EXIT();
}

/**
  Sets the memory of the given buffer to a particular value
  This function does a 8 byte copy and falls back to 1 byte if required.

  @param[in] Length The length of bytes of the input buffer
  @param[in] Value The value of the buffer locations that need to be set to
  @param [in out] Buffer The input buffer
**/
STATIC
VOID *
SetMem_8 (
  IN OUT VOID  *Buffer,
  IN     UINTN Length,
  IN     UINT8 Value
  )
{
  volatile UINT8 *Pointer8 = NULL;
  volatile UINT64 *Pointer64 = NULL;
  UINT32 Value32;
  UINT64 Value64;

  if ((((UINTN)Buffer & 0x7) == 0) && (Length >= 8)) {
    Value32 = (Value << 24) | (Value << 16) | (Value << 8) | Value;
    Value64 = LShiftU64 (Value32, 32) | Value32;

    Pointer64 = (UINT64*) Buffer;
    while (Length >= 8) {
      *(Pointer64++) = Value64;
      Length -= 8;
    }
    Pointer8 = (UINT8*) Pointer64;
  } else {
    Pointer8 = (UINT8*) Buffer;
  }

  while (Length-- > 0) {
    *(Pointer8++) = Value;
  }

  return Buffer;
}

/**
  Copies 'Length' no of bytes from source buffer into destination buffer
  The function attempts to perform an 8 byte copy and falls back to 1 byte copies if required

  @param[in] SourceBuffer Source address
  @param[in] Length The length in no of bytes
  @param[out] DestinationBuffer Destination address
**/
VOID *
CopyMem_8 (
  IN OUT VOID      *DestinationBuffer,
  IN     CONST VOID *SourceBuffer,
  IN     UINTN      Length
  )
{
  volatile UINT64 *Destination64 = NULL;
  volatile UINT64 *Source64 = NULL;
  UINT32 Alignment = 0;

  if (((((UINTN)DestinationBuffer) & 0x7) == 0) && ((((UINTN)SourceBuffer) & 0x7) == 0) && (Length >= 8)) {
    if (SourceBuffer > DestinationBuffer) {
      Destination64 = (UINT64 *)DestinationBuffer;
      Source64 = (UINT64 *)SourceBuffer;
      // Copy the bytes first using 8 byte copy
      while (Length >= 8) {
        *(Destination64++) = *(Source64++);
        Length -= 8;
      }

      // Copy the remaining bytes using a 1 byte copy
      if (Length > 0) {
        return CopyMem_S((UINT8 *)Destination64, Length, (UINT8 *)Source64, Length);
      }
    } else if (SourceBuffer < DestinationBuffer) {
      Destination64 = (UINT64*)((UINTN)DestinationBuffer + Length);
      Source64 = (UINT64*)((UINTN)SourceBuffer + Length);

      Alignment = Length & 0x7;
      if (Alignment != 0) {
        // Copy the unaligned bytes using a byte copy
        CopyMem_S((UINT8 *)Destination64, Alignment, (UINT8 *)Source64, Alignment);
      }
      Length -= Alignment;

      // Copy the remaining bytes using 8 byte copy as it is now a multiple of 8.
      while (Length > 0) {
        *(--Destination64) = *(--Source64);
        Length -= 8;
      }
    }
  } else {
    // Source or destination address are not aligned OR the length of bytes is less than 8
    // Let's fall back to 1 byte memcopy
    return CopyMem_S(DestinationBuffer, Length, SourceBuffer, Length);
  }

  return DestinationBuffer;
}

/**
  Copy data from a regular buffer to an interleaved buffer.

  Both buffers have to be equal or greater than NumOfBytes.

  @param[in]  pRegularBuffer       input regular buffer
  @param[out] ppInterleavedBuffer  output interleaved buffer
  @param[in]  LineSize             line size of interleaved buffer
  @param[in]  NumOfBytes           number of bytes to copy
**/
VOID
WriteToInterleavedBuffer(
  IN     VOID *pRegularBuffer,
     OUT VOID **ppInterleavedBuffer,
  IN     UINT32 LineSize,
  IN     UINT32 NumOfBytes
  )
{
  UINT8 *pFrom = NULL;
  UINT32 NumOfSegments = 0;
  UINT32 Remain = 0;
  UINT32 Index = 0;

  NVDIMM_ENTRY();

  if (pRegularBuffer == NULL || ppInterleavedBuffer == NULL || LineSize == 0) {
    NVDIMM_DBG("Invalid input parameter.");
    return;
  }

  NumOfSegments = NumOfBytes / LineSize;
  Remain = NumOfBytes % LineSize;

  pFrom = pRegularBuffer;
  for (Index = 0; Index < NumOfSegments; Index++) {
    CopyMem_8(ppInterleavedBuffer[Index], pFrom, LineSize);
    pFrom += LineSize;
  }

  if (Remain > 0) {
    CopyMem_8(ppInterleavedBuffer[Index], pFrom, Remain);
  }

  NVDIMM_EXIT();
}

/**
  Clear a part or whole of interleaved buffer.

  @param[out] ppInterleavedBuffer  interleaved buffer to clear
  @param[in]  LineSize             line size of interleaved buffer
  @param[in]  NumOfBytes           number of bytes to clear
**/
VOID
ClearInterleavedBuffer(
     OUT VOID **ppInterleavedBuffer,
  IN     UINT32 LineSize,
  IN     UINT32 NumOfBytes
  )
{
  UINT32 Index = 0;
  UINT32 NumOfSegments = 0;
  UINT32 Remain = 0;

  NVDIMM_ENTRY();

  if (ppInterleavedBuffer == NULL || LineSize == 0) {
    NVDIMM_DBG("Invalid input parameter.");
    return;
  }

  NumOfSegments = NumOfBytes / LineSize;
  Remain = NumOfBytes % LineSize;

  for (Index = 0; Index < NumOfSegments; Index++) {
    SetMem_8(ppInterleavedBuffer[Index], LineSize, 0);
  }

  if (Remain > 0) {
    SetMem_8(ppInterleavedBuffer[Index], Remain, 0);
  }

  NVDIMM_EXIT();
}

STATIC
UINT16
GetLogEntriesCount(
  IN     LOG_INFO_DATA_RETURN *pLogInfoDataReturn
  )
{
  INT32 Tmp = 0;
  UINT16 Result = 0;

  Tmp = (INT32) pLogInfoDataReturn->CurrentSequenceNum -
      (INT32) pLogInfoDataReturn->OldestSequenceNum;

  if (Tmp > 0) {
    Result = (UINT16) Tmp + 1;
  } else if (Tmp < 0) {
    Result = (UINT16) (Tmp + pLogInfoDataReturn->MaxLogEntries + 1);
  } else {
    Result = 0;
  }

  return Result;
}

/**
  Get count of media and/or thermal errors on given DIMM

  @param[in] pDimm - pointer to DIMM to get registers for.
  @param[out] pMediaLogCount - number of media errors on DIMM
  @param[out] pThermalLogCount - number of thermal errors on DIMM

  @retval EFI_INVALID_PARAMETER One or more parameters are invalid
  @retval EFI_SUCCESS All ok
**/
EFI_STATUS
FwCmdGetErrorCount(
  IN     DIMM *pDimm,
     OUT UINT32 *pMediaLogCount OPTIONAL,
     OUT UINT32 *pThermalLogCount OPTIONAL
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  PT_INPUT_PAYLOAD_GET_ERROR_LOG InputPayload;
  LOG_INFO_DATA_RETURN OutputPayload;

  NVDIMM_ENTRY();

  ZeroMem(&InputPayload, sizeof(InputPayload));
  ZeroMem(&OutputPayload, sizeof(OutputPayload));

  if (pDimm == NULL || (pMediaLogCount == NULL && pThermalLogCount == NULL)) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  InputPayload.LogParameters.Separated.LogInfo = 1;

  if (pMediaLogCount != NULL) {
    InputPayload.LogParameters.Separated.LogType = ErrorLogTypeMedia;

    InputPayload.LogParameters.Separated.LogLevel = 0;
    ReturnCode = FwCmdGetErrorLog(pDimm, &InputPayload, &OutputPayload, sizeof(OutputPayload), NULL, 0);
    if (EFI_ERROR(ReturnCode)) {
      *pMediaLogCount = 0;
      goto Finish;
    }
    *pMediaLogCount = GetLogEntriesCount(&OutputPayload);

    InputPayload.LogParameters.Separated.LogLevel = 1;
    ReturnCode = FwCmdGetErrorLog(pDimm, &InputPayload, &OutputPayload, sizeof(OutputPayload), NULL, 0);
    if (EFI_ERROR(ReturnCode)) {
      *pMediaLogCount = 0;
      goto Finish;
    }
    *pMediaLogCount += GetLogEntriesCount(&OutputPayload);
  }

  if (pThermalLogCount != NULL) {
    InputPayload.LogParameters.Separated.LogType = ErrorLogTypeThermal;

    InputPayload.LogParameters.Separated.LogLevel = 0;
    ReturnCode = FwCmdGetErrorLog(pDimm, &InputPayload, &OutputPayload, sizeof(OutputPayload), NULL, 0);
    if (EFI_ERROR(ReturnCode)) {
      *pThermalLogCount = 0;
      goto Finish;
    }
    *pThermalLogCount = GetLogEntriesCount(&OutputPayload);

    InputPayload.LogParameters.Separated.LogLevel = 1;
    ReturnCode = FwCmdGetErrorLog(pDimm, &InputPayload, &OutputPayload, sizeof(OutputPayload), NULL, 0);
    if (EFI_ERROR(ReturnCode)) {
      *pThermalLogCount = 0;
      goto Finish;
    }
    *pThermalLogCount += GetLogEntriesCount(&OutputPayload);
  }

Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Generate the OEM PCD Header

  @param[in out] pPlatformConfigData Pointer to Platform Config Data Header

  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR Unable to retrieve PCAT table
  @retval EFI_INVALID_PARAMETER pPlatformConfigData is NULL
**/
STATIC
EFI_STATUS
GenerateOemPcdHeader (
  IN OUT NVDIMM_CONFIGURATION_HEADER *pPlatformConfigData
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  if (pPlatformConfigData == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  if (gNvmDimmData->PMEMDev.pPcatHead == NULL) {
    NVDIMM_DBG("PCAT table not found");
    ReturnCode = EFI_DEVICE_ERROR;
    goto Finish;
  }

  pPlatformConfigData->Header.Signature = NVDIMM_CONFIGURATION_HEADER_SIG;
  pPlatformConfigData->Header.Length = sizeof (*pPlatformConfigData);
  // For Purley platforms, only one revision (0x1) for PCD Config Header is supported
  if (IS_ACPI_HEADER_REV_MAJ_0_MIN_VALID(gNvmDimmData->PMEMDev.pPcatHead->pPlatformConfigAttr)) {
    pPlatformConfigData->Header.Revision.AsUint8 = NVDIMM_CONFIGURATION_TABLES_REVISION_1;
  }
  else {
    pPlatformConfigData->Header.Revision.AsUint8 = gNvmDimmData->PMEMDev.pPcatHead->pPlatformConfigAttr->Header.Revision.AsUint8;
  }
  CopyMem_S(&pPlatformConfigData->Header.OemId, sizeof(pPlatformConfigData->Header.OemId), NVDIMM_CONFIGURATION_HEADER_OEM_ID, NVDIMM_CONFIGURATION_HEADER_OEM_ID_LEN);
  pPlatformConfigData->Header.OemTableId = NVDIMM_CONFIGURATION_HEADER_OEM_TABLE_ID;
  pPlatformConfigData->Header.OemRevision = NVDIMM_CONFIGURATION_HEADER_OEM_REVISION;
  pPlatformConfigData->Header.CreatorId = NVDIMM_CONFIGURATION_HEADER_CREATOR_ID;
  pPlatformConfigData->Header.CreatorRevision = NVDIMM_CONFIGURATION_HEADER_CREATOR_REVISION;

  GenerateChecksum(pPlatformConfigData, pPlatformConfigData->Header.Length, PCAT_TABLE_HEADER_CHECKSUM_OFFSET);

Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}


/**
  Get Platform Config Data OEM partition Intel config region and check a correctness of header.
  We only return the actua PCD config data, from the first 64KiB of Intel FW/SW config metadata.
  The latter 64KiB is reserved for OEM use.

  The caller is responsible for a memory deallocation of the ppPlatformConfigData

  @param[in] pDimm The Intel NVM Dimm to retrieve PCD from
  @param[in] RetoreCorrupt If true will generate a default PCD when a corrupt header is found
  @param[out] ppPlatformConfigData Pointer to a new buffer pointer for storing retrieved data

  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR Incorrect PCD header
  @retval Other return codes from GetPcdOemConfigDataUsingSmallPayload
**/
EFI_STATUS
GetPlatformConfigDataOemPartition (
  IN     DIMM *pDimm,
  IN     BOOLEAN RestoreCorrupt,
     OUT NVDIMM_CONFIGURATION_HEADER **ppPlatformConfigData
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  UINT32 PcdDataSize = 0;
  NVDIMM_ENTRY();

  if (pDimm == NULL || ppPlatformConfigData == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  /** Get current Platform Config Data oem partition from dimm **/
  ReturnCode = GetPcdOemConfigDataUsingSmallPayload(pDimm, (UINT8 **)ppPlatformConfigData, &PcdDataSize);
  if(RestoreCorrupt && (EFI_NOT_FOUND == ReturnCode || EFI_VOLUME_CORRUPTED == ReturnCode)) {
    NVDIMM_WARN("Generating new OemPcdHeader due to missing or corrupt PCD config header.");
    *ppPlatformConfigData = AllocateZeroPool(sizeof(NVDIMM_CONFIGURATION_HEADER));
    if (*ppPlatformConfigData == NULL) {
      ReturnCode = EFI_OUT_OF_RESOURCES;
      goto Finish;
    }

    ReturnCode = GenerateOemPcdHeader(*ppPlatformConfigData);
    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_DBG("Generating new OemPcdHeader failed.");
      goto Finish;
    }
    goto Finish;
  }

  if (EFI_ERROR(ReturnCode) || *ppPlatformConfigData == NULL) {
    NVDIMM_DBG("Error calling Get Platform Config Data FW command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);

    goto Finish;
  }

Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Set Platform Config Data OEM Partition Intel config region.
  We only write to the first 64KiB of Intel FW/SW config metadata. The latter
  64KiB is reserved for OEM use.

  @param[in] pDimm The Intel NVM Dimm to set PCD
  @param[in] pNewConf Pointer to new config data to write
  @param[in] NewConfSize Size of pNewConf

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER NULL inputs or bad size
  @retval Other return codes from FwCmdSetPlatformConfigData
**/
EFI_STATUS
SetPlatformConfigDataOemPartition(
  IN     DIMM *pDimm,
  IN     NVDIMM_CONFIGURATION_HEADER *pNewConf,
  IN     UINT32 NewConfSize
  )
{
  UINT8 *pOemPartition = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  if ((pDimm == NULL) || (pNewConf == NULL)) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  if ((NewConfSize == 0) || (NewConfSize > PCD_OEM_PARTITION_INTEL_CFG_REGION_SIZE)) {
    NVDIMM_DBG("Bad NewConfSize");
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  /* Previous algorithm assumed read and write via large payload MB transactions, so
     required reading / writing entire PCD region. Switched to using SMALL MB, which allows
     writing only the relevant data, and preventing any writes > 64kb. This technique is faster
     given current size of PCD Data.
   */

  // Write the PCD data via small payload MB. This call internally enforces using small payload MB
  // for any OEM Partition writes.
  ReturnCode = FwCmdSetPlatformConfigData(pDimm, PCD_OEM_PARTITION_ID, (UINT8*)pNewConf, NewConfSize);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Failed to set Platform Config Data");
    goto Finish;
  }

Finish:
  FREE_POOL_SAFE(pOemPartition);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}


/**
  Matches FW return code to one of available EFI_STATUS EFI base types

  @param[in] Status - status byte returned from FW command

  @retval - Appropriate EFI_STATUS
**/
EFI_STATUS
MatchFwReturnCode (
  IN     UINT8 FwStatus
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();
  switch (FwStatus) {
  case FW_SUCCESS:
    break;

  case FW_INVALID_COMMAND_PARAMETER:
  case FW_INVALID_ALIGNMENT:
    ReturnCode = EFI_INVALID_PARAMETER;
    break;

  case FW_DATA_TRANSFER_ERROR:
  case FW_INTERNAL_DEVICE_ERROR:
  case FW_NO_RESOURCES:
    ReturnCode = EFI_DEVICE_ERROR;
    break;

  case FW_UNSUPPORTED_COMMAND:
  case FW_INJECTION_NOT_ENABLED:
    ReturnCode = EFI_UNSUPPORTED;
    break;

  case FW_DEVICE_BUSY:
    ReturnCode = EFI_NO_RESPONSE;
    break;
  case FW_MEDIA_DISABLED:
    ReturnCode = EFI_NO_MEDIA;
    break;

  case FW_INCORRECT_PASSPHRASE:
  case FW_CONFIG_LOCKED:
    ReturnCode = EFI_ACCESS_DENIED;
    break;

  case FW_AUTH_FAILED:
  case FW_INVALID_SECURITY_STATE:
    ReturnCode = EFI_SECURITY_VIOLATION;
    break;

  case FW_DATA_NOT_SET:
    ReturnCode = EFI_NOT_STARTED;
    break;

  case FW_TIMEOUT_OCCURED:
    ReturnCode = EFI_TIMEOUT;
    break;

  case FW_SYSTEM_TIME_NOT_SET:

  case FW_REVISION_FAILURE:
  case FW_INCOMPATIBLE_DIMM_TYPE:
  case FW_ABORTED:
  case FW_UPDATE_ALREADY_OCCURED:
    ReturnCode = EFI_ABORTED;
    break;

  default:
    ReturnCode = EFI_ABORTED;
    break;
  }

  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

#ifdef OS_BUILD
/**
  Matches DSM return code to one of available EFI_STATUS EFI base types

  @param[in] DsmStatus - status byte returned from FW command

  @retval - Appropriate EFI_STATUS
**/
EFI_STATUS
MatchDsmReturnCode(
  IN     UINT8 DsmStatus
)
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  switch (DsmStatus) {
  case DSM_VENDOR_SUCCESS:
    break;
  default:
    ReturnCode = EFI_ABORTED;
    break;
  }

  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}
#endif

/**
  Check if SKU conflict occurred.
  Any mixed modes between manageable DIMMs are prohibited on a platform.

  @param[in] pDimm1 - first DIMM to compare SKU mode
  @param[in] pDimm2 - second DIMM to compare SKU mode

  @retval NVM_SUCCESS - if everything went fine
  @retval NVM_ERR_DIMM_SKU_MODE_MISMATCH - if mode conflict occurred
  @retval NVM_ERR_DIMM_SKU_SECURITY_MISMATCH - if security mode conflict occurred
**/
NvmStatusCode
IsDimmSkuModeMismatch(
  IN    DIMM *pDimm1,
  IN    DIMM *pDimm2
  )
{
  NvmStatusCode StatusCode = NVM_ERR_INVALID_PARAMETER;
  NVDIMM_ENTRY();

  if (pDimm1 == NULL || pDimm2 == NULL) {
    NVDIMM_DBG("Invalid parameter given to check SKU mismatch");
    goto Finish;
  }

  if (!IsDimmManageable(pDimm1) || !IsDimmManageable(pDimm2)) {
    StatusCode = NVM_SUCCESS;
    goto Finish;
  }

  StatusCode = SkuComparison(*(UINT32 *)&pDimm1->SkuInformation,
                             *(UINT32 *)&pDimm2->SkuInformation);

Finish:
  NVDIMM_EXIT();
  return StatusCode;
}

/**
  Calculate a size of capacity considered Reserved. It is the aligned PM
  capacity less the mapped AD capacity

  @param[in] Dimm to retrieve reserved size for
  @param[out] pReservedCapacity pointer to reserved capacity

  @retval EFI_INVALID_PARAMETER passed NULL argument
  @retval EFI_ABORTED Failure to retrieve current memory mode
  @retval EFI_SUCCESS Success
**/
EFI_STATUS
GetReservedCapacity(
  IN     DIMM *pDimm,
  OUT UINT64 *pReservedCapacity
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  MEMORY_MODE CurrentMode = MEMORY_MODE_1LM;

  if (pDimm == NULL || pReservedCapacity == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  ReturnCode = CurrentMemoryMode(&CurrentMode);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Unable to determine current memory mode");
    goto Finish;
  }

  if (pDimm->Configured || (MEMORY_MODE_2LM == CurrentMode)) {
    *pReservedCapacity =  ROUNDDOWN(pDimm->PmCapacity, REGION_PERSISTENT_SIZE_ALIGNMENT_B) - pDimm->MappedPersistentCapacity;
  }
  else {
    *pReservedCapacity = 0;
  }

Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

#define FW_TEMPERATURE_CONST_1 625
#define FW_TEMPERATURE_CONST_2 10000

/**
  Transform temperature in FW format to usual integer in Celsius

  @param[in] Temperature Temperature from FW

  @retval Value in Celsius
**/
INT16
TransformFwTempToRealValue(
  IN     TEMPERATURE Temperature
  )
{
  INT16 Value = 0;

  Value = (INT16) (((UINT64)Temperature.Separated.TemperatureValue * FW_TEMPERATURE_CONST_1) / FW_TEMPERATURE_CONST_2);

  if (Temperature.Separated.Sign == TEMPERATURE_NEGATIVE) {
    Value *= -1;
  }

  return Value;
}

/**
  Transform temperature from usual integer in Celsius to FW format

  @param[in] Value Temperature in Celsius

  @retval Temperature in FW format
**/
TEMPERATURE
TransformRealValueToFwTemp(
  IN     INT16 Value
  )
{
  TEMPERATURE Temperature;

  ZeroMem(&Temperature, sizeof(Temperature));

  if (Value >= 0) {
    Temperature.Separated.Sign = TEMPERATURE_POSITIVE;
  } else {
    Temperature.Separated.Sign = TEMPERATURE_NEGATIVE;
    /** Change to positive **/
    Value *= -1;
  }

  Temperature.Separated.TemperatureValue = (UINT16) (((UINT64)Value * FW_TEMPERATURE_CONST_2) / FW_TEMPERATURE_CONST_1);

  return Temperature;
}

/**
  Get the Dimm UID (a globally unique NVDIMM identifier) for DIMM,
  as per the following representation defined in ACPI 6.1 specification:
    "%02x%02x-%02x-%02x%2x-%02x%02x%02x%02x" (if the Manufacturing Location and Manufacturing Date fields are valid)
    "%02x%02x-%02x%02x%02x%02x" (if the Manufacturing Location and Manufacturing Date fields are invalid)

  @param[in] pDimm DIMM for which the UID is being initialized
  @param[out] pDimmUid Array to store Dimm UID
  @param[in] DimmUidLen Size of pDimmUid

  @retval EFI_SUCCESS Dimm UID field was initialized successfully.
  @retval EFI_INVALID_PARAMETER one or more parameters are NULL.
**/
EFI_STATUS
GetDimmUid(
  IN     DIMM *pDimm,
     OUT CHAR16 *pDimmUid,
  IN     UINT32 DimmUidLen
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  CHAR16 *TmpDimmUid = NULL;

  if (pDimm == NULL || pDimmUid == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  if (pDimm->VendorId != 0 && pDimm->ManufacturingInfoValid != FALSE && pDimm->SerialNumber != 0) {
    TmpDimmUid = CatSPrint(NULL, L"%04x", EndianSwapUint16(pDimm->VendorId));
    if (pDimm->ManufacturingInfoValid == TRUE) {
      TmpDimmUid = CatSPrintClean(TmpDimmUid, L"-%02x-%04x", pDimm->ManufacturingLocation, EndianSwapUint16(pDimm->ManufacturingDate));
    }
    TmpDimmUid = CatSPrintClean(TmpDimmUid ,L"-%08x", EndianSwapUint32(pDimm->SerialNumber));
  } else {
    TmpDimmUid = CatSPrint(NULL, L"");
  }

  if (TmpDimmUid != NULL) {
    StrnCpyS(pDimmUid, DimmUidLen, TmpDimmUid, DimmUidLen - 1);
    FREE_POOL_SAFE(TmpDimmUid);
  }

Finish:
  NVDIMM_EXIT_CHECK_I64(ReturnCode);
  return ReturnCode;
}

/**
  Set Obj Status when DIMM is not found using Id expected by end user

  @param[in] DimmId the Pid for the DIMM that was not found
  @param[in] pDimms Pointer to head of list where DimmId should be found
  @param[out] pCommandStatus Pointer to command status structure

**/
VOID
SetObjStatusForDimmNotFound(
  IN     UINT16 DimmId,
  IN     LIST_ENTRY *pDimms,
  OUT COMMAND_STATUS *pCommandStatus
)
{
  DIMM *pCurrentDimm = NULL;

  pCurrentDimm = GetDimmByPid(DimmId, pDimms);
  if (pCurrentDimm != NULL) {
    SetObjStatusForDimm(pCommandStatus, pCurrentDimm, NVM_ERR_DIMM_NOT_FOUND);
    pCurrentDimm = NULL;
  }
  else
  {
    SetObjStatus(pCommandStatus, DimmId, NULL, 0, NVM_ERR_DIMM_NOT_FOUND);
  }
}

/**
Set object status for DIMM

@param[out] pCommandStatus Pointer to command status structure
@param[in] pDimm DIMM for which the object status is being set
@param[in] Status Object status to set
**/
VOID
SetObjStatusForDimm(
  OUT COMMAND_STATUS *pCommandStatus,
  IN     DIMM *pDimm,
  IN     NVM_STATUS Status
)
{
  SetObjStatusForDimmWithErase(pCommandStatus, pDimm, Status, FALSE);
}

/**
  Set object status for DIMM

  @param[out] pCommandStatus Pointer to command status structure
  @param[in] pDimm DIMM for which the object status is being set
  @param[in] Status Object status to set
  @param[in] If TRUE - clear all other status before setting this one
**/
VOID
SetObjStatusForDimmWithErase(
     OUT COMMAND_STATUS *pCommandStatus,
  IN     DIMM *pDimm,
  IN     NVM_STATUS Status,
  IN     BOOLEAN EraseFirst
  )
{
  CHAR16 DimmUid[MAX_DIMM_UID_LENGTH];

  if (pDimm == NULL || pCommandStatus == NULL) {
    return;
  }

  ZeroMem(DimmUid, sizeof(DimmUid));

  if (EFI_ERROR(GetDimmUid(pDimm, DimmUid, MAX_DIMM_UID_LENGTH))) {
    NVDIMM_ERR("Error in GetDimmUid");
    return;
  }

  if (EraseFirst) {
    EraseObjStatus(pCommandStatus, pDimm->DeviceHandle.AsUint32, DimmUid, MAX_DIMM_UID_LENGTH);
  }

  pCommandStatus->ObjectType = ObjectTypeDimm;
  SetObjStatus(pCommandStatus, pDimm->DeviceHandle.AsUint32, DimmUid, MAX_DIMM_UID_LENGTH, Status);
}

/**
  Get overwrite DIMM operation status for DIMM

  @param[in] pDimm DIMM to retrieve overwrite DIMM operation status from
  @parma[out] pOverwriteDimmStatus Retrieved status

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER One or more parameters are NULL
**/
EFI_STATUS
GetOverwriteDimmStatus(
  IN     DIMM *pDimm,
     OUT UINT8 *pOverwriteDimmStatus
  )
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  UINT8 FwStatus = FW_SUCCESS;
  PT_OUTPUT_PAYLOAD_FW_LONG_OP_STATUS LongOpStatus;

  NVDIMM_ENTRY();

  ZeroMem(&LongOpStatus, sizeof(LongOpStatus));

  if (pDimm == NULL || pOverwriteDimmStatus == NULL) {
    goto Finish;
  }

  ReturnCode = FwCmdGetLongOperationStatus(pDimm, &FwStatus, &LongOpStatus);
  if (EFI_ERROR(ReturnCode)) {
    if ((pDimm->FwVer.FwApiMajor == 1 && pDimm->FwVer.FwApiMinor <= 4 && FwStatus == FW_INTERNAL_DEVICE_ERROR) ||
      FwStatus == FW_DATA_NOT_SET) {
      /** It is valid case when there is no long operation status **/
      *pOverwriteDimmStatus = OVERWRITE_DIMM_STATUS_NOT_STARTED;
      ReturnCode = EFI_SUCCESS;
    }
    else {
      *pOverwriteDimmStatus = OVERWRITE_DIMM_STATUS_UNKNOWN;
      goto Finish;
    }
  }

  if (LongOpStatus.CmdOpcode == PtSetSecInfo && LongOpStatus.CmdSubcode == SubopOverwriteDimm) {
    switch (LongOpStatus.Status) {
      case FW_DEVICE_BUSY:
        *pOverwriteDimmStatus = OVERWRITE_DIMM_STATUS_IN_PROGRESS;
        break;
      case FW_DATA_NOT_SET:
        *pOverwriteDimmStatus = OVERWRITE_DIMM_STATUS_NOT_STARTED;
        break;
      default:
        *pOverwriteDimmStatus = OVERWRITE_DIMM_STATUS_COMPLETED;
        break;
    }
  } else {
    *pOverwriteDimmStatus = OVERWRITE_DIMM_STATUS_UNKNOWN;
  }

  ReturnCode = EFI_SUCCESS;

Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Poll while ARS long operation status reports DEVICE BUSY

  @param[in] pDimm DIMM to retrieve overwrite DIMM operation status from
  @parma[in] TimeoutSecs - Poll timeout in seconds

  @retval EFI_SUCCESS Success
  @retval EFI_TIMEOUT Timeout expired and did not receive something other than FW_DEVICE_BUSY
  @retval EFI_INVALID_PARAMETER One or more parameters are NULL
  @retval EFI_DEVICE_ERROR Retrieved an unexpeced opcode/subopcode when requesting long op status
**/

EFI_STATUS
PollOnArsDeviceBusy(
  IN     DIMM *pDimm,
  IN     UINT32 TimeoutSecs
)
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  UINT8 FwStatus = FW_SUCCESS;
  PT_OUTPUT_PAYLOAD_FW_LONG_OP_STATUS LongOpStatus;
  UINT32 RetryMax = 0;
  UINT32 RetryCount = 0;

  NVDIMM_ENTRY();

  ZeroMem(&LongOpStatus, sizeof(LongOpStatus));

  if (pDimm == NULL) {
    goto Finish;
  }

  RetryMax = (TimeoutSecs * 1000000) / POLL_ARS_LONG_OP_DELAY_US;

  for(RetryCount = 0; RetryCount < RetryMax; ++RetryCount) {
    ReturnCode = FwCmdGetLongOperationStatus(pDimm, &FwStatus, &LongOpStatus);
    if (EFI_ERROR(ReturnCode)) {
        NVDIMM_ERR("Error occurred while polling for ARS enable/disable state.\n");
        break;
    }

    if (LongOpStatus.CmdOpcode == PtSetFeatures && LongOpStatus.CmdSubcode == SubopAddressRangeScrub) {
      if (FW_DEVICE_BUSY != LongOpStatus.Status) {
        ReturnCode = EFI_SUCCESS;
        goto Finish;
      }
    }
    else {
      NVDIMM_ERR("Unexpected opcode/subopcodes retrieved with Get Long Op Status\n");
      ReturnCode = EFI_DEVICE_ERROR;
      break;
    }
    gBS->Stall(POLL_ARS_LONG_OP_DELAY_US);
    ZeroMem(&LongOpStatus, sizeof(LongOpStatus));
  }

  if (EFI_SUCCESS == ReturnCode && RetryCount == RetryMax) {
    ReturnCode = EFI_TIMEOUT;
  }
Finish:
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Customer Format Dimm
  Send a customer format command through the smbus

  @param[in] pDimm The dimm to attempt to format

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER Invalid FW Command Parameter
**/
EFI_STATUS
FwCmdFormatDimm(
  IN    DIMM *pDimm
  )
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;
  NVM_FW_CMD *pFwCmd = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));

  if (!pFwCmd) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->Opcode = PtCustomerFormat;
  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);

  if (EFI_ERROR(ReturnCode) && ReturnCode != EFI_TIMEOUT) {
    NVDIMM_DBG("Error detected when sending PtCustomerFormat command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to get DDRT IO init info

  @param[in] pDimm Target DIMM structure pointer
  @param[out] pDdrtIoInitInfo pointer to filled payload with DDRT IO init info

  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR if failed to open PassThru protocol
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
  @retval EFI_INVALID_PARAMETER input parameter null
**/
EFI_STATUS
FwCmdGetDdrtIoInitInfo(
  IN     DIMM *pDimm,
     OUT PT_OUTPUT_PAYLOAD_GET_DDRT_IO_INIT_INFO *pDdrtIoInitInfo
  )
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  NVM_FW_CMD *pFwCmd = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pDdrtIoInitInfo == NULL) {
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetAdminFeatures;
  pFwCmd->SubOpcode = SubopDdrtIoInitInfo;
  pFwCmd->OutputPayloadSize = sizeof(*pDdrtIoInitInfo);
  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);

  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Failed to get DDRT IO init info");
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  CopyMem_S(pDdrtIoInitInfo, sizeof(*pDdrtIoInitInfo), pFwCmd->OutPayload, sizeof(*pDdrtIoInitInfo));

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Get Command Access Policy for a specific command
  @param[IN] pDimm Target DIMM structure pointer
  @param[IN] Opcode for the command
  @param[IN] SubOpcode for the command
  @param[OUT] pRestricted code for applied restriction (0-3)

  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR if failed to open PassThru protocol
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
  @retval EFI_INVALID_PARAMETER input parameter null
**/
EFI_STATUS
FwCmdGetCommandAccessPolicy(
  IN  DIMM *pDimm,
  IN  UINT8 Opcode,
  IN  UINT8 Subopcode,
  OUT UINT8 *pRestriction
)
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  PT_INPUT_PAYLOAD_GET_COMMAND_ACCESS_POLICY *pInputCAP = NULL;
  PT_OUTPUT_PAYLOAD_GET_COMMAND_ACCESS_POLICY *pOutputCAP = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pRestriction == NULL) {
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetAdminFeatures;
  pFwCmd->SubOpcode = SubopCommandAccessPolicy;
  pFwCmd->OutputPayloadSize = sizeof(PT_OUTPUT_PAYLOAD_GET_COMMAND_ACCESS_POLICY);

  pInputCAP = (PT_INPUT_PAYLOAD_GET_COMMAND_ACCESS_POLICY*) pFwCmd->InputPayload;
  pInputCAP->Opcode = Opcode;
  pInputCAP->Subopcode = Subopcode;
  pFwCmd->InputPayloadSize = sizeof(PT_INPUT_PAYLOAD_GET_COMMAND_ACCESS_POLICY);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending GetCommandAccessPolicy command (RC = 0x%x)", ReturnCode);
    NVDIMM_DBG("FW CMD Status 0x%x", pFwCmd->Status);
    if (pFwCmd->Status == FW_INVALID_COMMAND_PARAMETER) {
      ReturnCode = EFI_UNSUPPORTED;
    }
    else {
      FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    }
    goto Finish;
  }

  pOutputCAP = (PT_OUTPUT_PAYLOAD_GET_COMMAND_ACCESS_POLICY*) pFwCmd->OutPayload;
  *pRestriction = pOutputCAP->Restriction;

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
Inject Temperature error payload
@param[IN] pDimm Target DIMM structure pointer
@param[IN] subopcode for error injection command
@param[OUT] pinjectInputPayload - input payload to be sent
@param[OUT] pFwStatus FW status returned by dimm

@retval EFI_SUCCESS Success
@retval EFI_DEVICE_ERROR if failed to open PassThru protocol
@retval EFI_OUT_OF_RESOURCES memory allocation failure
@retval EFI_INVALID_PARAMETER input parameter null
**/
EFI_STATUS
FwCmdInjectError(
  IN  DIMM *pDimm,
  IN  UINT8 SubOpCode,
  OUT VOID *pinjectInputPayload,
  OUT UINT8 *pFwStatus
)
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pinjectInputPayload == NULL || pFwStatus == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));

  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtInjectError;
  pFwCmd->SubOpcode = SubOpCode;
  pFwCmd->InputPayloadSize = SMALL_PAYLOAD_SIZE;
  pFwCmd->OutputPayloadSize = 0;
  CopyMem_S(pFwCmd->InputPayload, sizeof(pFwCmd->InputPayload), pinjectInputPayload, pFwCmd->InputPayloadSize);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  *pFwStatus = pFwCmd->Status;
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Failed to inject error, error: %x\n", ReturnCode);
    if (pFwCmd->Status == FW_INJECTION_NOT_ENABLED) {
      NVDIMM_DBG("FW Error injection is not enabled");
    }
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to get DIMMs system time

  @param[in] pDimm Target DIMM structure pointer
  @param[out] pSystemTimePayload pointer to filled payload

  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR if failed to open PassThru protocol
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
  @retval EFI_INVALID_PARAMETER input parameter null
**/
EFI_STATUS
FwCmdGetSystemTime(
  IN     DIMM *pDimm,
  OUT PT_SYTEM_TIME_PAYLOAD *pSystemTimePayload
)
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pSystemTimePayload == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));

  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetAdminFeatures;
  pFwCmd->SubOpcode = SubopSystemTime;
  pFwCmd->OutputPayloadSize = sizeof(*pSystemTimePayload);

  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);

  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_ERR("Error detected when sending FwCmdGetSystemTime command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }
  CopyMem_S(pSystemTimePayload, sizeof(*pSystemTimePayload), pFwCmd->OutPayload, sizeof(*pSystemTimePayload));

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to get extended ADR status info

  @param[in] pDimm Target DIMM structure pointer
  @param[out] pExtendedAdrInfo pointer to filled payload with extended ADR info

  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR if failed to open PassThru protocol
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
  @retval EFI_INVALID_PARAMETER input parameter null
  @retval EFI_UNSUPPORTED if FIS doesn't support Get Admin Features/Extended ADR
**/
EFI_STATUS
FwCmdGetExtendedAdrInfo(
  IN     DIMM *pDimm,
  OUT PT_OUTPUT_PAYLOAD_GET_EADR *pExtendedAdrInfo
)
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  NVM_FW_CMD *pFwCmd = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pExtendedAdrInfo == NULL) {
    goto Finish;
  }

  //Get Extended ADR status info is new to FIS2.0
  if (pDimm->FwVer.FwApiMajor < 2) {
    ReturnCode = EFI_UNSUPPORTED;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetAdminFeatures;
  pFwCmd->SubOpcode = SubopExtendedAdr;
  pFwCmd->OutputPayloadSize = sizeof(*pExtendedAdrInfo);
  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);

  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Failed to get extended ADR info");
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  CopyMem_S(pExtendedAdrInfo, sizeof(*pExtendedAdrInfo), pFwCmd->OutPayload, sizeof(*pExtendedAdrInfo));

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Firmware command to get Latch System Shutdown State

  @param[in] pDimm Target DIMM structure pointer
  @param[out] pExtendedAdrInfo pointer to filled payload with Latch System Shutdown State info

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER if parameter provided is invalid
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
**/
EFI_STATUS
FwCmdGetLatchSystemShutdownStateInfo(
  IN     DIMM *pDimm,
  OUT PT_OUTPUT_PAYLOAD_GET_LATCH_SYSTEM_SHUTDOWN_STATE *pLastSystemShutdownStateInfo
)
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  NVM_FW_CMD *pFwCmd = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pLastSystemShutdownStateInfo == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  pFwCmd = AllocateZeroPool(sizeof(*pFwCmd));
  if (pFwCmd == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtGetAdminFeatures;
  pFwCmd->SubOpcode = SubopLatchSystemShutdownState;
  pFwCmd->OutputPayloadSize = sizeof(*pLastSystemShutdownStateInfo);
  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);

  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Failed to get Latch System Shutdown State info");
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  CopyMem_S(pLastSystemShutdownStateInfo, sizeof(*pLastSystemShutdownStateInfo), pFwCmd->OutPayload, sizeof(*pLastSystemShutdownStateInfo));

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
Check if DIMM is manageable

@param[in] pDimm the DIMM struct

@retval BOOLEAN whether or not dimm is manageable
**/
BOOLEAN
IsDimmManageable(
  IN  DIMM *pDimm
)
{
  if (pDimm == NULL)
  {
    return FALSE;
  }

  return IsDimmManageableByValues(pDimm->SubsystemVendorId,
    pDimm->FmtInterfaceCodeNum,
    pDimm->FmtInterfaceCode,
    pDimm->SubsystemDeviceId,
    pDimm->FwVer.FwApiMajor,
    pDimm->FwVer.FwApiMinor);
}

/**
Check if DIMM is in supported config

@param[in] pDimm the DIMM struct

@retval BOOLEAN whether or not dimm is in supported config
**/
BOOLEAN
IsDimmInSupportedConfig(
  IN  DIMM *pDimm
)
{
  if (pDimm == NULL)
  {
    return FALSE;
  }
  return !IsDimmInUnmappedPopulationViolation(pDimm);
}

/**
Check if DIMM is in population violation

@param[in] pDimm the DIMM struct

@retval BOOLEAN whether or not dimm is in population violation
**/
BOOLEAN
IsDimmInPopulationViolation(
  IN  DIMM *pDimm
)
{
  if (pDimm == NULL)
  {
    return FALSE;
  }
  return (IsDimmInUnmappedPopulationViolation(pDimm) || IsDimmInPmMappedPopulationViolation(pDimm));
}

/**
Check if DIMM is in population violation and fully unmapped

@param[in] pDimm the DIMM struct

@retval BOOLEAN whether or not dimm is in population violation and fully unmapped
**/
BOOLEAN
IsDimmInUnmappedPopulationViolation(
  IN  DIMM *pDimm
)
{
  if (pDimm == NULL)
  {
    return FALSE;
  }
  return (DIMM_CONFIG_DCPMM_POPULATION_ISSUE == pDimm->ConfigStatus);
}

/**
Check if DIMM is in population violation and persistent memory is still mapped

@param[in] pDimm the DIMM struct

@retval BOOLEAN whether or not dimm is in population violation and persistent memory is still mapped
**/
BOOLEAN
IsDimmInPmMappedPopulationViolation(
  IN  DIMM *pDimm
)
{
  if (pDimm == NULL)
  {
    return FALSE;
  }
  return (DIMM_CONFIG_PM_MAPPED_VM_POPULATION_ISSUE == pDimm->ConfigStatus);
}

/**
Check if the dimm interface code of this DIMM is supported

@param[in] pDimm the DIMM struct

@retval true if supported, false otherwise
**/
BOOLEAN
IsDimmInterfaceCodeSupported(
  IN  DIMM *pDimm
)
{
  if (pDimm == NULL)
  {
    return FALSE;
  }

  return IsDimmInterfaceCodeSupportedByValues(pDimm->FmtInterfaceCodeNum,
    pDimm->FmtInterfaceCode);
}

/**
Check if the subsystem device ID of this DIMM is supported

@param[in] pDimm the DIMM struct

@retval true if supported, false otherwise
**/
BOOLEAN
IsSubsystemDeviceIdSupported(
  IN  DIMM *pDimm
)
{
  if (pDimm == NULL)
  {
    return FALSE;
  }

  return IsSubsystemDeviceIdSupportedByValues(
    pDimm->SubsystemDeviceId);
}

/**
Check if current firmware API version is supported

@param[in] pDimm the DIMM struct

@retval true if supported, false otherwise
**/
BOOLEAN
IsFwApiVersionSupported(
  IN  DIMM *pDimm
)
{
  if (pDimm == NULL)
  {
    return FALSE;
  }

  return IsFwApiVersionSupportedByValues(pDimm->FwVer.FwApiMajor,
    pDimm->FwVer.FwApiMinor);
}

/**
Clears the PCD Cache on each DIMM in the global DIMM list

@retval EFI_SUCCESS Success
**/
EFI_STATUS ClearPcdCacheOnDimmList(VOID)
{
#ifdef PCD_CACHE_ENABLED
  DIMM *pDimm = NULL;
  LIST_ENTRY *pDimmNode = NULL;

  if (NULL != gNvmDimmData) {
    LIST_FOR_EACH(pDimmNode, &gNvmDimmData->PMEMDev.Dimms) {
      if (NULL != pDimmNode) {
        pDimm = DIMM_FROM_NODE(pDimmNode);
        if (NULL != pDimm) {
          // Free memory and set to NULL so won't be used by Get PCD calls
          FREE_POOL_SAFE(pDimm->pPcdOem);
        }
      }
    }
  }
#endif // PCD_CACHE_ENABLED
  return EFI_SUCCESS;
}

/**
  Return what passthru method will be used to send the command.

  @param[in] pDimm The DCPMM to transact with
  @param[in] IsLargePayloadCommand Need to know if large payload interface is
                                   even desired. If not, then it makes no sense
                                   to write to the large payload mailbox unless
                                   the user specifies it.
  @param[out] Method Pointer to passthru method variable to modify
  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR if we failed to do basic communication with the DCPMM
**/
EFI_STATUS
DeterminePassThruMethod(
  IN DIMM *pDimm,
  IN BOOLEAN IsLargePayloadCommand,
  OUT DIMM_PASSTHRU_METHOD *Method
)
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  EFI_DCPMM_CONFIG2_PROTOCOL *pNvmDimmConfigProtocol = NULL;
  EFI_DCPMM_CONFIG_TRANSPORT_ATTRIBS Attribs;
  // Initialize incoming variable to a good default, just in case
  *Method = DimmPassthruSmbusSmallPayload;

  CHECK_RESULT(OpenNvmDimmProtocol(gNvmDimmConfigProtocolGuid, (VOID **)&pNvmDimmConfigProtocol, NULL), Finish);

  CHECK_RESULT(pNvmDimmConfigProtocol->GetFisTransportAttributes(pNvmDimmConfigProtocol, &Attribs), Finish);

  // Check if the user manually specified a certain interface. If specified,
  // go to passthru directly and don't do any auto-detection.
  // Ignore "default" settings here, they'll be used later
  if ((Attribs.Protocol != FisTransportAuto || Attribs.PayloadSize != FisTransportSizeAuto) &&
      // Skip this flow if only large payload was disabled (the default setting in OS)
      !(FisTransportAuto == Attribs.Protocol && FisTransportSizeSmallMb == Attribs.PayloadSize)) {
    // User only specified "-ddrt"
    if (IS_DDRT_FLAG_ENABLED(Attribs) && Attribs.PayloadSize == FisTransportSizeAuto) {
      if (IsLargePayloadCommand) {
        *Method = DimmPassthruDdrtLargePayload;
      } else {
        *Method = DimmPassthruDdrtSmallPayload;
      }
    } else if (IS_DDRT_FLAG_ENABLED(Attribs) && IS_LARGE_PAYLOAD_FLAG_ENABLED(Attribs)) {
      *Method = DimmPassthruDdrtLargePayload;
    } else if (IS_DDRT_FLAG_ENABLED(Attribs) && IS_SMALL_PAYLOAD_FLAG_ENABLED(Attribs)) {
      *Method = DimmPassthruDdrtSmallPayload;
    } else if (IS_SMBUS_FLAG_ENABLED(Attribs)) {
      *Method = DimmPassthruSmbusSmallPayload;
    } else {
      NVDIMM_ERR("Invalid Attribs state of %d, %d detected. Exiting", Attribs.Protocol, Attribs.PayloadSize);
      ReturnCode = EFI_INVALID_PARAMETER;
      goto Finish;
    }
    goto Finish;
  }

  if (pDimm->BootStatusBitmask & DIMM_BOOT_STATUS_MAILBOX_NOT_READY) {
    // We did not succeed in calling IdentifyDimm over any interface in
    // InitializeDimm(). Seems like a dead DCPMM. Don't bother sending anything more.
    ReturnCode = EFI_DEVICE_ERROR;
    NVDIMM_ERR("DCPMM mailbox is not ready. Cancelling PassThru()");
    goto Finish;
  }

    // If caller wants to send a large payload command
  if (TRUE == IsLargePayloadCommand &&
      // and if no problems found with sending large payload
      !(FisTransportSizeSmallMb == Attribs.PayloadSize ||
      (DIMM_MEDIA_NOT_ACCESSIBLE(pDimm->BootStatusBitmask)) ||
      (pDimm->BootStatusBitmask & DIMM_BOOT_STATUS_DDRT_NOT_READY))) {

    // Then allow them to do so
    *Method = DimmPassthruDdrtLargePayload;

  // Otherwise prefer small payload DDRT
  } else if (!((pDimm->BootStatusBitmask & DIMM_BOOT_STATUS_DDRT_NOT_READY))) {
    *Method = DimmPassthruDdrtSmallPayload;
  } else {
    // Otherwise last resort is small payload smbus
    *Method = DimmPassthruSmbusSmallPayload;
  }

Finish:
  return ReturnCode;
}

/**
  Check if sending a large payload command over the DDRT large payload
  mailbox is possible. Used by callers often to determine chunking behavior.

  @param[in] pDimm The DCPMM to transact with
  @param[out] Available Whether large payload is available. Pointer to boolean variable

  @retval EFI_SUCCESS Success
  @retval EFI_DEVICE_ERROR if we failed to do basic communication with the DCPMM
**/
EFI_STATUS
IsLargePayloadAvailable(
  IN DIMM *pDimm,
  OUT BOOLEAN *Available
)
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  DIMM_PASSTHRU_METHOD Method = DimmPassthruDdrtLargePayload;

  // TRUE -> We are attempting to send a large payload command
  // Only return true if DDRT large payload is allowed
  CHECK_RESULT(DeterminePassThruMethod(pDimm, TRUE, &Method), Finish);
  *Available = DimmPassthruDdrtLargePayload == Method;

Finish:
  return ReturnCode;
}

EFI_STATUS
PassThru(
  IN     struct _DIMM *pDimm,
  IN OUT NVM_FW_CMD *pCmd,
  IN     UINT64 Timeout
)
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  DIMM_PASSTHRU_METHOD Method = DimmPassthruDdrtLargePayload;
  BOOLEAN IsLargePayloadCommand = FALSE;

#ifdef OS_BUILD
  UINT8 InputPayloadTemp[IN_PAYLOAD_SIZE];
  NVM_INPUT_PAYLOAD_SMBUS_OS_PASSTHRU *pInputPayloadSOP = NULL;
#endif

  IsLargePayloadCommand = pCmd->LargeInputPayloadSize > 0;
  CHECK_RESULT(DeterminePassThruMethod(pDimm, IsLargePayloadCommand, &Method), Finish);

  // Obviously not ideal implementation, but ran into issues getting %s working
  // on Linux with NVDIMM_* prints. Need something working now though.
  if (DimmPassthruDdrtLargePayload == Method) {
    NVDIMM_DBG("Calling 0x%x:0x%x over ddrt lp on DCPMM 0x%x", pCmd->Opcode, pCmd->SubOpcode, pDimm->DeviceHandle.AsUint32);
  }
  else if (DimmPassthruDdrtSmallPayload == Method) {
    NVDIMM_DBG("Calling 0x%x:0x%x over ddrt sp on DCPMM 0x%x", pCmd->Opcode, pCmd->SubOpcode, pDimm->DeviceHandle.AsUint32);
  }
  else if (DimmPassthruSmbusSmallPayload == Method) {
    NVDIMM_DBG("Calling 0x%x:0x%x over smbus on DCPMM 0x%x", pCmd->Opcode, pCmd->SubOpcode, pDimm->DeviceHandle.AsUint32);
  }

  if (DimmPassthruSmbusSmallPayload == Method) {
#ifdef OS_BUILD
    // SMBUS: Use a special bios emulated command, which BIOS will interpret
    // as a passthru to the DCPMM through the interface of choice

    // Carefully copy the buffers
    CopyMem(InputPayloadTemp, pCmd->InputPayload, IN_PAYLOAD_SIZE);
    ZeroMem(pCmd->InputPayload, IN_PAYLOAD_SIZE + IN_PAYLOAD_SIZE_EXT_PAD);
    // Re-interpret the existing input payload, now using the full buffer
    pInputPayloadSOP = (NVM_INPUT_PAYLOAD_SMBUS_OS_PASSTHRU *)(pCmd->InputPayload);
    CopyMem(pInputPayloadSOP->Data, InputPayloadTemp, IN_PAYLOAD_SIZE);

    // Update the rest of the parameters
    pCmd->InputPayloadSize = IN_PAYLOAD_SIZE + IN_PAYLOAD_SIZE_EXT_PAD;
    pInputPayloadSOP->Opcode = pCmd->Opcode;
    pInputPayloadSOP->SubOpcode = pCmd->SubOpcode;
    pInputPayloadSOP->Timeout = PT_TIMEOUT_INTERVAL_EXT;
    // Specify SMBUS
    pInputPayloadSOP->TransportInterface = SmbusTransportInterface;
    pCmd->Opcode = PtEmulatedBiosCommands;
    pCmd->SubOpcode = SubopExtVendorSpecific;
  }

  // Use the OS passthru dsm mechanism to talk with the DCPMM
  // for both DDRT and SMBUS
  ReturnCode = DefaultPassThru(pDimm, pCmd, PT_TIMEOUT_INTERVAL);

  // If we're using the special bios emulated command (smbus only
  // for now), do some cleanup and restore previous pCmd values
  if (DimmPassthruSmbusSmallPayload == Method) {
    pCmd->Opcode = pInputPayloadSOP->Opcode;
    pCmd->SubOpcode = pInputPayloadSOP->SubOpcode;
    ZeroMem(pInputPayloadSOP, IN_PAYLOAD_SIZE + IN_PAYLOAD_SIZE_EXT_PAD);
    CopyMem(pCmd->InputPayload, InputPayloadTemp, IN_PAYLOAD_SIZE);
    pCmd->InputPayloadSize = IN_PAYLOAD_SIZE;
  }
  if (EFI_ERROR(ReturnCode)) {
    goto Finish;
  }

#else
    // SMBUS: Use the bios DCPMM protocol to send commands to the DCPMM
    CHECK_RESULT(DcpmmCmd(pDimm, pCmd, DCPMM_TIMEOUT_INTERVAL, FisOverSmbus), Finish);

  } else {
    // DDRT:  Use the bios DCPMM protocol to send commands to the DCPMM
    CHECK_RESULT(DcpmmCmd(pDimm, pCmd, DCPMM_TIMEOUT_INTERVAL, FisOverDdrt), Finish);
  }
#endif // OS_BUILD

Finish:
  return ReturnCode;
}

/**
  Makes Bios emulated pass through call and acquires the DCPMM Boot
  Status Register

  @param[in] pDimm The DCPMM to retrieve BSR from
  @param[out] pBsrValue Pointer to memory to copy BSR value to

  @retval EFI_SUCCESS: Success
  @retval EFI_OUT_OF_RESOURCES: memory allocation failure
**/
EFI_STATUS
EFIAPI
FwCmdGetBsr(
  IN     DIMM *pDimm,
     OUT UINT64 *pBsrValue
)
{
  NVM_FW_CMD *pFwCmd = NULL;
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pBsrValue == NULL) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }


  CHECK_RESULT_MALLOC(pFwCmd, AllocateZeroPool(sizeof(*pFwCmd)), Finish);

  pFwCmd->DimmID = pDimm->DimmID;
  pFwCmd->Opcode = PtEmulatedBiosCommands;
  pFwCmd->SubOpcode = SubopGetBSR;
  pFwCmd->OutputPayloadSize = sizeof(unsigned long long);
  ReturnCode = PassThru(pDimm, pFwCmd, PT_TIMEOUT_INTERVAL);
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_DBG("Error detected when sending BIOS emulated GetBSR command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pFwCmd, ReturnCode);
    goto Finish;
  }

  CopyMem_S(pBsrValue, sizeof(*pBsrValue), pFwCmd->OutPayload, sizeof(UINT64));
  NVDIMM_ERR("Bsr received is 0x%x", *pBsrValue);

Finish:
  FREE_POOL_SAFE(pFwCmd);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;
}

/**
  Gather boot status register value and populate the boot status bitmask

  @param[in] pDimm to retrieve DDRT Training status from
  @param[out] pBsr BSR Boot Status Register to retrieve and convert to bitmask
  @param[in, out] pBootStatusBitmask Pointer to the boot status bitmask

  @retval EFI_SUCCESS Success
  @retval EFI_INVALID_PARAMETER One or more parameters are NULL
**/

EFI_STATUS
PopulateDimmBsrAndBootStatusBitmask(
  IN     DIMM *pDimm,
     OUT DIMM_BSR *pBsr,
     OUT UINT16 *pBootStatusBitmask OPTIONAL
)
{
  EFI_STATUS ReturnCode = EFI_SUCCESS;

  NVDIMM_ENTRY();

  // Some values are optional to limit fw calls
  // However to populate BootStatusBitmask correctly we need
  // to populate Bsr. So make that a requirement
  if (NULL == pBsr) {
    ReturnCode = EFI_INVALID_PARAMETER;
    goto Finish;
  }

  ZeroMem(pBsr, sizeof(DIMM_BSR));

  CHECK_RESULT(FwCmdGetBsr(pDimm, &(pBsr->AsUint64)), Finish);

  if ((pBsr->AsUint64 == MAX_UINT64_VALUE) || (pBsr->AsUint64 == 0)) {
    // Invalid values returned in BSR. Return failure
    ReturnCode = EFI_NO_RESPONSE;
    goto Finish;
  }

  // If pBootStatusBitmask is unspecified, we should only populate BSR
  if (NULL == pBootStatusBitmask) {
    goto Finish;
  }

  // Clear caller's value before use
  *pBootStatusBitmask = DIMM_BOOT_STATUS_NORMAL;

  // This section is notably missing the DIMM_BOOT_STATUS_DDRT_NOT_READY
  // bit. We use this to determine if DDRT is accessible to us, and if only
  // the BSR is used, it's not the full picture. It's more accurate
  // to test the interface directly, so it's done in InitializeDimm() currently.
  if (pBsr->Separated_Current_FIS.MR == DIMM_BSR_MEDIA_NOT_TRAINED) {
    *pBootStatusBitmask |= DIMM_BOOT_STATUS_MEDIA_NOT_READY;
  }
  if (pBsr->Separated_Current_FIS.MR == DIMM_BSR_MEDIA_ERROR) {
    *pBootStatusBitmask |= DIMM_BOOT_STATUS_MEDIA_ERROR;
  }
  if (pBsr->Separated_Current_FIS.MD == DIMM_BSR_MEDIA_DISABLED) {
    *pBootStatusBitmask |= DIMM_BOOT_STATUS_MEDIA_DISABLED;
  }
  if (pBsr->Separated_Current_FIS.MBR == DIMM_BSR_MAILBOX_NOT_READY) {
    *pBootStatusBitmask |= DIMM_BOOT_STATUS_MAILBOX_NOT_READY;
  }
  if (pBsr->Separated_Current_FIS.RR == DIMM_BSR_REBOOT_REQUIRED) {
    *pBootStatusBitmask |= DIMM_BOOT_STATUS_REBOOT_REQUIRED;
  }

Finish:
  NVDIMM_EXIT_I64(ReturnCode);

  return ReturnCode;
}

#ifndef OS_BUILD
/**
  Passthrough FIS command by Dcpmm BIOS protocol.

  @param[in] pDimm Target DIMM structure pointer
  @param[in, out] pCmd Firmware command structure pointer
  @param[in] Timeout Optional command timeout in microseconds
  @param[in] DcpmmInterface Interface for FIS request

  @retval EFI_SUCCESS Success
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
  @retval EFI_INVALID_PARAMETER input parameter null
**/
EFI_STATUS
DcpmmCmd(
  IN     struct _DIMM *pDimm,
  IN OUT NVM_FW_CMD *pCmd,
  IN     UINT32 Timeout OPTIONAL,
  IN     DCPMM_FIS_INTERFACE DcpmmInterface
)
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  DCPMM_FIS_INPUT *pInputPayload = NULL;
  DCPMM_FIS_OUTPUT *pOutputPayload = NULL;
  DCPMM_FIS_OUTPUT *pLargePayloadInfo = NULL;
  UINT16 Command = 0;

  NVDIMM_ENTRY();

  if(pDimm == NULL || pCmd == NULL) {
    goto Finish;
  }

  pInputPayload = (DCPMM_FIS_INPUT *)AllocateZeroPool(sizeof(*pInputPayload) + pCmd->InputPayloadSize);
  if (pInputPayload == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  if (pCmd->OutputPayloadSize > 0) {
    pOutputPayload = (DCPMM_FIS_OUTPUT *)AllocateZeroPool(sizeof(*pOutputPayload) + pCmd->OutputPayloadSize);
    if (pOutputPayload == NULL) {
      ReturnCode = EFI_OUT_OF_RESOURCES;
      goto Finish;
    }
  }

  /** Get large payload info **/
  if (pCmd->LargeInputPayloadSize > 0 || pCmd->LargeOutputPayloadSize > 0) {
    pLargePayloadInfo = (DCPMM_FIS_OUTPUT *)AllocateZeroPool(sizeof(*pLargePayloadInfo));
    if (pLargePayloadInfo == NULL) {
      ReturnCode = EFI_OUT_OF_RESOURCES;
      goto Finish;
    }

    ReturnCode = DcpmmLargePayloadInfo(pDimm, Timeout, DcpmmInterface, pLargePayloadInfo, &pCmd->Status);
    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_ERR("Error detected when sending DcpmmLargePayloadInfo");
      FW_CMD_ERROR_TO_EFI_STATUS(pCmd, ReturnCode);
      goto Finish;
    }
  }

  /** Prepare input payload structure **/
  Command = (UINT16)pCmd->SubOpcode << EXT_SUB_OP_SHIFT | (UINT16)pCmd->Opcode;
  pInputPayload->Head.FisCmd = Command;
  pInputPayload->Head.DataSize = pCmd->InputPayloadSize;
  CopyMem_S(pInputPayload->Data.Fis.Payload, pCmd->InputPayloadSize, pCmd->InputPayload, pCmd->InputPayloadSize);

  /** Prepare output payload structure **/
  if (pCmd->OutputPayloadSize > 0) {
    pOutputPayload->Head.DataSize = pCmd->OutputPayloadSize;
  }

  /** Write data to large input payload **/
  if (pCmd->LargeInputPayloadSize > 0) {
    if (pCmd->LargeInputPayloadSize > pLargePayloadInfo->Data.LpInfo.InpPayloadSize) {
      NVDIMM_ERR("Available large input payload size is not enough");
      ReturnCode = EFI_INVALID_PARAMETER;
      goto Finish;
    } else {
      ReturnCode = DcpmmLargePayloadWrite(pDimm, pCmd->LargeInputPayload, pCmd->LargeInputPayloadSize,
        pLargePayloadInfo->Data.LpInfo.DataChunkSize, Timeout, DcpmmInterface, &pCmd->Status);
      if (EFI_ERROR(ReturnCode)) {
        NVDIMM_ERR("Error detected when sending DcpmmLargePayloadWrite");
        FW_CMD_ERROR_TO_EFI_STATUS(pCmd, ReturnCode);
        goto Finish;
      }
    }
  }

  ReturnCode = gNvmDimmData->pDcpmmProtocol->DcpmmFisRequest(
    DcpmmInterface,
    pDimm->DeviceHandle.AsUint32,
    pInputPayload,
    pOutputPayload,
    Timeout,
    &pCmd->Status
  );
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_WARN("Error detected when sending DcpmmFisRequest command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    FW_CMD_ERROR_TO_EFI_STATUS(pCmd, ReturnCode);
    goto Finish;
  }

  if (pCmd->OutputPayloadSize > 0) {
    CopyMem_S(pCmd->OutPayload, pCmd->OutputPayloadSize, pOutputPayload->Data.Fis.Payload, pCmd->OutputPayloadSize);
  }

  /** Read data from large output payload **/
  if (pCmd->LargeOutputPayloadSize > 0) {
    if (pCmd->LargeOutputPayloadSize > pLargePayloadInfo->Data.LpInfo.OutPayloadSize) {
      NVDIMM_ERR("Data in large output payload cannot be fully filled");
      ReturnCode = EFI_INVALID_PARAMETER;
      goto Finish;
    } else {
      ReturnCode = DcpmmLargePayloadRead(pDimm, pCmd->LargeOutputPayloadSize, pLargePayloadInfo->Data.LpInfo.DataChunkSize,
        Timeout, DcpmmInterface, pCmd->LargeOutputPayload, &pCmd->Status);
      if (EFI_ERROR(ReturnCode)) {
        NVDIMM_ERR("Error detected when sending DcpmmLargePayloadRead");
        FW_CMD_ERROR_TO_EFI_STATUS(pCmd, ReturnCode);
        goto Finish;
      }
    }
  }

Finish:
  FREE_POOL_SAFE(pInputPayload);
  FREE_POOL_SAFE(pOutputPayload);
  FREE_POOL_SAFE(pLargePayloadInfo);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;

}

/**
  Get large payload info by Dcpmm BIOS protocol.

  @param[in] pDimm Target DIMM structure pointer
  @param[in] Timeout Optional command timeout in microseconds
  @param[in] DcpmmInterface Interface for FIS request
  @param[out] pOutput Large payload info output data buffer
  @param[out] pStatus FIS request status

  @retval EFI_SUCCESS Success
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
  @retval EFI_INVALID_PARAMETER input parameter null
**/
EFI_STATUS
DcpmmLargePayloadInfo(
  IN     struct _DIMM *pDimm,
  IN     UINT32 Timeout OPTIONAL,
  IN     DCPMM_FIS_INTERFACE DcpmmInterface,
     OUT DCPMM_FIS_OUTPUT *pOutput,
     OUT UINT8 *pStatus
)
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  DCPMM_FIS_INPUT *pLargeInputPayload = NULL;

  NVDIMM_ENTRY();

  if (pDimm == NULL || pOutput == NULL || pStatus == NULL) {
    goto Finish;
  }

  pLargeInputPayload = (DCPMM_FIS_INPUT *)AllocateZeroPool(sizeof(*pLargeInputPayload));
  if (pLargeInputPayload == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pLargeInputPayload->Head.FisCmd = (UINT16)FIS_CMD_GET_LP_MB_INFO;
  pLargeInputPayload->Head.DataSize = 0;

  pOutput->Head.DataSize = sizeof(pOutput->Data.LpInfo);

  ReturnCode = gNvmDimmData->pDcpmmProtocol->DcpmmFisRequest(
    DcpmmInterface,
    pDimm->DeviceHandle.AsUint32,
    pLargeInputPayload,
    pOutput,
    Timeout,
    pStatus
  );
  if (EFI_ERROR(ReturnCode)) {
    NVDIMM_ERR("Error detected when sending DcpmmFisRequest command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
    goto Finish;
  }

Finish:
  FREE_POOL_SAFE(pLargeInputPayload);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;

}

/**
  Write large payload by Dcpmm BIOS protocol.

  @param[in] pDimm Target DIMM structure pointer
  @param[in] pInput Input data buffer
  @param[in] InputSize Total input data size
  @param[in] MaxChunkSize Maximum chunk of data to write
  @param[in] Timeout Optional command timeout in microseconds
  @param[in] DcpmmInterface Interface for FIS request
  @param[out] pStatus FIS request status

  @retval EFI_SUCCESS Success
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
  @retval EFI_INVALID_PARAMETER input parameter null
**/
EFI_STATUS
DcpmmLargePayloadWrite(
  IN     struct _DIMM *pDimm,
  IN     UINT8 *pInput,
  IN     UINT32 InputSize,
  IN     UINT32 MaxChunkSize,
  IN     UINT32 Timeout OPTIONAL,
  IN     DCPMM_FIS_INTERFACE DcpmmInterface,
     OUT UINT8 *pStatus
)
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  DCPMM_FIS_INPUT *pLargeInputPayload = NULL;
  UINT32 Offset = 0;
  UINT32 CurrentChunkSize = MaxChunkSize < InputSize ? MaxChunkSize : InputSize;

  NVDIMM_ENTRY();

  if (InputSize <= 0 || pDimm == NULL || pInput == NULL || pStatus == NULL) {
    goto Finish;
  }

  pLargeInputPayload = (DCPMM_FIS_INPUT *)AllocateZeroPool(sizeof(*pLargeInputPayload) + CurrentChunkSize);
  if (pLargeInputPayload == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pLargeInputPayload->Head.FisCmd = (UINT16)FIS_CMD_WRITE_LP_INPUT_MB;
  pLargeInputPayload->Head.DataSize = sizeof(pLargeInputPayload->Data.LpWrite) + MaxChunkSize;
  for (Offset = 0; Offset < InputSize; Offset += MaxChunkSize) {
    pLargeInputPayload->Data.LpWrite.Offset = Offset;
    if (InputSize - Offset < MaxChunkSize) {
      CurrentChunkSize = InputSize - Offset;
      pLargeInputPayload->Head.DataSize = sizeof(pLargeInputPayload->Data.LpWrite) + CurrentChunkSize;
      pLargeInputPayload->Data.LpWrite.Size = CurrentChunkSize;
    }
    CopyMem_S(pLargeInputPayload->Data.LpWrite.Payload, CurrentChunkSize, pInput + Offset, CurrentChunkSize);

    ReturnCode = gNvmDimmData->pDcpmmProtocol->DcpmmFisRequest(
      DcpmmInterface,
      pDimm->DeviceHandle.AsUint32,
      pLargeInputPayload,
      NULL,
      Timeout,
      pStatus
    );
    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_ERR("Error detected when sending DcpmmFisRequest command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
      goto Finish;
    }
  }


Finish:
  FREE_POOL_SAFE(pLargeInputPayload);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;

}

/**
  Read large payload by Dcpmm BIOS protocol.

  @param[in] pDimm Target DIMM structure pointer
  @param[in] OutputSize Total output data size
  @param[in] MaxChunkSize Maximum chunk of data to read
  @param[in] Timeout Optional command timeout in microseconds
  @param[in] DcpmmInterface Interface for FIS request
  @param[out] pOutput Output data buffer
  @param[out] pStatus FIS request status pointer

  @retval EFI_SUCCESS Success
  @retval EFI_OUT_OF_RESOURCES memory allocation failure
  @retval EFI_INVALID_PARAMETER input parameter null
**/
EFI_STATUS
DcpmmLargePayloadRead(
  IN     struct _DIMM *pDimm,
  IN     UINT32 OutputSize,
  IN     UINT32 MaxChunkSize,
  IN     UINT32 Timeout OPTIONAL,
  IN     DCPMM_FIS_INTERFACE DcpmmInterface,
  IN OUT UINT8 *pOutput,
     OUT UINT8 *pStatus
)
{
  EFI_STATUS ReturnCode = EFI_INVALID_PARAMETER;
  DCPMM_FIS_INPUT *pLargeInputPayload = NULL;
  DCPMM_FIS_OUTPUT *pLargeOutputPayload = NULL;
  UINT32 Offset = 0;
  UINT32 CurrentChunkSize = MaxChunkSize < OutputSize ? MaxChunkSize : OutputSize;

  NVDIMM_ENTRY();

  if (OutputSize <= 0 || pDimm == NULL || pOutput == NULL || pStatus == NULL) {
    goto Finish;
  }

  pLargeInputPayload = (DCPMM_FIS_INPUT *)AllocateZeroPool(sizeof(*pLargeInputPayload));
  if (pLargeInputPayload == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pLargeOutputPayload = (DCPMM_FIS_OUTPUT *)AllocateZeroPool(
    sizeof(*pLargeOutputPayload) + CurrentChunkSize
  );
  if (pLargeOutputPayload == NULL) {
    ReturnCode = EFI_OUT_OF_RESOURCES;
    goto Finish;
  }

  pLargeInputPayload->Head.FisCmd = (UINT16)FIS_CMD_READ_LP_OUTPUT_MB;
  pLargeInputPayload->Head.DataSize = sizeof(pLargeInputPayload->Data.LpRead);
  pLargeInputPayload->Data.LpRead.Size = MaxChunkSize;
  for (Offset = 0; Offset < OutputSize; Offset += MaxChunkSize) {
    pLargeInputPayload->Data.LpRead.Offset = Offset;
    if (OutputSize - Offset < MaxChunkSize) {
      CurrentChunkSize = OutputSize - Offset;
      pLargeInputPayload->Data.LpRead.Size = CurrentChunkSize;
    }

    pLargeOutputPayload->Head.DataSize = CurrentChunkSize;

    ReturnCode = gNvmDimmData->pDcpmmProtocol->DcpmmFisRequest(
      DcpmmInterface,
      pDimm->DeviceHandle.AsUint32,
      pLargeInputPayload,
      pLargeOutputPayload,
      Timeout,
      pStatus
    );
    if (EFI_ERROR(ReturnCode)) {
      NVDIMM_ERR("Error detected when sending DcpmmFisRequest command (RC = " FORMAT_EFI_STATUS ")", ReturnCode);
      goto Finish;
    }
    CopyMem_S(pOutput + Offset, CurrentChunkSize, &pLargeOutputPayload->Data.LpData, CurrentChunkSize);
  }

Finish:
  FREE_POOL_SAFE(pLargeInputPayload);
  FREE_POOL_SAFE(pLargeOutputPayload);
  NVDIMM_EXIT_I64(ReturnCode);
  return ReturnCode;

}
#endif // !OS_BUILD
