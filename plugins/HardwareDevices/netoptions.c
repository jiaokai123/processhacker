/*
 * Process Hacker Plugins -
 *   Hardware Devices Plugin
 *
 * Copyright (C) 2015-2016 dmex
 * Copyright (C) 2016 wj32
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

#define INITGUID
#include "devices.h"
#include <Setupapi.h>
#include <ndisguid.h>

#define ITEM_CHECKED (INDEXTOSTATEIMAGEMASK(2))
#define ITEM_UNCHECKED (INDEXTOSTATEIMAGEMASK(1))

typedef struct _NET_ENUM_ENTRY
{
    BOOLEAN DevicePresent;
    IF_LUID DeviceLuid;
    PPH_STRING DeviceGuid;
    PPH_STRING DeviceName;
} NET_ENUM_ENTRY, *PNET_ENUM_ENTRY;

static int __cdecl AdapterEntryCompareFunction(
    _In_ const void *elem1,
    _In_ const void *elem2
    )
{
    PNET_ENUM_ENTRY entry1 = *(PNET_ENUM_ENTRY *)elem1;
    PNET_ENUM_ENTRY entry2 = *(PNET_ENUM_ENTRY *)elem2;

    return uint64cmp(entry1->DeviceLuid.Value, entry2->DeviceLuid.Value);
}

VOID NetAdaptersLoadList(
    VOID
    )
{
    PPH_STRING settingsString;
    PH_STRINGREF remaining;

    settingsString = PhaGetStringSetting(SETTING_NAME_INTERFACE_LIST);
    remaining = settingsString->sr;

    while (remaining.Length != 0)
    {
        ULONG64 ifindex;
        ULONG64 luid64;
        PH_STRINGREF part1;
        PH_STRINGREF part2;
        PH_STRINGREF part3;
        IF_LUID ifLuid;
        DV_NETADAPTER_ID id;
        PDV_NETADAPTER_ENTRY entry;

        if (remaining.Length == 0)
            break;

        PhSplitStringRefAtChar(&remaining, ',', &part1, &remaining);
        PhSplitStringRefAtChar(&remaining, ',', &part2, &remaining);
        PhSplitStringRefAtChar(&remaining, ',', &part3, &remaining);

        PhStringToInteger64(&part1, 10, &ifindex);
        PhStringToInteger64(&part2, 10, &luid64);

        ifLuid.Value = luid64;
        InitializeNetAdapterId(&id, (IF_INDEX)ifindex, ifLuid, PhCreateString2(&part3));
        entry = CreateNetAdapterEntry(&id);
        DeleteNetAdapterId(&id);

        entry->UserReference = TRUE;
    }
}

VOID NetAdaptersSaveList(
    VOID
    )
{
    PH_STRING_BUILDER stringBuilder;
    PPH_STRING settingsString;

    PhInitializeStringBuilder(&stringBuilder, 260);

    PhAcquireQueuedLockShared(&NetworkAdaptersListLock);

    for (ULONG i = 0; i < NetworkAdaptersList->Count; i++)
    {
        PDV_NETADAPTER_ENTRY entry = PhReferenceObjectSafe(NetworkAdaptersList->Items[i]);

        if (!entry)
            continue;

        if (entry->UserReference)
        {
            PhAppendFormatStringBuilder(
                &stringBuilder,
                L"%lu,%I64u,%s,",
                entry->Id.InterfaceIndex,    // This value is UNSAFE and may change after reboot.
                entry->Id.InterfaceLuid.Value, // This value is SAFE and does not change (Vista+).
                entry->Id.InterfaceGuid->Buffer
                );
        }

        PhDereferenceObjectDeferDelete(entry);
    }

    PhReleaseQueuedLockShared(&NetworkAdaptersListLock);

    if (stringBuilder.String->Length != 0)
        PhRemoveEndStringBuilder(&stringBuilder, 1);

    settingsString = PH_AUTO(PhFinalStringBuilderString(&stringBuilder));
    PhSetStringSetting2(SETTING_NAME_INTERFACE_LIST, &settingsString->sr);
}

BOOLEAN FindAdapterEntry(
    _In_ PDV_NETADAPTER_ID Id,
    _In_ BOOLEAN RemoveUserReference
    )
{
    BOOLEAN found = FALSE;

    PhAcquireQueuedLockShared(&NetworkAdaptersListLock);

    for (ULONG i = 0; i < NetworkAdaptersList->Count; i++)
    {
        PDV_NETADAPTER_ENTRY currentEntry = PhReferenceObjectSafe(NetworkAdaptersList->Items[i]);

        if (!currentEntry)
            continue;

        found = EquivalentNetAdapterId(&currentEntry->Id, Id);

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

    PhReleaseQueuedLockShared(&NetworkAdaptersListLock);

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

    item.mask = LVIF_TEXT | LVIF_PARAM | (WindowsVersion >= WINDOWS_VISTA ? LVIF_GROUPID : 0);
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
    LVGROUP group;

    group.cbSize = sizeof(LVGROUP);
    group.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_ALIGN | LVGF_STATE;
    group.mask = group.mask;
    group.uAlign = LVGA_HEADER_LEFT;
    group.state = LVGS_COLLAPSIBLE;
    group.iGroupId = Index;
    group.pszHeader = Text;

    ListView_InsertGroup(ListViewHandle, INT_MAX, &group);
}

VOID AddNetworkAdapterToListView(
    _In_ PDV_NETADAPTER_CONTEXT Context,
    _In_ BOOLEAN AdapterPresent,
    _In_ IF_INDEX IfIndex,
    _In_ IF_LUID Luid,
    _In_ PPH_STRING Guid,
    _In_ PPH_STRING Description
    )
{
    DV_NETADAPTER_ID adapterId;
    INT lvItemIndex;
    BOOLEAN found = FALSE;
    PDV_NETADAPTER_ID newId = NULL;

    InitializeNetAdapterId(&adapterId, IfIndex, Luid, NULL);

    for (ULONG i = 0; i < NetworkAdaptersList->Count; i++)
    {
        PDV_NETADAPTER_ENTRY entry = PhReferenceObjectSafe(NetworkAdaptersList->Items[i]);

        if (!entry)
            continue;

        if (EquivalentNetAdapterId(&entry->Id, &adapterId))
        {
            newId = PhAllocate(sizeof(DV_NETADAPTER_ID));
            CopyNetAdapterId(newId, &entry->Id);

            if (entry->UserReference)
                found = TRUE;
        }

        PhDereferenceObjectDeferDelete(entry);

        if (newId)
            break;
    }

    if (!newId)
    {
        newId = PhAllocate(sizeof(DV_NETADAPTER_ID));
        CopyNetAdapterId(newId, &adapterId);
        PhMoveReference(&newId->InterfaceGuid, Guid);
    }

    lvItemIndex = AddListViewItemGroupId(
        Context->ListViewHandle,
        AdapterPresent ? 0 : 1,
        MAXINT,
        Description->Buffer,
        newId
        );

    if (found)
        ListView_SetItemState(Context->ListViewHandle, lvItemIndex, ITEM_CHECKED, LVIS_STATEIMAGEMASK);

    DeleteNetAdapterId(&adapterId);
}

ULONG64 RegQueryQword(
    _In_ HANDLE KeyHandle,
    _In_ PWSTR ValueName
    )
{
    ULONG64 value = 0;
    PH_STRINGREF valueName;
    PKEY_VALUE_PARTIAL_INFORMATION buffer;

    PhInitializeStringRef(&valueName, ValueName);

    if (NT_SUCCESS(PhQueryValueKey(KeyHandle, &valueName, KeyValuePartialInformation, &buffer)))
    {
        if (buffer->Type == REG_DWORD || buffer->Type == REG_QWORD)
        {
            value = *(ULONG64*)buffer->Data;
        }

        PhFree(buffer);
    }

    return value;
}

VOID FindNetworkAdapters(
    _In_ PDV_NETADAPTER_CONTEXT Context
    )
{
    if (Context->UseAlternateMethod)
    {
        ULONG bufferLength = 0;
        PVOID buffer = NULL;
        ULONG flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

        if (WindowsVersion >= WINDOWS_VISTA)
        {
            flags |= GAA_FLAG_INCLUDE_ALL_INTERFACES;
        }

        if (GetAdaptersAddresses(AF_UNSPEC, flags, NULL, NULL, &bufferLength) != ERROR_BUFFER_OVERFLOW)
            return;

        buffer = PhAllocate(bufferLength);
        memset(buffer, 0, bufferLength);

        Context->EnumeratingAdapters = TRUE;

        if (GetAdaptersAddresses(AF_UNSPEC, flags, NULL, buffer, &bufferLength) == ERROR_SUCCESS)
        {
            PhAcquireQueuedLockShared(&NetworkAdaptersListLock);

            for (PIP_ADAPTER_ADDRESSES i = buffer; i; i = i->Next)
            {
                PPH_STRING description;
                
                description = PH_AUTO(PhCreateString(i->Description));

                AddNetworkAdapterToListView(
                    Context,
                    TRUE,
                    i->IfIndex,
                    i->Luid,
                    PhConvertMultiByteToUtf16(i->AdapterName),
                    description
                    );
            }

            PhReleaseQueuedLockShared(&NetworkAdaptersListLock);
        }

        Context->EnumeratingAdapters = FALSE;

        PhFree(buffer);
    }
    else
    {
        PPH_LIST deviceList;
        HDEVINFO deviceInfoHandle;
        SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
        SP_DEVINFO_DATA deviceInfoData;
        PSP_DEVICE_INTERFACE_DETAIL_DATA deviceInterfaceDetail = NULL;
        ULONG deviceInfoLength = 0;

        if ((deviceInfoHandle = SetupDiGetClassDevs(
            &GUID_DEVINTERFACE_NET,
            NULL,
            NULL,
            DIGCF_DEVICEINTERFACE
            )) == INVALID_HANDLE_VALUE)
        {
            return;
        }

        deviceList = PH_AUTO(PhCreateList(1));

        for (ULONG i = 0; i < 1000; i++)
        {
            memset(&deviceInterfaceData, 0, sizeof(SP_DEVICE_INTERFACE_DATA));
            deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

            if (!SetupDiEnumDeviceInterfaces(deviceInfoHandle, 0, &GUID_DEVINTERFACE_NET, i, &deviceInterfaceData))
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
                PNET_ENUM_ENTRY adapterEntry;
                HANDLE keyHandle = NULL;
                HANDLE deviceHandle = NULL;
                DEVPROPTYPE devicePropertyType;
                WCHAR diskFriendlyName[MAX_PATH] = L"";

                if (!SetupDiGetDeviceProperty(
                    deviceInfoHandle,
                    &deviceInfoData,
                    WindowsVersion > WINDOWS_7 ? &DEVPKEY_Device_FriendlyName : &DEVPKEY_Device_DeviceDesc,
                    &devicePropertyType,
                    (PBYTE)diskFriendlyName,
                    ARRAYSIZE(diskFriendlyName),
                    NULL,
                    0
                    ))
                {
                    continue;
                }

                if (!(keyHandle = SetupDiOpenDevRegKey(
                    deviceInfoHandle,
                    &deviceInfoData,
                    DICS_FLAG_GLOBAL,
                    0,
                    DIREG_DRV,
                    KEY_QUERY_VALUE
                    )))
                {
                    continue;
                }

                adapterEntry = PhAllocate(sizeof(NET_ENUM_ENTRY));
                memset(adapterEntry, 0, sizeof(NET_ENUM_ENTRY));

                adapterEntry->DeviceGuid = PhQueryRegistryString(keyHandle, L"NetCfgInstanceId");
                adapterEntry->DeviceLuid.Info.IfType = RegQueryQword(keyHandle, L"*IfType");
                adapterEntry->DeviceLuid.Info.NetLuidIndex = RegQueryQword(keyHandle, L"NetLuidIndex");

                NetworkAdapterCreateHandle(
                    &deviceHandle,
                    adapterEntry->DeviceGuid
                    );

                if (deviceHandle)
                {
                    PPH_STRING adapterName;

                    adapterEntry->DevicePresent = TRUE;

                    if (adapterName = NetworkAdapterQueryName(deviceHandle, adapterEntry->DeviceGuid))
                    {
                        adapterEntry->DeviceName = adapterName;
                    }
                    else
                    {
                        adapterEntry->DeviceName = PhCreateString(diskFriendlyName);
                    }

                    NtClose(deviceHandle);
                }
                else
                {
                    adapterEntry->DeviceName = PhCreateString(diskFriendlyName);
                }

                PhAddItemList(deviceList, adapterEntry);

                NtClose(keyHandle);
            }

            PhFree(deviceInterfaceDetail);
        }

        SetupDiDestroyDeviceInfoList(deviceInfoHandle);

        // Sort the entries
        qsort(deviceList->Items, deviceList->Count, sizeof(PVOID), AdapterEntryCompareFunction);

        Context->EnumeratingAdapters = TRUE;
        PhAcquireQueuedLockShared(&NetworkAdaptersListLock);

        for (ULONG i = 0; i < deviceList->Count; i++)
        {
            PNET_ENUM_ENTRY entry = deviceList->Items[i];

            AddNetworkAdapterToListView(
                Context,
                entry->DevicePresent,
                0,
                entry->DeviceLuid,
                entry->DeviceGuid,
                entry->DeviceName
                );

            if (entry->DeviceName)
                PhDereferenceObject(entry->DeviceName);
            // Note: DeviceGuid is disposed by WM_DESTROY.

            PhFree(entry);
        }

        PhReleaseQueuedLockShared(&NetworkAdaptersListLock);
        Context->EnumeratingAdapters = FALSE;
    }
}

INT_PTR CALLBACK NetworkAdapterOptionsDlgProc(
    _In_ HWND hwndDlg,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
    )
{
    PDV_NETADAPTER_CONTEXT context = NULL;

    if (uMsg == WM_INITDIALOG)
    {
        context = (PDV_NETADAPTER_CONTEXT)PhAllocate(sizeof(DV_NETADAPTER_CONTEXT));
        memset(context, 0, sizeof(DV_NETADAPTER_CONTEXT));

        SetProp(hwndDlg, L"Context", (HANDLE)context);
    }
    else
    {
        context = (PDV_NETADAPTER_CONTEXT)GetProp(hwndDlg, L"Context");

        if (uMsg == WM_DESTROY)
        {
            if (context->OptionsChanged)
                NetAdaptersSaveList();

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
            context->ListViewHandle = GetDlgItem(hwndDlg, IDC_NETADAPTERS_LISTVIEW);

            PhSetListViewStyle(context->ListViewHandle, FALSE, TRUE);
            ListView_SetExtendedListViewStyleEx(context->ListViewHandle, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
            PhSetControlTheme(context->ListViewHandle, L"explorer");
            PhAddListViewColumn(context->ListViewHandle, 0, 0, 0, LVCFMT_LEFT, 350, L"Network Adapters");
            PhSetExtendedListView(context->ListViewHandle);

            if (WindowsVersion >= WINDOWS_VISTA)
            {
                context->UseAlternateMethod = FALSE;

                ListView_EnableGroupView(context->ListViewHandle, TRUE);
                AddListViewGroup(context->ListViewHandle, 0, L"Connected");
                AddListViewGroup(context->ListViewHandle, 1, L"Disconnected");

                FindNetworkAdapters(context);
            }
            else
            {
                context->UseAlternateMethod = TRUE;

                Button_Enable(GetDlgItem(hwndDlg, IDC_SHOW_HIDDEN_ADAPTERS), FALSE);

                FindNetworkAdapters(context);
            }
        }
        break;
    case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
            case IDC_SHOW_HIDDEN_ADAPTERS:
                {
                    context->UseAlternateMethod = !context->UseAlternateMethod;
                   
                    if (WindowsVersion >= WINDOWS_VISTA)
                    {
                        if (context->UseAlternateMethod)
                        {
                            ListView_EnableGroupView(context->ListViewHandle, FALSE);
                        }
                        else
                        {
                            ListView_EnableGroupView(context->ListViewHandle, TRUE);
                        }
                    }

                    ListView_DeleteAllItems(context->ListViewHandle);

                    FindNetworkAdapters(context);
                }
                break;
            }
        }
        break;
    case WM_NOTIFY:
        {
            LPNMHDR header = (LPNMHDR)lParam;

            if (header->code == LVN_ITEMCHANGED)
            {
                LPNM_LISTVIEW listView = (LPNM_LISTVIEW)lParam;

                if (context->EnumeratingAdapters)
                    break;

                if (listView->uChanged & LVIF_STATE)
                {
                    switch (listView->uNewState & 0x3000)
                    {
                    case 0x2000: // checked
                        {
                            PDV_NETADAPTER_ID param = (PDV_NETADAPTER_ID)listView->lParam;

                            if (!FindAdapterEntry(param, FALSE))
                            {
                                PDV_NETADAPTER_ENTRY entry;

                                entry = CreateNetAdapterEntry(param);
                                entry->UserReference = TRUE;
                            }

                            context->OptionsChanged = TRUE;
                        }
                        break;
                    case 0x1000: // unchecked
                        {
                            PDV_NETADAPTER_ID param = (PDV_NETADAPTER_ID)listView->lParam;

                            FindAdapterEntry(param, TRUE);

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