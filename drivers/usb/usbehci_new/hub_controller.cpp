/*
 * PROJECT:     ReactOS Universal Serial Bus Bulk Enhanced Host Controller Interface
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        drivers/usb/usbehci/hub_controller.cpp
 * PURPOSE:     USB EHCI device driver.
 * PROGRAMMERS:
 *              Michael Martin (michael.martin@reactos.org)
 *              Johannes Anderwald (johannes.anderwald@reactos.org)
 */

#define INITGUID
#include "usbehci.h"

class CHubController : public IHubController,
                       public IDispatchIrp
{
public:
    STDMETHODIMP QueryInterface( REFIID InterfaceId, PVOID* Interface);

    STDMETHODIMP_(ULONG) AddRef()
    {
        InterlockedIncrement(&m_Ref);
        return m_Ref;
    }
    STDMETHODIMP_(ULONG) Release()
    {
        InterlockedDecrement(&m_Ref);

        if (!m_Ref)
        {
            delete this;
            return 0;
        }
        return m_Ref;
    }

    // IHubController interface functions
    virtual NTSTATUS Initialize(IN PDRIVER_OBJECT DriverObject, IN PHCDCONTROLLER Controller, IN PUSBHARDWAREDEVICE Device, IN BOOLEAN IsRootHubDevice, IN ULONG DeviceAddress);

    // IDispatchIrp interface functions
    virtual NTSTATUS HandlePnp(IN PDEVICE_OBJECT DeviceObject, IN OUT PIRP Irp);
    virtual NTSTATUS HandlePower(IN PDEVICE_OBJECT DeviceObject, IN OUT PIRP Irp);
    virtual NTSTATUS HandleDeviceControl(IN PDEVICE_OBJECT DeviceObject, IN OUT PIRP Irp);

    // local functions
    NTSTATUS HandleQueryInterface(PIO_STACK_LOCATION IoStack);
    NTSTATUS SetDeviceInterface(BOOLEAN bEnable);
    NTSTATUS CreatePDO(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT * OutDeviceObject);
    NTSTATUS GetHubControllerDeviceObject(PDEVICE_OBJECT * HubDeviceObject);

    // constructor / destructor
    CHubController(IUnknown *OuterUnknown){}
    virtual ~CHubController(){}

protected:
    LONG m_Ref;
    PHCDCONTROLLER m_Controller;
    PUSBHARDWAREDEVICE m_Hardware;
    BOOLEAN m_IsRootHubDevice;
    ULONG m_DeviceAddress;
    ULONG m_PDODeviceNumber;
    BOOLEAN m_InterfaceEnabled;
    UNICODE_STRING m_HubDeviceInterfaceString;
    PDEVICE_OBJECT m_HubControllerDeviceObject;
    PDRIVER_OBJECT m_DriverObject;
};

//----------------------------------------------------------------------------------------
NTSTATUS
STDMETHODCALLTYPE
CHubController::QueryInterface(
    IN  REFIID refiid,
    OUT PVOID* Output)
{
    return STATUS_UNSUCCESSFUL;
}
//----------------------------------------------------------------------------------------
NTSTATUS
CHubController::Initialize(
    IN PDRIVER_OBJECT DriverObject,
    IN PHCDCONTROLLER Controller,
    IN PUSBHARDWAREDEVICE Device,
    IN BOOLEAN IsRootHubDevice,
    IN ULONG DeviceAddress)
{
    NTSTATUS Status;
    PCOMMON_DEVICE_EXTENSION DeviceExtension;

    DPRINT1("CHubController::Initialize\n");

    //
    // initialize members
    //
    m_Controller = Controller;
    m_Hardware = Device;
    m_IsRootHubDevice = IsRootHubDevice;
    m_DeviceAddress = DeviceAddress;
    m_DriverObject = DriverObject;

    //
    // create PDO
    //
    Status = CreatePDO(m_DriverObject, &m_HubControllerDeviceObject);
    if (!NT_SUCCESS(Status))
    {
        //
        // failed to create hub device object
        //
        return Status;
    }

    //
    // get device extension
    //
    DeviceExtension = (PCOMMON_DEVICE_EXTENSION)m_HubControllerDeviceObject->DeviceExtension;

    //
    // initialize device extension
    //
    DeviceExtension->IsFDO = FALSE;
    DeviceExtension->IsHub = TRUE; //FIXME
    DeviceExtension->Dispatcher = PDISPATCHIRP(this);

    //
    // clear init flag
    //
    m_HubControllerDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;


    return STATUS_SUCCESS;
}

