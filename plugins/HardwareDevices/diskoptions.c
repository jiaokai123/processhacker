/*
 * Process Hacker Plugins -
 *   Hardware Devices Plugin
 *
 * Copyright (C) 2015-2016 dmex
 *
 * This file is part of Process Hacker.
 *
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "devices.h"
#include <Setupapi.h>

#define ITEM_CHECKED (INDEXTOSTATEIMAGEMASK(2))
#define ITEM_UNCHECKED (INDEXTOSTATEIMAGEMASK(1))

static _EnableThemeDialogTexture EnableThemeDialogTexture_I = NULL;

typedef struct _DISK_ENUM_ENTRY
{
    ULONG DeviceIndex;
    BOOLEAN DevicePresent;
    PPH_STRING DevicePath;
    PPH_STRING DeviceName;
    PPH_STRING DeviceMountPoints;
} DISK_ENUM_ENTRY, *PDISK_ENUM_ENTRY;

static int __cdecl DiskEntryCompareFunction(
    _In_ const void *elem1,
    _In_ const void *elem2
    )
{
    PDISK_ENUM_ENTRY entry1 = *(PDISK_ENUM_ENTRY *)elem1;
    PDISK_ENUM_ENTRY entry2 = *(PDISK_ENUM_ENTRY *)elem2;

    return uint64cmp(entry1->DeviceIndex, entry2->DeviceIndex);
}

VOID DiskDrivesLoadList(
    VOID
    )
{
    PPH_STRING settingsString;
    PH_STRINGREF remaining;

    settingsString = PhaGetStringSetting(SETTING_NAME_DISK_LIST);
    remaining = settingsString->sr;

    while (remaining.Length != 0)
    {
        PH_STRINGREF part;
        DV_DISK_ID id;
        PDV_DISK_ENTRY entry;

        if (remaining.Length == 0)
            break;

        PhSplitStringRefAtChar(&remaining, ',', &part, &remaining);

        InitializeDiskId(&id, PhCreateString2(&part));
        entry = CreateDiskEntry(&id);
        DeleteDiskId(&id);

        entry->UserReference = TRUE;
    }
}

VOID DiskDrivesSaveList(
    VOID
    )
{
    PH_STRING_BUILDER stringBuilder;
    PPH_STRING settingsString;

    PhInitializeStringBuilder(&stringBuilder, 260);

    PhAcquireQueuedLockShared(&DiskDrivesListLock);

    for (ULONG i = 0; i < DiskDrivesList->Count; i++)
    {
        PDV_DISK_ENTRY entry = PhReferenceObjectSafe(DiskDrivesList->Items[i]);

        if (!entry)
            continue;

        if (entry->UserReference)
        {
            PhAppendFormatStringBuilder(
                &stringBuilder,
                L"%s,",
                entry->Id.DevicePath->Buffer // This value is SAFE and does not change.
                );
        }

        PhDereferenceObjectDeferDelete(entry);
    }

    PhReleaseQueuedLockShared(&DiskDrivesListLock);

    if (stringBuilder.String->Length != 0)
        PhRemoveEndStringBuilder(&stringBuilder, 1);

    settingsString = PH_AUTO(PhFinalStringBuilderString(&stringBuilder));
    PhSetStringSetting2(SETTING_NAME_DISK_LIST, &settingsString->sr);
}

BOOLEAN FindDiskEntry(
    _In_ PDV_DISK_ID Id,
    _In_ BOOLEAN RemoveUserReference
    )
{
    BOOLEAN found = FALSE;

    PhAcquireQueuedLockShared(&DiskDrivesListLock);

    for (ULONG i = 0; i < DiskDrivesList->Count; i++)
    {
        PDV_DISK_ENTRY currentEntry = PhReferenceObjectSafe(DiskDrivesList->Items[i]);

        if (!currentEntry)
            continue;

        found = EquivalentDiskId(&currentEntry->Id, Id);

        if (found)
        {
            if (RemoveUserReference)
            {
                if (currentEntry->UserReference)
                {
                    PhDereferenceObjectDeferDelete(currentEntry);
                    currentEntry->UserReference = FALSE;
                }
            }

            PhDereferenceObjectDeferDelete(currentEntry);

            break;
        }
        else
        {
            PhDereferenceObjectDeferDelete(currentEntry);
        }
    }

    PhReleaseQueuedLockShared(&DiskDrivesListLock);

    return found;
}

static INT AddListViewItemGroupId(
    _In_ HWND ListViewHandle,
    _In_ INT GroupId,
    _In_ INT Index,
    _In_ PWSTR Text,
    _In_opt_ PVOID Param
    )
{
    LVITEM item;

    item.mask = LVIF_TEXT | LVIF_GROUPID | LVIF_PARAM;
    item.iGroupId = GroupId;
    item.iItem = Index;
    item.iSubItem = 0;
    item.pszText = Text;
    item.lParam = (LPARAM)Param;

    return ListView_InsertItem(ListViewHandle, &item);
}

static VOID AddListViewGroup(
    _In_ HWND ListViewHandle,
    _In_ INT Index,
    _In_ PWSTR Text
    )
{
    LVGROUP group = { LVGROUP_V5_SIZE };
    group.mask = LVGF_HEADER | LVGF_GROUPID;

    if (WindowsVersion >= WINDOWS_VISTA)
    {
        group.cbSize = sizeof(LVGROUP);
        group.mask = group.mask | LVGF_ALIGN | LVGF_STATE;
        group.uAlign = LVGA_HEADER_LEFT;
        group.state = LVGS_COLLAPSIBLE;
    }

    group.iGroupId = Index;
    group.pszHeader = Text;

    ListView_InsertGroup(ListViewHandle, INT_MAX, &group);
}

VOID AddDiskDriveToListView(
    _In_ PDV_DISK_OPTIONS_CONTEXT Context,
    _In_ BOOLEAN DiskPresent,
    _In_ PPH_STRING DiskPath,
    _In_ PPH_STRING DiskName
    )
{
    DV_DISK_ID adapterId;
    INT lvItemIndex;
    BOOLEAN found = FALSE;
    PDV_DISK_ID newId = NULL;

    InitializeDiskId(&adapterId, DiskPath);

    for (ULONG i = 0; i < DiskDrivesList->Count; i++)
    {
        PDV_DISK_ENTRY entry = PhReferenceObjectSafe(DiskDrivesList->Items[i]);

        if (!entry)
            continue;

        if (EquivalentDiskId(&entry->Id, &adapterId))
        {
            newId = PhAllocate(sizeof(DV_DISK_ID));
            CopyDiskId(newId, &entry->Id);

            if (entry->UserReference)
                found = TRUE;
        }

        PhDereferenceObjectDeferDelete(entry);

        if (newId)
            break;
    }

    if (!newId)
    {
        newId = PhAllocate(sizeof(DV_DISK_ID));
        CopyDiskId(newId, &adapterId);
        PhMoveReference(&newId->DevicePath, DiskPath);
    }

    lvItemIndex = AddListViewItemGroupId(
        Context->ListViewHandle,
        DiskPresent ? 0 : 1,
        MAXINT,
        DiskName->Buffer,
        newId
        );

    if (found)
        ListView_SetItemState(Context->ListViewHandle, lvItemIndex, ITEM_CHECKED, LVIS_STATEIMAGEMASK);

    DeleteDiskId(&adapterId);
}

VOID FindDiskDrives(
    _In_ PDV_DISK_OPTIONS_CONTEXT Context
    )
{
    PPH_LIST deviceList;
    HDEVINFO deviceInfoHandle;
    SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
    SP_DEVINFO_DATA deviceInfoData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA deviceInterfaceDetail = NULL;
    ULONG deviceInfoLength = 0;

    if ((deviceInfoHandle = SetupDiGetClassDevs(
        &GUID_DEVINTERFACE_DISK,
        NULL,
        NULL,
        DIGCF_DEVICEINTERFACE
        )) == INVALID_HANDLE_VALUE)
    {
        return;
    }

    deviceList = PH_AUTO(PhCreateList(1));
    Context->EnumeratingDisks = TRUE;

    for (ULONG i = 0; i < 1000; i++)
    {
        memset(&deviceInterfaceData, 0, sizeof(SP_DEVICE_INTERFACE_DATA));
        deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        if (!SetupDiEnumDeviceInterfaces(deviceInfoHandle, 0, &GUID_DEVINTERFACE_DISK, i, &deviceInterfaceData))
            break;

        memset(&deviceInfoData, 0, sizeof(SP_DEVINFO_DATA));
        deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

        if (SetupDiGetDeviceInterfaceDetail(
            deviceInfoHandle,
            &deviceInterfaceData,
            0,
            0,
            &deviceInfoLength,
            &deviceInfoData
            ) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            continue;
        }

        deviceInterfaceDetail = PhAllocate(deviceInfoLength);
        deviceInterfaceDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (SetupDiGetDeviceInterfaceDetail(
            deviceInfoHandle, 
            &deviceInterfaceData, 
            deviceInterfaceDetail, 
            deviceInfoLength, 
            &deviceInfoLength, 
            &deviceInfoData
            ))
        {
            HANDLE deviceHandle;
            PDISK_ENUM_ENTRY diskEntry;
            WCHAR diskFriendlyName[MAX_PATH] = L""; 
            
            diskEntry = PhAllocate(sizeof(DISK_ENUM_ENTRY));
            memset(diskEntry, 0, sizeof(DISK_ENUM_ENTRY));

            SetupDiGetDeviceRegistryProperty(
                deviceInfoHandle,
                &deviceInfoData,
                SPDRP_FRIENDLYNAME,
                NULL,
                (PBYTE)diskFriendlyName,
                ARRAYSIZE(diskFriendlyName),
                NULL
                );

            diskEntry->DeviceIndex = ULONG_MAX; // Note: Do not initialize to zero.
            diskEntry->DeviceName = PhCreateString(diskFriendlyName);
            diskEntry->DevicePath = PhCreateString(deviceInterfaceDetail->DevicePath);

            if (NT_SUCCESS(DiskDriveCreateHandle(
                &deviceHandle,
                diskEntry->DevicePath
                )))
            {
                ULONG diskIndex = ULONG_MAX; // Note: Do not initialize to zero

                if (NT_SUCCESS(DiskDriveQueryDeviceTypeAndNumber(
                    deviceHandle, 
                    &diskIndex,
                    NULL
                    )))
                {
                    PPH_STRING diskMountPoints = PH_AUTO_T(PH_STRING, DiskDriveQueryDosMountPoints(diskIndex));
                    
                    diskEntry->DeviceIndex = diskIndex;
                    diskEntry->DevicePresent = TRUE;

                    if (!PhIsNullOrEmptyString(diskMountPoints))
                    {
                        diskEntry->DeviceMountPoints = PhFormatString(
                            L"Disk %lu (%s) [%s]",
                            diskIndex,
                            diskMountPoints->Buffer,
                            diskFriendlyName
                            );
                    }
                    else
                    {
                        diskEntry->DeviceMountPoints = PhFormatString(
                            L"Disk %lu [%s]",
                            diskIndex,
                            diskFriendlyName
                            );
                    }
                }

                NtClose(deviceHandle);
            }

            PhAddItemList(deviceList, diskEntry);
        }

        PhFree(deviceInterfaceDetail);
    }

    SetupDiDestroyDeviceInfoList(deviceInfoHandle);

    // Sort the entries
    qsort(deviceList->Items, deviceList->Count, sizeof(PVOID), DiskEntryCompareFunction);

    PhAcquireQueuedLockShared(&DiskDrivesListLock);

    for (ULONG i = 0; i < deviceList->Count; i++)
    {
        PDISK_ENUM_ENTRY entry = deviceList->Items[i];

        AddDiskDriveToListView(
            Context,
            entry->DevicePresent,
            entry->DevicePath,
            entry->DeviceMountPoints ? entry->DeviceMountPoints : entry->DeviceName
            );

        if (entry->DeviceMountPoints)
            PhDereferenceObject(entry->DeviceMountPoints);
        if (entry->DeviceName)
            PhDereferenceObject(entry->DeviceName);
        // Note: DevicePath is disposed by WM_DESTROY.

        PhFree(entry);
    }

    PhReleaseQueuedLockShared(&DiskDrivesListLock);

    Context->EnumeratingDisks = FALSE;
}

INT_PTR CALLBACK DiskDriveOptionsDlgProc(
    _In_ HWND hwndDlg,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    PDV_DISK_OPTIONS_CONTEXT context = NULL;

    if (uMsg == WM_INITDIALOG)
    {
        context = (PDV_DISK_OPTIONS_CONTEXT)PhAllocate(sizeof(DV_DISK_OPTIONS_CONTEXT));
        memset(context, 0, sizeof(DV_DISK_OPTIONS_CONTEXT));

        SetProp(hwndDlg, L"Context", (HANDLE)context);
    }
    else
    {
        context = (PDV_DISK_OPTIONS_CONTEXT)GetProp(hwndDlg, L"Context");

        if (uMsg == WM_DESTROY)
        {
            if (context->OptionsChanged)
                DiskDrivesSaveList();

            RemoveProp(hwndDlg, L"Context");
            PhFree(context);
        }
    }

    if (context == NULL)
        return FALSE;

    switch (uMsg)
    {
    case WM_INITDIALOG:
        {
            // TODO: Why does this need to use GetParent?
            PhCenterWindow(GetParent(hwndDlg), GetParent(GetParent(hwndDlg)));

            context->ListViewHandle = GetDlgItem(hwndDlg, IDC_DISKDRIVE_LISTVIEW);

            PhSetListViewStyle(context->ListViewHandle, FALSE, TRUE);
            ListView_SetExtendedListViewStyleEx(context->ListViewHandle, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
            PhSetControlTheme(context->ListViewHandle, L"explorer");
            PhAddListViewColumn(context->ListViewHandle, 0, 0, 0, LVCFMT_LEFT, 350, L"Disk Drives");
            PhSetExtendedListView(context->ListViewHandle);

            ListView_EnableGroupView(context->ListViewHandle, TRUE);
            AddListViewGroup(context->ListViewHandle, 0, L"Connected");
            AddListViewGroup(context->ListViewHandle, 1, L"Disconnected");

            FindDiskDrives(context);

            context->OptionsChanged = FALSE;

            if (!EnableThemeDialogTexture_I)
                EnableThemeDialogTexture_I = PhGetModuleProcAddress(L"uxtheme.dll", "EnableThemeDialogTexture");

            if (EnableThemeDialogTexture_I)
                EnableThemeDialogTexture_I(hwndDlg, ETDT_ENABLETAB);
        }
        break;
    case WM_NOTIFY:
        {
            LPNMHDR header = (LPNMHDR)lParam;

            if (header->code == LVN_ITEMCHANGED)
            {
                LPNM_LISTVIEW listView = (LPNM_LISTVIEW)lParam;

                if (context->EnumeratingDisks)
                    break;

                if (listView->uChanged & LVIF_STATE)
                {
                    switch (listView->uNewState & 0x3000)
                    {
                    case 0x2000: // checked
                        {
                            PDV_DISK_ID param = (PDV_DISK_ID)listView->lParam;

                            if (!FindDiskEntry(param, FALSE))
                            {
                                PDV_DISK_ENTRY entry;

                                entry = CreateDiskEntry(param);
                                entry->UserReference = TRUE;
                            }

                            context->OptionsChanged = TRUE;
                        }
                        break;
                    case 0x1000: // unchecked
                        {
                            PDV_DISK_ID param = (PDV_DISK_ID)listView->lParam;

                            FindDiskEntry(param, TRUE);

                            context->OptionsChanged = TRUE;
                        }
                        break;
                    }
                }
            }
        }
        break;
    }

    return FALSE;
}