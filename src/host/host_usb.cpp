/*
 * Copyright (C) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "host_usb.h"
#include "server.h"

namespace Hdc {
HdcHostUSB::HdcHostUSB(const bool serverOrDaemonIn, void *ptrMainBase, void *ctxUSBin)
    : HdcUSBBase(serverOrDaemonIn, ptrMainBase)
{
    modRunning = false;
    if (!ctxUSBin) {
        return;
    }
    HdcServer *pServer = (HdcServer *)ptrMainBase;
    uv_idle_init(&pServer->loopMain, &usbWork);
    ctxUSB = (libusb_context *)ctxUSBin;

    uv_timer_init(&pServer->loopMain, &devListWatcher);
    uv_sem_init(&semUsbSend, MAX_IO_OVERLAP);
}

HdcHostUSB::~HdcHostUSB()
{
    if (modRunning) {
        Stop();
    }
    uv_sem_destroy(&semUsbSend);
    WRITE_LOG(LOG_DEBUG, "~HdcHostUSB");
}

void HdcHostUSB::Stop()
{
    if (!ctxUSB) {
        return;
    }
    Base::TryCloseHandle((uv_handle_t *)&usbWork);
    Base::TryCloseHandle((uv_handle_t *)&devListWatcher);
    modRunning = false;
}

int HdcHostUSB::Initial()
{
    if (!ctxUSB) {
        WRITE_LOG(LOG_FATAL, "USB mod ctxUSB is nullptr, recompile please");
        return -1;
    }
    WRITE_LOG(LOG_DEBUG, "HdcHostUSB init");
    modRunning = true;
    StartupUSBWork();  // Main thread registration, IO in sub-thread
    return 0;
}

void HdcHostUSB::SendUsbReset(HUSB hUSB, uint32_t sessionId)
{
    USBHead *usbPayloadHeader = new USBHead();
    usbPayloadHeader->option = USB_OPTION_RESET;
    usbPayloadHeader->sessionId = sessionId;
    if (memcpy_s(usbPayloadHeader->flag, sizeof(usbPayloadHeader->flag), PACKET_FLAG.c_str(), 2) != EOK) {
        delete usbPayloadHeader;
        return;
    }
    auto resetUsbCallback = [](struct libusb_transfer *transfer) -> void LIBUSB_CALL {
        USBHead *usbHead = (USBHead *)transfer->user_data;
        if (LIBUSB_TRANSFER_COMPLETED != transfer->status) {
            WRITE_LOG(LOG_FATAL, "SendUSBRaw status:%d", transfer->status);
        }
        delete usbHead;
        libusb_reset_device(transfer->dev_handle);
        libusb_free_transfer(transfer);
        // has send soft reset, next reset daemon's send
        WRITE_LOG(LOG_DEBUG, "Device reset singal send");
    };
    libusb_transfer *transferUsb = libusb_alloc_transfer(0);
    // clang-format off
    libusb_fill_bulk_transfer(transferUsb, hUSB->devHandle, hUSB->epHost, (uint8_t *)usbPayloadHeader, sizeof(USBHead),
        resetUsbCallback, usbPayloadHeader, GLOBAL_TIMEOUT * TIME_BASE);
    // clang-format on
    int err = libusb_submit_transfer(transferUsb);
    if (err < 0) {
        WRITE_LOG(LOG_FATAL, "libusb_submit_transfer failed, err:%d", err);
        delete usbPayloadHeader;
        return;
    }
}

bool HdcHostUSB::DetectMyNeed(libusb_device *device, string &sn)
{
    bool ret = false;
    HUSB hUSB = new HdcUSB();
    hUSB->device = device;
    int childRet = OpenDeviceMyNeed(hUSB);
    if (childRet < 0) {
        delete hUSB;
        return false;
    }
    WRITE_LOG(LOG_INFO, "Needed device found, busid:%d devid:%d connectkey:%s", hUSB->busId, hUSB->devId,
              hUSB->serialNumber.c_str());
    // USB device is automatically connected after recognition, auto connect USB
    UpdateUSBDaemonInfo(hUSB, nullptr, STATUS_READY);
    HdcServer *hdcServer = (HdcServer *)clsMainBase;
    HSession hSession = hdcServer->MallocSession(true, CONN_USB, this);
    hSession->connectKey = hUSB->serialNumber;
    uv_timer_t *waitTimeDoCmd = new uv_timer_t;
    uv_timer_init(&hdcServer->loopMain, waitTimeDoCmd);
    waitTimeDoCmd->data = hSession;
    uv_timer_start(waitTimeDoCmd, hdcServer->UsbPreConnect, 500, 3000);
    mapIgnoreDevice[sn] = HOST_USB_REGISTER;
    ret = true;
    libusb_release_interface(hUSB->devHandle, hUSB->interfaceNumber);
    libusb_close(hUSB->devHandle);
    hUSB->devHandle = nullptr;
    delete hUSB;
    return ret;
}

// sub-thread all called
void HdcHostUSB::PenddingUSBIO(uv_idle_t *handle)
{
    libusb_context *ctxUSB = (libusb_context *)handle->data;
    // every plug,handle，libusb_handle_events
    struct timeval zerotime;
    int nComplete = 0;
    zerotime.tv_sec = 0;
    zerotime.tv_usec = 1;  // if == 0,windows will be high CPU load
    libusb_handle_events_timeout_completed(ctxUSB, &zerotime, &nComplete);
}

void HdcHostUSB::KickoutZombie(HSession hSession)
{
    HdcServer *ptrConnect = (HdcServer *)hSession->classInstance;
    HUSB hUSB = hSession->hUSB;
    if (!hUSB->devHandle || hSession->isDead) {
        return;
    }
    if (LIBUSB_ERROR_NO_DEVICE != libusb_kernel_driver_active(hUSB->devHandle, hUSB->interfaceNumber)) {
        return;
    }
    ptrConnect->FreeSession(hSession->sessionId);
}

void HdcHostUSB::RemoveIgnoreDevice(string &mountInfo)
{
    if (mapIgnoreDevice.count(mountInfo)) {
        mapIgnoreDevice.erase(mountInfo);
        WRITE_LOG(LOG_DEBUG, "Remove %s from mapIgnoreDevice", mountInfo.c_str());
    }
}

void HdcHostUSB::WatchDevPlugin(uv_timer_t *handle)
{
    HdcHostUSB *thisClass = (HdcHostUSB *)handle->data;
    HdcServer *ptrConnect = (HdcServer *)thisClass->clsMainBase;
    libusb_device **devs = nullptr;
    libusb_device *dev = nullptr;
    // kick zombie
    ptrConnect->EnumUSBDeviceRegister(KickoutZombie);
    // find new
    ssize_t cnt = libusb_get_device_list(thisClass->ctxUSB, &devs);
    if (cnt < 0) {
        WRITE_LOG(LOG_FATAL, "Failed to get device list");
        return;
    }
    int i = 0;
    // linux replug devid increment，windows will be not
    while ((dev = devs[i++]) != nullptr) {  // must postfix++
        string szTmpKey = Base::StringFormat("%d-%d", libusb_get_bus_number(dev), libusb_get_device_address(dev));
        // check is in ignore list
        UsbCheckStatus statusCheck = thisClass->mapIgnoreDevice[szTmpKey];
        if (statusCheck == HOST_USB_IGNORE || statusCheck == HOST_USB_REGISTER) {
            continue;
        }
        string sn = szTmpKey;
        if (!thisClass->DetectMyNeed(dev, sn)) {
            // add to ignore device
            thisClass->mapIgnoreDevice[szTmpKey] = HOST_USB_IGNORE;
            WRITE_LOG(LOG_DEBUG, "Add %s to ignore list", szTmpKey.c_str());
        }
    }
    libusb_free_device_list(devs, 1);
}

int HdcHostUSB::StartupUSBWork()
{
    //    LIBUSB_HOTPLUG_NO_FLAGS = 0,//Only the registered callback function will only be called when the plug is
    //    inserted. LIBUSB_HOTPLUG_ENUMERATE = 1<<0,//The program load initialization before the device has been
    //    inserted, and the registered callback function is called (execution, scanning)
    WRITE_LOG(LOG_DEBUG, "USBHost loopfind mode");
    devListWatcher.data = this;
#ifdef HDC_PCDEBUG
    constexpr int interval = 500;
#else
    constexpr int interval = 3000;
#endif
    uv_timer_start(&devListWatcher, WatchDevPlugin, 0, interval);
    usbWork.data = ctxUSB;
    uv_idle_start(&usbWork, PenddingUSBIO);
    return 0;
}

int HdcHostUSB::CheckDescriptor(HUSB hUSB)
{
    char serialNum[BUF_SIZE_MEDIUM] = "";
    int childRet = 0;
    struct libusb_device_descriptor desc;
    int curBus = libusb_get_bus_number(hUSB->device);
    int curDev = libusb_get_device_address(hUSB->device);
    hUSB->busId = curBus;
    hUSB->devId = curDev;
    if (libusb_get_device_descriptor(hUSB->device, &desc)) {
        WRITE_LOG(LOG_DEBUG, "CheckDescriptor libusb_get_device_descriptor failed");
        return -1;
    }
    WRITE_LOG(LOG_DEBUG, "CheckDescriptor busid:%d devid:%d", curBus, curDev);
    // Get the serial number of the device, if there is no serial number, use the ID number to replace
    // If the device is not in time, occasionally can't get it, this is determined by the external factor, cannot be
    // changed. LIBUSB_SUCCESS
    childRet = libusb_get_string_descriptor_ascii(hUSB->devHandle, desc.iSerialNumber, (uint8_t *)serialNum,
                                                  sizeof(serialNum));
    if (childRet < 0) {
        hUSB->serialNumber = Base::StringFormat("%d-%d", curBus, curDev);
    } else {
        hUSB->serialNumber = serialNum;
    }
    return 0;
}

// hSession can be null
void HdcHostUSB::UpdateUSBDaemonInfo(HUSB hUSB, HSession hSession, uint8_t connStatus)
{
    // add to list
    HdcServer *pServer = (HdcServer *)clsMainBase;
    HdcDaemonInformation di;
    di.connectKey = hUSB->serialNumber;
    di.connType = CONN_USB;
    di.connStatus = connStatus;
    di.hSession = hSession;
    di.usbMountPoint = "";
    di.usbMountPoint = Base::StringFormat("%d-%d", hUSB->busId, hUSB->devId);

    HDaemonInfo pDi = nullptr;
    HDaemonInfo hdiNew = &di;
    pServer->AdminDaemonMap(OP_QUERY, hUSB->serialNumber, pDi);
    if (!pDi) {
        pServer->AdminDaemonMap(OP_ADD, hUSB->serialNumber, hdiNew);
    } else {
        pServer->AdminDaemonMap(OP_UPDATE, hUSB->serialNumber, hdiNew);
    }
}

bool HdcHostUSB::IsDebuggableDev(const struct libusb_interface_descriptor *ifDescriptor)
{
    constexpr uint8_t harmonyEpNum = 2;
    constexpr uint8_t harmonyClass = 0xff;
    constexpr uint8_t harmonySubClass = 0x50;
    constexpr uint8_t harmonyProtocol = 0x01;

    if (ifDescriptor->bInterfaceClass != harmonyClass || ifDescriptor->bInterfaceSubClass != harmonySubClass
        || ifDescriptor->bInterfaceProtocol != harmonyProtocol) {
        return false;
    }
    if (ifDescriptor->bNumEndpoints != harmonyEpNum) {
        return false;
    }
    return true;
}

int HdcHostUSB::CheckActiveConfig(libusb_device *device, HUSB hUSB)
{
    unsigned int j = 0;
    int ret = -1;
    struct libusb_config_descriptor *descConfig = nullptr;
    if (libusb_get_active_config_descriptor(device, &descConfig)) {
        return -1;
    }
    for (j = 0; j < descConfig->bNumInterfaces; ++j) {
        const struct libusb_interface *interface = &descConfig->interface[j];
        if (interface->num_altsetting >= 1) {
            const struct libusb_interface_descriptor *ifDescriptor = &interface->altsetting[0];
            if (!IsDebuggableDev(ifDescriptor)) {
                continue;
            }
            hUSB->interfaceNumber = ifDescriptor->bInterfaceNumber;
            unsigned int k = 0;
            for (k = 0; k < ifDescriptor->bNumEndpoints; ++k) {
                const struct libusb_endpoint_descriptor *ep_desc = &ifDescriptor->endpoint[k];
                if ((ep_desc->bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK) {
                    if (ep_desc->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                        hUSB->epDevice = ep_desc->bEndpointAddress;
                    } else {
                        hUSB->epHost = ep_desc->bEndpointAddress;
                    }
                }
            }
            if (hUSB->epDevice == 0 || hUSB->epHost == 0) {
                break;
            }
            ret = 0;
        }
    }
    libusb_free_config_descriptor(descConfig);
    return ret;
}

void LIBUSB_CALL HdcHostUSB::ReadUSBBulkCallback(struct libusb_transfer *transfer)
{
    HSession hSession = (HSession)transfer->user_data;
    HdcHostUSB *thisClass = (HdcHostUSB *)hSession->classModule;
    HUSB hUSB = hSession->hUSB;
    bool bOK = false;
    int err = 0;
    while (true) {
        if (!thisClass->modRunning || (hSession->isDead && 0 == hSession->sendRef))
            break;
        if (LIBUSB_TRANSFER_COMPLETED != transfer->status) {
            WRITE_LOG(LOG_FATAL, "Host usb not LIBUSB_TRANSFER_COMPLETED, status:%d", transfer->status);
            break;
        }
        if (!thisClass->SendToHdcStream(hSession, reinterpret_cast<uv_stream_t *>(&hSession->dataPipe[STREAM_MAIN]),
                                        hUSB->bufDevice, transfer->actual_length)) {
            break;
        }
        // loop self
        libusb_fill_bulk_transfer(transfer, hUSB->devHandle, hUSB->epDevice, hUSB->bufDevice, hUSB->bufSizeDevice,
                                  ReadUSBBulkCallback, hSession, 0);  // no user data
        err = libusb_submit_transfer(transfer);
        if (err < 0) {
            WRITE_LOG(LOG_FATAL, "libusb_submit_transfer failed, err:%d", err);
            break;
        }
        bOK = true;
        break;
    }
    if (!bOK) {
        auto server = reinterpret_cast<HdcServer *>(thisClass->clsMainBase);
        server->FreeSession(hSession->sessionId);
        WRITE_LOG(LOG_WARN, "ReadUSBBulkCallback failed");
        libusb_free_transfer(transfer);
        hUSB->transferRecv = nullptr;
    }
}

void HdcHostUSB::RegisterReadCallback(HSession hSession)
{
    HUSB hUSB = hSession->hUSB;
    if (hSession->isDead || !modRunning || hSession->hUSB->transferRecv) {
        return;
    }
    hSession->hUSB->transferRecv = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(hSession->hUSB->transferRecv, hUSB->devHandle, hUSB->epDevice, hUSB->bufDevice,
                              hUSB->bufSizeDevice,                // Note: in_buffer is where input data
                              ReadUSBBulkCallback, hSession, 0);  // no user data
    hSession->hUSB->transferRecv->user_data = hSession;
    libusb_submit_transfer(hSession->hUSB->transferRecv);
}

// ==0 Represents new equipment and is what we need,<0  my need
int HdcHostUSB::OpenDeviceMyNeed(HUSB hUSB)
{
    libusb_device *device = hUSB->device;
    int ret = -1;
    if (LIBUSB_SUCCESS != libusb_open(device, &hUSB->devHandle)) {
        return -100;
    }
    while (modRunning) {
        libusb_device_handle *handle = hUSB->devHandle;
        if (CheckDescriptor(hUSB)) {
            break;
        }
        if (CheckActiveConfig(device, hUSB)) {
            break;
        }
        // USB filter rules are set according to specific device
        // pedding device
        libusb_claim_interface(handle, hUSB->interfaceNumber);
        ret = 0;
        break;
    }
    if (ret) {
        // not my need device
        libusb_close(hUSB->devHandle);
        hUSB->devHandle = nullptr;
    }
    return ret;
}

// at main thread
void LIBUSB_CALL HdcHostUSB::WriteUSBBulkCallback(struct libusb_transfer *transfer)
{
    USBHead *usbHead = reinterpret_cast<USBHead *>(transfer->buffer);
    HSession hSession = reinterpret_cast<HSession>(transfer->user_data);
    HdcSessionBase *server = reinterpret_cast<HdcSessionBase *>(hSession->classInstance);
    HdcHostUSB *thisClass = reinterpret_cast<HdcHostUSB *>(hSession->classModule);
    if (usbHead->option & USB_OPTION_TAIL) {
        --hSession->sendRef;
    }
    uv_sem_post(&thisClass->semUsbSend);

    if (LIBUSB_TRANSFER_COMPLETED != transfer->status || (hSession->isDead && 0 == hSession->sendRef)) {
        WRITE_LOG(LOG_FATAL, "SendUSBRaw status:%d", transfer->status);
        if (hSession->hUSB->transferRecv != nullptr) {
            libusb_cancel_transfer(hSession->hUSB->transferRecv);
        }
        server->FreeSession(hSession->sessionId);
    }
    delete[] transfer->buffer;
    libusb_free_transfer(transfer);
}

bool HdcHostUSB::WaitMaxOverlap(HSession hSession)
{
    int result = 0;
    bool ret = false;
    while (true) {
        result = uv_sem_trywait(&semUsbSend);
        if (result == 0) {
            ret = true;
            break;
        } else if (result == UV_EAGAIN && !hSession->isDead) {
            uv_sleep(3);  // at least sleep 3ms to Hand over CPU for IO
            continue;
        } else {
            break;
        }
    }
    return ret;
}

// libusb can send directly across threads?!!!
// Just call from child work thread, it will be block when overlap full
int HdcHostUSB::SendUSBRaw(HSession hSession, uint8_t *data, const int length)
{
    int ret = ERR_GENERIC;
    HUSB hUSB = hSession->hUSB;
    libusb_transfer *transferUsb = libusb_alloc_transfer(0);
    uint8_t *sendBuf = nullptr;
    constexpr int retryTimeout = GLOBAL_TIMEOUT * TIME_BASE;
    while (true) {
        sendBuf = new uint8_t[length];
        if (!sendBuf) {
            ret = ERR_BUF_ALLOC;
            break;
        }
        if (memcpy_s(sendBuf, length, data, length) != EOK) {
            ret = ERR_BUF_COPY;
            break;
        }
        libusb_fill_bulk_transfer(transferUsb, hUSB->devHandle, hUSB->epHost, sendBuf, length, WriteUSBBulkCallback,
                                  hSession, retryTimeout);
        if (!WaitMaxOverlap(hSession)) {
            ret = ERR_THREAD_MUTEX_FAIL;
            break;
        }
        if (libusb_submit_transfer(transferUsb) < 0) {
            uv_sem_post(&semUsbSend);
            ret = ERR_IO_FAIL;
            break;
        }
        ret = length;
        break;
    }
    if (ret < 0) {
        --hSession->sendRef;
        if (sendBuf != nullptr) {
            delete[] sendBuf;
        }
        if (hUSB->transferRecv != nullptr) {
            libusb_cancel_transfer(hUSB->transferRecv);
        }
        libusb_free_transfer(transferUsb);
    }
    return ret;
}

bool HdcHostUSB::FindDeviceByID(HUSB hUSB, const char *usbMountPoint, libusb_context *ctxUSB)
{
    libusb_device **listDevices = nullptr;
    bool ret = false;
    char tmpStr[BUF_SIZE_TINY] = "";
    int busNum = 0;
    int devNum = 0;
    int curBus = 0;
    int curDev = 0;

    int device_num = libusb_get_device_list(ctxUSB, &listDevices);
    if (device_num <= 0) {
        libusb_free_device_list(listDevices, 1);
        return false;
    }
    if (strchr(usbMountPoint, '-') && EOK == strcpy_s(tmpStr, sizeof(tmpStr), usbMountPoint)) {
        *strchr(tmpStr, '-') = '\0';
        busNum = atoi(tmpStr);
        devNum = atoi(tmpStr + strlen(tmpStr) + 1);
    } else
        return false;

    int i = 0;
    for (i = 0; i < device_num; ++i) {
        struct libusb_device_descriptor desc;
        if (LIBUSB_SUCCESS != libusb_get_device_descriptor(listDevices[i], &desc)) {
            break;
        }
        curBus = libusb_get_bus_number(listDevices[i]);
        curDev = libusb_get_device_address(listDevices[i]);
        if ((curBus == busNum && curDev == devNum)) {
            hUSB->device = listDevices[i];
            int childRet = OpenDeviceMyNeed(hUSB);
            if (!childRet) {
                ret = true;
            }
            break;
        }
    }
    libusb_free_device_list(listDevices, 1);
    return ret;
}

bool HdcHostUSB::ReadyForWorkThread(HSession hSession)
{
    HdcUSBBase::ReadyForWorkThread(hSession);
    return true;
};

// Determines that daemonInfo must have the device
HSession HdcHostUSB::ConnectDetectDaemon(const HSession hSession, const HDaemonInfo pdi)
{
    HdcServer *pServer = (HdcServer *)clsMainBase;
    HUSB hUSB = hSession->hUSB;
    hUSB->usbMountPoint = pdi->usbMountPoint;
    hUSB->ctxUSB = ctxUSB;
    if (!FindDeviceByID(hUSB, hUSB->usbMountPoint.c_str(), hUSB->ctxUSB)) {
        pServer->FreeSession(hSession->sessionId);
        return nullptr;
    }
    UpdateUSBDaemonInfo(hUSB, hSession, STATUS_CONNECTED);
    RegisterReadCallback(hSession);

    hUSB->usbMountPoint = pdi->usbMountPoint;
    WRITE_LOG(LOG_DEBUG, "HSession HdcHostUSB::ConnectDaemon");

    Base::StartWorkThread(&pServer->loopMain, pServer->SessionWorkThread, Base::FinishWorkThread, hSession);
    // wait for thread up
    while (hSession->childLoop.active_handles == 0) {
        uv_sleep(1);
    }
    auto ctrl = pServer->BuildCtrlString(SP_START_SESSION, 0, nullptr, 0);
    Base::SendToStream((uv_stream_t *)&hSession->ctrlPipe[STREAM_MAIN], ctrl.data(), ctrl.size());
    return hSession;
}
}  // namespace Hdc