//-----------------------------------------------------------------------------------------
NTSTATUS
CHubController::GetHubControllerDeviceObject(PDEVICE_OBJECT * HubDeviceObject)
{
    //
    // store controller object
    //
    *HubDeviceObject = m_HubControllerDeviceObject;

    return STATUS_SUCCESS;
}
//-----------------------------------------------------------------------------------------
NTSTATUS
CHubController::HandlePnp(
    IN PDEVICE_OBJECT DeviceObject,
    IN OUT PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PCOMMON_DEVICE_EXTENSION DeviceExtension;
    PDEVICE_CAPABILITIES DeviceCapabilities;
    PPNP_BUS_INFORMATION BusInformation;
    NTSTATUS Status;
    ULONG Index = 0, Length;
    USHORT VendorID, DeviceID;
    ULONG HiSpeed, NumPorts;
    WCHAR Buffer[300];
    LPWSTR DeviceName;

    //
    // get device extension
    //
    DeviceExtension = (PCOMMON_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    //
    // sanity check
    //
    ASSERT(DeviceExtension->IsFDO == FALSE);

    //
    // get current stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    switch(IoStack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
        {
            DPRINT1("CHubController::HandlePnp IRP_MN_START_DEVICE\n");
            //
            // register device interface 
            //
            Status = SetDeviceInterface(TRUE);
            break;
        }
        case IRP_MN_QUERY_ID:
        {
            DPRINT1("CHubController::HandlePnp IRP_MN_QUERY_ID Type %x\n", IoStack->Parameters.QueryId.IdType);

            if (IoStack->Parameters.QueryId.IdType == BusQueryDeviceID)
            {
                if (m_Hardware)
                {
                    //
                    // query device id
                    //
                    Status = m_Hardware->GetDeviceDetails(&VendorID, &DeviceID, &NumPorts, &HiSpeed);

                    HiSpeed = TRUE;

                    if (HiSpeed)
                    {
                        //
                        // USB 2.0 hub
                        //
                        swprintf(Buffer, L"USB\\ROOT_HUB20");
                    }
                    else
                    {
                        //
                        // USB 1.1 hub
                        //
                        swprintf(Buffer, L"USB\\ROOT_HUB");
                    }

                    DPRINT1("Name %S\n", Buffer);

                    //
                    // calculate length
                    //
                    Length = (wcslen(Buffer) + 1);

                    //
                    // allocate buffer
                    //
                    DeviceName = (LPWSTR)ExAllocatePoolWithTag(PagedPool, Length * sizeof(WCHAR), TAG_USBEHCI);

                    if (!DeviceName)
                    {
                        //
                        // no memory
                        //
                        Status = STATUS_INSUFFICIENT_RESOURCES;
                        break;
                    }

                    //
                    // copy device name
                    //
                    wcscpy(DeviceName, Buffer);

                    //
                    // store result
                    //
                    Irp->IoStatus.Information = (ULONG_PTR)DeviceName;
                    Status = STATUS_SUCCESS;
                    break;
                 }
                 Status = STATUS_UNSUCCESSFUL;
                 PC_ASSERT(0);
                 break;
            }

            if (IoStack->Parameters.QueryId.IdType == BusQueryHardwareIDs)
            {
                if (m_Hardware)
                {
                    //
                    // query device id
                    //
                    Status = m_Hardware->GetDeviceDetails(&VendorID, &DeviceID, &NumPorts, &HiSpeed);

                    HiSpeed = TRUE;

                    if (!NT_SUCCESS(Status))
                    {
                         DPRINT1("CHubController::HandlePnp> failed to get hardware id %x\n", Status);
                         VendorID = 0x8086;
                         DeviceID = 0x3A37;
                    }

                    if (HiSpeed)
                    {
                        //
                        // USB 2.0 hub
                        //
                        Index += swprintf(&Buffer[Index], L"USB\\ROOT_HUB20&VID%04x&PID%04x&REV0000", VendorID, DeviceID) + 1;
                        Index += swprintf(&Buffer[Index], L"USB\\ROOT_HUB20&VID%04x&PID%04x", VendorID, DeviceID) + 1;
                        Index += swprintf(&Buffer[Index], L"USB\\ROOT_HUB20") + 1;
                    }
                    else
                    {
                        //
                        // USB 1.1 hub
                        //
                        Index += swprintf(&Buffer[Index], L"USB\\ROOT_HUB&VID%04x&PID%04x&REV0000", VendorID, DeviceID) + 1;
                        Index += swprintf(&Buffer[Index], L"USB\\ROOT_HUB&VID%04x&PID%04x", VendorID, DeviceID) + 1;
                        Index += swprintf(&Buffer[Index], L"USB\\ROOT_HUB") + 1;
                    }

                   Buffer[Index] = UNICODE_NULL;
                   Index++;


                    DPRINT1("Name %S\n", Buffer);

                    //
                    // allocate buffer
                    //
                    DeviceName = (LPWSTR)ExAllocatePoolWithTag(PagedPool, Index * sizeof(WCHAR), TAG_USBEHCI);

                    if (!DeviceName)
                    {
                        //
                        // no memory
                        //
                        Status = STATUS_INSUFFICIENT_RESOURCES;
                        break;
                    }

                    //
                    // copy device name
                    //
                    RtlMoveMemory(DeviceName, Buffer, Index * sizeof(WCHAR));

                    //
                    // store result
                    //
                    Irp->IoStatus.Information = (ULONG_PTR)DeviceName;
                    Status = STATUS_SUCCESS;
                    break;
                }
            }
            Status = STATUS_SUCCESS;
            break;
        }
        case IRP_MN_QUERY_CAPABILITIES:
        {
            DPRINT1("CHubController::HandlePnp IRP_MN_QUERY_CAPABILITIES\n");

            DeviceCapabilities = (PDEVICE_CAPABILITIES)IoStack->Parameters.DeviceCapabilities.Capabilities;

            DeviceCapabilities->LockSupported = FALSE;
            DeviceCapabilities->EjectSupported = FALSE;
            DeviceCapabilities->Removable = FALSE;
            DeviceCapabilities->DockDevice = FALSE;
            DeviceCapabilities->UniqueID = FALSE;
            DeviceCapabilities->SilentInstall = FALSE;
            DeviceCapabilities->RawDeviceOK = FALSE;
            DeviceCapabilities->SurpriseRemovalOK = FALSE;
            DeviceCapabilities->Address = 0;
            DeviceCapabilities->UINumber = 0;
            DeviceCapabilities->DeviceD2 = 1;

            /* FIXME */
            DeviceCapabilities->HardwareDisabled = FALSE;
            DeviceCapabilities->NoDisplayInUI = FALSE;
            DeviceCapabilities->DeviceState[0] = PowerDeviceD0;
            for (Index = 0; Index < PowerSystemMaximum; Index++)
                DeviceCapabilities->DeviceState[Index] = PowerDeviceD3;
            DeviceCapabilities->DeviceWake = PowerDeviceUnspecified;
            DeviceCapabilities->D1Latency = 0;
            DeviceCapabilities->D2Latency = 0;
            DeviceCapabilities->D3Latency = 0;

            Status = STATUS_SUCCESS;
            break;
        }
        case IRP_MN_QUERY_INTERFACE:
        {
            DPRINT1("CHubController::HandlePnp IRP_MN_QUERY_INTERFACE\n");

            //
            // handle device interface requests
            //
            Status = HandleQueryInterface(IoStack);
            break;
        }
        case IRP_MN_REMOVE_DEVICE:
        {
            DPRINT1("CHubController::HandlePnp IRP_MN_REMOVE_DEVICE\n");

            //
            // deactivate device interface for BUS PDO
            //
            SetDeviceInterface(FALSE);

            //
            // complete the request first
            //
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);

            //
            // now delete device
            //
            IoDeleteDevice(m_HubControllerDeviceObject);

            //
            // nullify pointer
            //
            m_HubControllerDeviceObject = 0;

            //
            // done
            //
            return STATUS_SUCCESS;
        }
        case IRP_MN_QUERY_BUS_INFORMATION:
        {
            DPRINT1("CHubController::HandlePnp IRP_MN_QUERY_BUS_INFORMATION\n");

            //
            // allocate buffer for bus information
            //
            BusInformation = (PPNP_BUS_INFORMATION)ExAllocatePool(PagedPool, sizeof(PNP_BUS_INFORMATION));
            if (BusInformation)
            {
                //
                // copy BUS guid
                //
                RtlMoveMemory(&BusInformation->BusTypeGuid, &GUID_BUS_TYPE_USB, sizeof(GUID));

                //
                // set bus type
                //
                BusInformation->LegacyBusType = PNPBus;
                BusInformation->BusNumber = 0;

                Status = STATUS_SUCCESS;
                Irp->IoStatus.Information = (ULONG_PTR)BusInformation;
            }
            else
            {
                //
                // no memory
                //
                Status = STATUS_INSUFFICIENT_RESOURCES;
            }
            break;
        }
        case IRP_MN_STOP_DEVICE:
        {
            DPRINT1("CHubController::HandlePnp IRP_MN_STOP_DEVICE\n");
            //
            // stop device
            //
            Status = STATUS_SUCCESS;
            break;
        }
        default:
        {
            DPRINT1("CHubController::HandlePnp Unhandeled %x\n", IoStack->MinorFunction);
            Status = Irp->IoStatus.Status;
            break;
        }
    }

    //
    // complete request
    //
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    //
    // done
    //
    return Status;
}

//-----------------------------------------------------------------------------------------
NTSTATUS
CHubController::HandlePower(
    IN PDEVICE_OBJECT DeviceObject,
    IN OUT PIRP Irp)
{
    UNIMPLEMENTED
    Irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_IMPLEMENTED;
}

//-----------------------------------------------------------------------------------------
NTSTATUS
CHubController::HandleDeviceControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN OUT PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PCOMMON_DEVICE_EXTENSION DeviceExtension;
    NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

    //
    // get current stack location
    //
    IoStack = IoGetCurrentIrpStackLocation(Irp);

    //
    // get device extension
    //
    DeviceExtension = (PCOMMON_DEVICE_EXTENSION)DeviceObject->DeviceExtension;


    DPRINT1("HandleDeviceControl>Type: FDO %u IoCtl %x InputBufferLength %lu OutputBufferLength %lu\n",
        DeviceExtension->IsFDO,
        IoStack->Parameters.DeviceIoControl.IoControlCode,
        IoStack->Parameters.DeviceIoControl.InputBufferLength,
        IoStack->Parameters.DeviceIoControl.OutputBufferLength);

    //
    // determine which request should be performed
    //
    switch(IoStack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_INTERNAL_USB_SUBMIT_URB:
        {
            DPRINT1("IOCTL_INTERNAL_USB_SUBMIT_URB UNIMPLEMENTED\n");

            //
            // request completed
            //
            Status = STATUS_NOT_IMPLEMENTED;
            break;
        }
        case IOCTL_INTERNAL_USB_GET_DEVICE_HANDLE:
        {
            DPRINT1("IOCTL_INTERNAL_USB_GET_DEVICE_HANDLE\n");

            if (IoStack->Parameters.Others.Argument1)
            {
                //
                // store object as device handle
                //
                *(PVOID *)IoStack->Parameters.Others.Argument1 = (PVOID)this;
                Status = STATUS_SUCCESS;
            }
            else
            {
                //
                // mis-behaving hub driver
                //
                Status = STATUS_INVALID_DEVICE_REQUEST;
            }

            //
            // request completed
            //
            break;
        }
        case IOCTL_INTERNAL_USB_GET_ROOTHUB_PDO:
        {
            DPRINT1("IOCTL_INTERNAL_USB_GET_ROOTHUB_PDO\n");

            //
            // this is the first request send, it delivers the PDO to the caller
            //
            if (IoStack->Parameters.Others.Argument1)
            {
                //
                // store root hub pdo object
                //
                *(PVOID *)IoStack->Parameters.Others.Argument1 = DeviceObject;
            }

            if (IoStack->Parameters.Others.Argument2)
            {
                //
                // documentation claims to deliver the hcd controller object, although it is wrong
                //
                *(PVOID *)IoStack->Parameters.Others.Argument2 = DeviceObject;
            }

            //
            // request completed
            //
            Status = STATUS_SUCCESS;
            break;
        }
        case IOCTL_INTERNAL_USB_GET_HUB_COUNT:
        {
            DPRINT1("IOCTL_INTERNAL_USB_GET_HUB_COUNT\n");

            //
            // after IOCTL_INTERNAL_USB_GET_ROOTHUB_PDO is delivered, the usbhub driver
            // requests this ioctl to deliver the number of presents. 

            if (IoStack->Parameters.Others.Argument1)
            {
                //
                // FIXME / verify: there is only one hub
                //
                *(PULONG)IoStack->Parameters.Others.Argument1 = 1;
            }

            //
            // request completed
            //
            Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = sizeof(ULONG);
            break;
        }
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

//=================================================================================================
//
// Generic Interface functions
//
VOID
USB_BUSIFFN
USBI_InterfaceReference(
    PVOID BusContext)
{
    CHubController * Controller = (CHubController*)BusContext;

    DPRINT1("USBH_InterfaceReference\n");

    //
    // add reference
    //
    Controller->AddRef();
}

VOID
USB_BUSIFFN
USBI_InterfaceDereference(
    PVOID BusContext)
{
    CHubController * Controller = (CHubController*)BusContext;

    DPRINT1("USBH_InterfaceDereference\n");

    //
    // release
    //
    Controller->Release();
}
//=================================================================================================
//
// USB Hub Interface functions
//
NTSTATUS
USB_BUSIFFN
USBHI_CreateUsbDevice(
    PVOID BusContext,
    PUSB_DEVICE_HANDLE *NewDevice,
    PUSB_DEVICE_HANDLE HubDeviceHandle,
    USHORT PortStatus,
    USHORT PortNumber)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
USB_BUSIFFN
USBHI_InitializeUsbDevice(
    PVOID BusContext,
    PUSB_DEVICE_HANDLE DeviceHandle)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
USB_BUSIFFN
USBHI_GetUsbDescriptors(
    PVOID BusContext,
    PUSB_DEVICE_HANDLE DeviceHandle,
    PUCHAR DeviceDescriptorBuffer,
    PULONG DeviceDescriptorBufferLength,
    PUCHAR ConfigDescriptorBuffer,
    PULONG ConfigDescriptorBufferLength)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
USB_BUSIFFN
USBHI_RemoveUsbDevice(
    PVOID BusContext,
    PUSB_DEVICE_HANDLE DeviceHandle,
    ULONG Flags)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
USB_BUSIFFN
USBHI_RestoreUsbDevice(
    PVOID BusContext,
    PUSB_DEVICE_HANDLE OldDeviceHandle,
    PUSB_DEVICE_HANDLE NewDeviceHandle)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
USB_BUSIFFN
USBHI_GetPortHackFlags(
    PVOID BusContext,
    PULONG Flags)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
USB_BUSIFFN
USBHI_QueryDeviceInformation(
    PVOID BusContext,
    PUSB_DEVICE_HANDLE DeviceHandle,
    PVOID DeviceInformationBuffer,
    ULONG DeviceInformationBufferLength,
    PULONG LengthReturned)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
USB_BUSIFFN
USBHI_GetControllerInformation(
    PVOID BusContext,
    PVOID ControllerInformationBuffer,
    ULONG ControllerInformationBufferLength,
    PULONG LengthReturned)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
USB_BUSIFFN
USBHI_ControllerSelectiveSuspend(
    PVOID BusContext,
    BOOLEAN Enable)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
USB_BUSIFFN
USBHI_GetExtendedHubInformation(
    PVOID BusContext,
    PDEVICE_OBJECT HubPhysicalDeviceObject,
    PVOID HubInformationBuffer,
    ULONG HubInformationBufferLength,
    PULONG LengthReturned)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
USB_BUSIFFN
USBHI_GetRootHubSymbolicName(
    PVOID BusContext,
    PVOID HubSymNameBuffer,
    ULONG HubSymNameBufferLength,
    PULONG HubSymNameActualLength)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

PVOID
USB_BUSIFFN
USBHI_GetDeviceBusContext(
    PVOID HubBusContext,
    PVOID DeviceHandle)
{
    UNIMPLEMENTED
    return NULL;
}

NTSTATUS
USB_BUSIFFN
USBHI_Initialize20Hub(
    PVOID BusContext,
    PUSB_DEVICE_HANDLE HubDeviceHandle,
    ULONG TtCount)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
USB_BUSIFFN
USBHI_RootHubInitNotification(
    PVOID BusContext,
    PVOID CallbackContext,
    PRH_INIT_CALLBACK CallbackRoutine)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

VOID
USB_BUSIFFN
USBHI_FlushTransfers(
    PVOID BusContext,
    PVOID DeviceHandle)
{
    UNIMPLEMENTED
}

VOID
USB_BUSIFFN
USBHI_SetDeviceHandleData(
    PVOID BusContext,
    PVOID DeviceHandle,
    PDEVICE_OBJECT UsbDevicePdo)
{
    UNIMPLEMENTED
}

//=================================================================================================
//
// USB Device Interface functions
//

VOID
USB_BUSIFFN
USBDI_GetUSBDIVersion(
    PVOID BusContext,
    PUSBD_VERSION_INFORMATION VersionInformation,
    PULONG HcdCapabilites)
{
    UNIMPLEMENTED
}

NTSTATUS
USB_BUSIFFN
USBDI_QueryBusTime(
    PVOID BusContext,
    PULONG CurrentFrame)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
USB_BUSIFFN
USBDI_SubmitIsoOutUrb(
    PVOID BusContext,
    PURB Urb)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
USB_BUSIFFN
USBDI_QueryBusInformation(
    PVOID BusContext,
    ULONG Level,
    PVOID BusInformationBuffer,
    PULONG BusInformationBufferLength,
    PULONG BusInformationActualLength)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

BOOLEAN
USB_BUSIFFN
USBDI_IsDeviceHighSpeed(
    PVOID BusContext)
{
    UNIMPLEMENTED
    return TRUE;
}

NTSTATUS
USB_BUSIFFN
USBDI_EnumLogEntry(
    PVOID BusContext, 
    ULONG DriverTag, 
    ULONG EnumTag,
    ULONG P1,
    ULONG P2)
{
    UNIMPLEMENTED
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
CHubController::HandleQueryInterface(
    PIO_STACK_LOCATION IoStack)
{
    PUSB_BUS_INTERFACE_HUB_V5 InterfaceHub;
    PUSB_BUS_INTERFACE_USBDI_V2 InterfaceDI;
    UNICODE_STRING GuidBuffer;
    NTSTATUS Status;

    if (IsEqualGUIDAligned(*IoStack->Parameters.QueryInterface.InterfaceType, USB_BUS_INTERFACE_HUB_GUID))
    {
        //
        // get request parameters
        //
        InterfaceHub = (PUSB_BUS_INTERFACE_HUB_V5)IoStack->Parameters.QueryInterface.Interface;
        InterfaceHub->Version = IoStack->Parameters.QueryInterface.Version;

        //
        // check version
        //
        if (IoStack->Parameters.QueryInterface.Version >= 6)
        {
            DPRINT1("USB_BUS_INTERFACE_HUB_GUID version %x not supported!\n", IoStack->Parameters.QueryInterface.Version);

            //
            // version not supported
            //
            return STATUS_NOT_SUPPORTED;
        }

        //
        // Interface version 0
        //
        if (IoStack->Parameters.QueryInterface.Version >= 0)
        {
            InterfaceHub->Size = IoStack->Parameters.QueryInterface.Size;
            InterfaceHub->BusContext = PVOID(this);
            InterfaceHub->InterfaceReference = USBI_InterfaceReference;
            InterfaceHub->InterfaceDereference = USBI_InterfaceDereference;
        }

        //
        // Interface version 1
        //
        if (IoStack->Parameters.QueryInterface.Version >= 1)
        {
            InterfaceHub->CreateUsbDevice = USBHI_CreateUsbDevice;
            InterfaceHub->InitializeUsbDevice = USBHI_InitializeUsbDevice;
            InterfaceHub->GetUsbDescriptors = USBHI_GetUsbDescriptors;
            InterfaceHub->RemoveUsbDevice = USBHI_RemoveUsbDevice;
            InterfaceHub->RestoreUsbDevice = USBHI_RestoreUsbDevice;
            InterfaceHub->GetPortHackFlags = USBHI_GetPortHackFlags;
            InterfaceHub->QueryDeviceInformation = USBHI_QueryDeviceInformation;
        }

        //
        // Interface version 2
        //
        if (IoStack->Parameters.QueryInterface.Version >= 2)
        {
            InterfaceHub->GetControllerInformation = USBHI_GetControllerInformation;
            InterfaceHub->ControllerSelectiveSuspend = USBHI_ControllerSelectiveSuspend;
            InterfaceHub->GetExtendedHubInformation = USBHI_GetExtendedHubInformation;
            InterfaceHub->GetRootHubSymbolicName = USBHI_GetRootHubSymbolicName;
            InterfaceHub->GetDeviceBusContext = USBHI_GetDeviceBusContext;
            InterfaceHub->Initialize20Hub = USBHI_Initialize20Hub;

        }

        //
        // Interface version 3
        //
        if (IoStack->Parameters.QueryInterface.Version >= 3)
        {
            InterfaceHub->RootHubInitNotification = USBHI_RootHubInitNotification;
        }

        //
        // Interface version 4
        //
        if (IoStack->Parameters.QueryInterface.Version >= 4)
        {
            InterfaceHub->FlushTransfers = USBHI_FlushTransfers;
        }

        //
        // Interface version 5
        //
        if (IoStack->Parameters.QueryInterface.Version >= 5)
        {
            InterfaceHub->SetDeviceHandleData = USBHI_SetDeviceHandleData;
        }

        //
        // request completed
        //
        return STATUS_SUCCESS;
    }
    else if (IsEqualGUIDAligned(*IoStack->Parameters.QueryInterface.InterfaceType, USB_BUS_INTERFACE_USBDI_GUID))
    {
        //
        // get request parameters
        //
        InterfaceDI = (PUSB_BUS_INTERFACE_USBDI_V2) IoStack->Parameters.QueryInterface.Interface;
        InterfaceDI->Version = IoStack->Parameters.QueryInterface.Version;

        //
        // check version
        //
        if (IoStack->Parameters.QueryInterface.Version >= 3)
        {
            DPRINT1("USB_BUS_INTERFACE_USBDI_GUID version %x not supported!\n", IoStack->Parameters.QueryInterface.Version);

            //
            // version not supported
            //
            return STATUS_NOT_SUPPORTED;
        }

        //
        // interface version 0
        //
        if (IoStack->Parameters.QueryInterface.Version >= 0)
        {
            InterfaceDI->Size = IoStack->Parameters.QueryInterface.Size;
            InterfaceDI->BusContext = PVOID(this);
            InterfaceDI->InterfaceReference = USBI_InterfaceReference;
            InterfaceDI->InterfaceDereference = USBI_InterfaceDereference;
            InterfaceDI->GetUSBDIVersion = USBDI_GetUSBDIVersion;
            InterfaceDI->QueryBusTime = USBDI_QueryBusTime;
            InterfaceDI->SubmitIsoOutUrb = USBDI_SubmitIsoOutUrb;
            InterfaceDI->QueryBusInformation = USBDI_QueryBusInformation;
        }

        //
        // interface version 1
        //
        if (IoStack->Parameters.QueryInterface.Version >= 1)
        {
            InterfaceDI->IsDeviceHighSpeed = USBDI_IsDeviceHighSpeed;
        }

        //
        // interface version 2
        //
        if (IoStack->Parameters.QueryInterface.Version >= 2)
        {
            InterfaceDI->EnumLogEntry = USBDI_EnumLogEntry;
        }

        //
        // request completed
        //
        return STATUS_SUCCESS;
    }
    else
    {
        //
        // convert guid to string
        //
        Status = RtlStringFromGUID(*IoStack->Parameters.QueryInterface.InterfaceType, &GuidBuffer);
        if (NT_SUCCESS(Status))
        {
            //
            // print interface
            //
            DPRINT1("HandleQueryInterface UNKNOWN INTERFACE GUID: %wZ Version %x\n", &GuidBuffer, IoStack->Parameters.QueryInterface.Version);

            //
            // free guid buffer
            //
            RtlFreeUnicodeString(&GuidBuffer);
        }
    }
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
CHubController::SetDeviceInterface(
    BOOLEAN Enable)
{
    NTSTATUS Status = STATUS_SUCCESS;

    if (Enable)
    {
        //
        // register device interface
        //
        Status = IoRegisterDeviceInterface(m_HubControllerDeviceObject, &GUID_DEVINTERFACE_USB_HUB, 0, &m_HubDeviceInterfaceString);

        if (NT_SUCCESS(Status))
        {
            //
            // now enable the device interface
            //
            Status = IoSetDeviceInterfaceState(&m_HubDeviceInterfaceString, TRUE);

            //
            // enable interface
            //
            m_InterfaceEnabled = TRUE;
        }
    }
    else if (m_InterfaceEnabled)
    {
        //
        // disable device interface
        //
        Status = IoSetDeviceInterfaceState(&m_HubDeviceInterfaceString, FALSE);

        if (NT_SUCCESS(Status))
        {
            //
            // now delete interface string
            //
            RtlFreeUnicodeString(&m_HubDeviceInterfaceString);
        }

        //
        // disable interface
        //
        m_InterfaceEnabled = FALSE;
    }

    //
    // done
    //
    return Status;
}

NTSTATUS
CHubController::CreatePDO(
    PDRIVER_OBJECT DriverObject,
    PDEVICE_OBJECT * OutDeviceObject)
{
    WCHAR CharDeviceName[64];
    NTSTATUS Status;
    ULONG UsbDeviceNumber = 0;
    UNICODE_STRING DeviceName;

    while (TRUE)
    {
        //
        // construct device name
        //
        swprintf(CharDeviceName, L"\\Device\\USBPDO-%d", UsbDeviceNumber);

        //
        // initialize device name
        //
        RtlInitUnicodeString(&DeviceName, CharDeviceName);

        //
        // create device
        //
        Status = IoCreateDevice(DriverObject,
                                sizeof(COMMON_DEVICE_EXTENSION),
                                &DeviceName,
                                FILE_DEVICE_CONTROLLER,
                                0,
                                FALSE,
                                OutDeviceObject);

        /* check for success */
        if (NT_SUCCESS(Status))
            break;

        //
        // is there a device object with that same name
        //
        if ((Status == STATUS_OBJECT_NAME_EXISTS) || (Status == STATUS_OBJECT_NAME_COLLISION))
        {
            //
            // Try the next name
            //
            UsbDeviceNumber++;
            continue;
        }

        //
        // bail out on other errors
        //
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("CreatePDO: Failed to create %wZ, Status %x\n", &DeviceName, Status);
            return Status;
        }
    }

    //
    // store PDO number
    //
    //m_PDODeviceNumber = UsbDeviceNumber;

    DPRINT1("CreateFDO: DeviceName %wZ\n", &DeviceName);

    /* done */
    return Status;
}



NTSTATUS
CreateHubController(
    PHUBCONTROLLER *OutHcdController)
{
    PHUBCONTROLLER This;

    //
    // allocate controller
    //
    This = new(NonPagedPool, TAG_USBEHCI) CHubController(0);
    if (!This)
    {
        //
        // failed to allocate
        //
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // add reference count
    //
    This->AddRef();

    //
    // return result
    //
    *OutHcdController = (PHUBCONTROLLER)This;

    //
    // done
    //
    return STATUS_SUCCESS;
}
