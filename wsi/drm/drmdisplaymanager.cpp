/*
// Copyright (c) 2016 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "drmdisplaymanager.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <linux/netlink.h>
#include <linux/types.h>

#include <gpudevice.h>
#include <hwctrace.h>

#include <nativebufferhandler.h>

namespace hwcomposer {

DrmDisplayManager::DrmDisplayManager() : HWCThread(-8, "DisplayManager") {
  CTRACE();
}

DrmDisplayManager::~DrmDisplayManager() {
  CTRACE();
  std::vector<std::unique_ptr<DrmDisplay>>().swap(displays_);

#ifndef DISABLE_HOTPLUG_NOTIFICATION
  close(hotplug_fd_);
#endif
  drmClose(fd_);
  close(fd_);
  close(offscreen_fd_);
}

bool DrmDisplayManager::Initialize(int* scanout_device_no) {
  CTRACE();

  InitializePreferredScanoutDevice(scanout_device_no);

  if (fd_ < 0) {
    ETRACE("Failed to open dri %s", PRINTERROR());
    return -ENODEV;
  }

  IsDrmMasterByDefault();

  struct drm_set_client_cap cap = {DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1};
  drmIoctl(fd_, DRM_IOCTL_SET_CLIENT_CAP, &cap);
  int ret = drmSetClientCap(fd_, DRM_CLIENT_CAP_ATOMIC, 1);
  if (ret) {
    ETRACE("Failed to set atomic cap %s", PRINTERROR());
    return false;
  }

  ret = drmSetClientCap(fd_, DRM_CLIENT_CAP_ATOMIC, 1);
  if (ret) {
    ETRACE("Failed to set atomic cap %d", ret);
    return false;
  }

  ScopedDrmResourcesPtr res(drmModeGetResources(fd_));
  if (!res) {
    ETRACE("Failed to get resources");
    return false;
  }

  if (res->count_crtcs == 0) {
    res.reset();
    return false;
  }

  for (int32_t i = 0; i < res->count_crtcs; ++i) {
    ScopedDrmCrtcPtr c(drmModeGetCrtc(fd_, res->crtcs[i]));
    if (!c) {
      ETRACE("Failed to get crtc %d", res->crtcs[i]);
      res.reset();
      return false;
    }

    std::unique_ptr<DrmDisplay> display(
        new DrmDisplay(fd_, i, c->crtc_id, device_num_, this));

    displays_.emplace_back(std::move(display));

    c.reset();
  }

          ETRACE("Display Initialized-----------------------");

#ifndef DISABLE_HOTPLUG_NOTIFICATION
  hotplug_fd_ = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
  if (hotplug_fd_ < 0) {
    ETRACE("Failed to create socket for hot plug monitor. %s", PRINTERROR());
    res.reset();
    return true;
  }

  struct sockaddr_nl addr;
  memset(&addr, 0, sizeof(addr));
  addr.nl_family = AF_NETLINK;
  addr.nl_pid = getpid();
  addr.nl_groups = 0xffffffff;

  ret = bind(hotplug_fd_, (struct sockaddr *)&addr, sizeof(addr));
  if (ret) {
    ETRACE("Failed to bind sockaddr_nl and hot plug monitor fd. %s",
           PRINTERROR());
    res.reset();
    return true;
  }

  fd_handler_.AddFd(hotplug_fd_);
#endif

  IHOTPLUGEVENTTRACE("DisplayManager Initialization succeeded.");
  res.reset();
  return true;
}

void DrmDisplayManager::PrintDeviceInfo(drmDevicePtr device, int i, bool print_revision) {
    ETRACE("\n Info for device[%i]\n", i);
    ETRACE("+-> available_nodes %#04x\n", device->available_nodes);
    ETRACE("+-> nodes\n");
    for (int j = 0; j < DRM_NODE_MAX; j++)
        if (device->available_nodes & 1 << j)
            ETRACE("|   +-> nodes[%d] %s\n", j, device->nodes[j]);
    ETRACE("+-> bustype %04x\n", device->bustype);
    if (device->bustype == DRM_BUS_PCI) {
        ETRACE("|   +-> pci\n");
        ETRACE("|       +-> domain %04x\n",device->businfo.pci->domain);
        ETRACE("|       +-> bus    %02x\n", device->businfo.pci->bus);
        ETRACE("|       +-> dev    %02x\n", device->businfo.pci->dev);
        ETRACE("|       +-> func   %1u\n", device->businfo.pci->func);
        ETRACE("+-> deviceinfo\n");
        ETRACE("    +-> pci\n");
        ETRACE("        +-> vendor_id     %04x\n", device->deviceinfo.pci->vendor_id);
        ETRACE("        +-> device_id     %04x\n", device->deviceinfo.pci->device_id);
        ETRACE("        +-> subvendor_id  %04x\n", device->deviceinfo.pci->subvendor_id);
        ETRACE("        +-> subdevice_id  %04x\n", device->deviceinfo.pci->subdevice_id);
        if (print_revision)
            ETRACE("        +-> revision_id   %02x\n", device->deviceinfo.pci->revision_id);
        else
            ETRACE("        +-> revision_id   IGNORED\n");
    } else if (device->bustype == DRM_BUS_USB) {
        ETRACE("|   +-> usb\n");
        ETRACE("|       +-> bus %03u\n", device->businfo.usb->bus);
        ETRACE("|       +-> dev %03u\n", device->businfo.usb->dev);
        ETRACE("+-> deviceinfo\n");
        ETRACE("    +-> usb\n");
        ETRACE("        +-> vendor  %04x\n", device->deviceinfo.usb->vendor);
        ETRACE("        +-> product %04x\n", device->deviceinfo.usb->product);
    } else if (device->bustype == DRM_BUS_PLATFORM) {
        char **compatible = device->deviceinfo.platform->compatible;
        ETRACE("|   +-> platform\n");
        ETRACE("|       +-> fullname\t%s\n", device->businfo.platform->fullname);
        ETRACE("+-> deviceinfo\n");
        ETRACE("    +-> platform\n");
        ETRACE("        +-> compatible\n");
        while (*compatible) {
            ETRACE("                    %s\n", *compatible);
            compatible++;
        }
    } else if (device->bustype == DRM_BUS_HOST1X) {
        char **compatible = device->deviceinfo.host1x->compatible;
        ETRACE("|   +-> host1x\n");
        ETRACE("|       +-> fullname\t%s\n", device->businfo.host1x->fullname);
        ETRACE("+-> deviceinfo\n");
        ETRACE("    +-> host1x\n");
        ETRACE("        +-> compatible\n");
        while (*compatible) {
            ETRACE("                    %s\n", *compatible);
            compatible++;
        }
    } else {
        ETRACE("Unknown/unhandled bustype\n");
    }
    ETRACE("\n");
}

void DrmDisplayManager::InitializePreferredScanoutDevice(int* scanout_device_no) {
#define MAX_DRM_DEVICES 64
  drmDevicePtr devices[MAX_DRM_DEVICES], device;
  std::string card_path("/dev/dri/card0");
  int i, ret, num_devices, preferred_device = 0;

  num_devices = drmGetDevices2(0, devices, MAX_DRM_DEVICES);
  if (num_devices < 0) {
    ETRACE("drmGetDevices2() returned an error %d\n", num_devices);
    return;
  }

  // FIXME: Add support for getENV()

  if (preferred_device >= num_devices) {
    ETRACE("Preferred Device No is greater than the total avaiable devices Preffered Device No: %d Total Available Devices: %d. \n", preferred_device, num_devices);
    ETRACE("Will try using first available Device with Scanout support. \n");
    preferred_device = 0;
   }

  for (i = 0; i < num_devices; i++) {
    device = devices[i];

    /* Skip Non Intel GPU for now*/
    if (device->deviceinfo.pci->vendor_id != 0x8086) {
      continue;
    }

    int drm_node = DRM_NODE_PRIMARY;
    // Check if this device has available card node.
    if (!(device->available_nodes & 1 << drm_node)) {
      continue;
    }

    // We assume Card0 is expected to drive Display
    if (std::string(device->nodes[drm_node]).compare(card_path) != 0) {
      ETRACE(" Found a device but not card0 skipping \n");
      continue;
    }

    ETRACE(" Found a device which is card0 \n");
    // TODO: ADD check to see if the device is iGFX or dGFX.
    // Found Intel GPU break.
    preferred_device = i;
    break;
  }

   device = devices[preferred_device];
   // Pass the preffered device setting to the caller.
   *scanout_device_no = preferred_device;
   device_num_ = preferred_device;

   int drm_node = DRM_NODE_PRIMARY;
   // We don't do any sanity checks here. If we cannot open as primary device, we just fail the initialization.
   fd_ = open(device->nodes[drm_node], O_RDWR | FD_CLOEXEC, 0);
   if (fd_ == -1 || errno == EACCES) {
     ETRACE("Can't open GPU file %s \n", device->nodes[drm_node]);
     return;
   }

   ETRACE("card string %s \n", device->nodes[drm_node]);

   drm_node = DRM_NODE_RENDER;
   // Check if this device has available render node.
   if (device->available_nodes & 1 << drm_node) {
     offscreen_fd_ = open(device->nodes[drm_node], O_RDWR);
    if (offscreen_fd_ != -1)
      fcntl(offscreen_fd_, F_SETFD, fcntl(offscreen_fd_, F_GETFD) | FD_CLOEXEC);

    if (offscreen_fd_ == -1 && errno == EACCES) {
      ETRACE("Can't open GPU file for offscreen rendering with right permissions, falling back to Card Node %s \n", device->nodes[drm_node]);
      offscreen_fd_ = fd_;
    }
  }

   if (offscreen_fd_ == -1) {
     ETRACE("Can't open GPU file for offscreen rendering, falling back to Card Node %s \n", device->nodes[drm_node]);
     offscreen_fd_ = fd_;  // Reset preferred_scanout_device to first available device in case it's invalid.
   }

   PrintDeviceInfo(device, preferred_device, true);
   drmFreeDevices(devices, ret);
}

void DrmDisplayManager::HotPlugEventHandler() {
  CTRACE();
  int fd = hotplug_fd_;
  char buffer[DRM_HOTPLUG_EVENT_SIZE];
  int ret;

  memset(&buffer, 0, sizeof(buffer));
  while (true) {
    bool drm_event = false, hotplug_event = false;
    size_t srclen = DRM_HOTPLUG_EVENT_SIZE - 1;
    ret = read(fd, &buffer, srclen);
    if (ret <= 0) {
      if (ret < 0)
        ETRACE("Failed to read uevent. %s", PRINTERROR());

      return;
    }

    buffer[ret] = '\0';

    for (int32_t i = 0; i < ret;) {
      char *event = buffer + i;
      if (!strcmp(event, "DEVTYPE=drm_minor"))
        drm_event = true;
      else if (!strcmp(event, "HOTPLUG=1") ||  // Common hotplug request
               !strcmp(event,
                       "HDMI-Change")) {  // Hotplug happened during suspend
        hotplug_event = true;
      }

      if (hotplug_event && drm_event)
        break;

      i += strlen(event) + 1;
    }

    if (drm_event && hotplug_event) {
      IHOTPLUGEVENTTRACE(
          "Recieved Hot Plug event related to display calling "
          "UpdateDisplayState.");
      UpdateDisplayState();
    }
  }
}

void DrmDisplayManager::HandleWait() {
  if (fd_handler_.Poll(-1) <= 0) {
    ETRACE("Poll Failed in DisplayManager %s", PRINTERROR());
  }
}

void DrmDisplayManager::InitializeDisplayResources() {
  buffer_handler_.reset(NativeBufferHandler::CreateInstance(fd_));
  frame_buffer_manager_.reset(new FrameBufferManager(fd_));
  if (!buffer_handler_) {
    ETRACE("Failed to create native buffer handler instance");
    return;
  }

  int size = displays_.size();
  for (int i = 0; i < size; ++i) {
    if (!displays_.at(i)->Initialize(buffer_handler_.get())) {
      ETRACE("Failed to Initialize Display %d", i);
    }
  }
}

void DrmDisplayManager::StartHotPlugMonitor() {
  if (!UpdateDisplayState()) {
    ETRACE("Failed to connect display.");
  }

  if (!InitWorker()) {
    ETRACE("Failed to initalizer thread to monitor Hot Plug events. %s",
           PRINTERROR());
  }
}

void DrmDisplayManager::HandleRoutine() {
  CTRACE();
  IHOTPLUGEVENTTRACE("DisplayManager::Routine.");
  if (fd_handler_.IsReady(hotplug_fd_)) {
    IHOTPLUGEVENTTRACE("Recieved Hot plug notification.");
    HotPlugEventHandler();
  }
}

bool DrmDisplayManager::UpdateDisplayState() {
  CTRACE();
  ScopedDrmResourcesPtr res(drmModeGetResources(fd_));
  if (!res) {
    ETRACE("Failed to get DrmResources resources");
    return false;
  }

#ifndef USE_MUTEX
  spin_lock_.lock();
#else
  mLock.lock();
#endif
  // Start of assuming no displays are connected
  for (auto &display : displays_) {
    if (device_.IsReservedDrmPlane() && !display->IsConnected())
      display->SetPlanesUpdated(false);
    display->MarkForDisconnect();
  }

  connected_display_count_ = 0;
  std::vector<NativeDisplay *> connected_displays;
  std::vector<uint32_t> no_encoder;
  uint32_t total_connectors = res->count_connectors;
  for (uint32_t i = 0; i < total_connectors; ++i) {
    ScopedDrmConnectorPtr connector(
        drmModeGetConnector(fd_, res->connectors[i]));
    if (!connector) {
      ETRACE("Failed to get connector %d", res->connectors[i]);
      break;
    }
    // check if a monitor is connected.
    if (connector->connection != DRM_MODE_CONNECTED) {
      connector.reset();
      continue;
    }
    connected_display_count_++;
    connector.reset();
  }

  for (uint32_t i = 0; i < total_connectors; ++i) {
    ScopedDrmConnectorPtr connector(
        drmModeGetConnector(fd_, res->connectors[i]));
    if (!connector) {
      ETRACE("Failed to get connector %d", res->connectors[i]);
      break;
    }
    // check if a monitor is connected.
    if (connector->connection != DRM_MODE_CONNECTED) {
      connector.reset();
      continue;
    }

    // Ensure we have atleast one valid mode.
    if (connector->count_modes == 0) {
      connector.reset();
      continue;
    }

    if (connector->encoder_id == 0) {
      no_encoder.emplace_back(i);
      connector.reset();
      continue;
    }

    std::vector<drmModeModeInfo> mode;
    uint32_t preferred_mode = 0;
    uint32_t size = connector->count_modes;
    mode.resize(size);
    for (uint32_t i = 0; i < size; ++i) {
      mode[i] = connector->modes[i];
      // There is only one preferred mode per connector.
      if (mode[i].type & DRM_MODE_TYPE_PREFERRED) {
        preferred_mode = i;
      }
    }

    // Lets try to find crts for any connected encoder.
    ScopedDrmEncoderPtr encoder(drmModeGetEncoder(fd_, connector->encoder_id));
    if (encoder && encoder->crtc_id) {
      for (auto &display : displays_) {
        IHOTPLUGEVENTTRACE(
            "Trying to connect %d with crtc: %d is display connected: %d \n",
            encoder->crtc_id, display->CrtcId(), display->IsConnected());
        // At initilaization  preferred mode is set!
        if (!display->IsConnected() && encoder->crtc_id == display->CrtcId() &&
            display->ConnectDisplay(mode.at(preferred_mode), connector.get(),
                                    preferred_mode)) {
          IHOTPLUGEVENTTRACE("Connected %d with crtc: %d pipe:%d \n",
                             encoder->crtc_id, display->CrtcId(),
                             display->GetDisplayPipe());
          // Set the modes supported for each display
          display->SetDrmModeInfo(mode);
          break;
        }
      }
    }

    encoder.reset();
    connector.reset();
  }

  // Deal with connectors with encoder_id == 0.
  uint32_t size = no_encoder.size();
  for (uint32_t i = 0; i < size; ++i) {
    ScopedDrmConnectorPtr connector(
        drmModeGetConnector(fd_, res->connectors[no_encoder.at(i)]));
    if (!connector) {
      ETRACE("Failed to get connector %d", res->connectors[i]);
      break;
    }

    std::vector<drmModeModeInfo> mode;
    uint32_t preferred_mode = 0;
    uint32_t size = connector->count_modes;
    mode.resize(size);
    for (uint32_t i = 0; i < size; ++i) {
      mode[i] = connector->modes[i];
      // There is only one preferred mode per connector.
      if (mode[i].type & DRM_MODE_TYPE_PREFERRED) {
        preferred_mode = i;
      }
    }

    // Try to find an encoder for the connector.
    size = connector->count_encoders;
    for (uint32_t j = 0; j < size; ++j) {
      ScopedDrmEncoderPtr encoder(
          drmModeGetEncoder(fd_, connector->encoders[j]));
      if (!encoder)
        continue;

      for (auto &display : displays_) {
        if (!display->IsConnected() &&
            (encoder->possible_crtcs & (1 << display->GetDisplayPipe())) &&
            display->ConnectDisplay(mode.at(preferred_mode), connector.get(),
                                    preferred_mode)) {
          IHOTPLUGEVENTTRACE("Connected with crtc: %d pipe:%d \n",
                             display->CrtcId(), display->GetDisplayPipe());
          // Set the modes supported for each display
          display->SetDrmModeInfo(mode);
          break;
        }
      }

      encoder.reset();
    }

    connector.reset();
  }

  for (auto &display : displays_) {
    if (!display->IsConnected()) {
      display->DisConnect();
    } else if (callback_) {
      connected_displays.emplace_back(display.get());
    }
  }

  if (callback_) {
    callback_->Callback(connected_displays);
  }

#ifndef USE_MUTEX
  spin_lock_.unlock();
#else
  mLock.unlock();
#endif
#ifndef ENABLE_ANDROID_WA
  notify_client_ = true;
#endif

  if (notify_client_ || (!(displays_.at(0)->IsConnected()))) {
    IHOTPLUGEVENTTRACE("NotifyClientsOfDisplayChangeStatus Called %d %d \n",
                       notify_client_, displays_.at(0)->IsConnected());
    NotifyClientsOfDisplayChangeStatus();
  }

  // update plane list for reservation
  if (device_.IsReservedDrmPlane())
    RemoveUnreservedPlanes();

  res.reset();
  return true;
}

void DrmDisplayManager::NotifyClientsOfDisplayChangeStatus() {
#ifndef USE_MUTEX
  spin_lock_.lock();
#else
  std::lock_guard<std::mutex> lock(mLock);
#endif

  for (auto &display : displays_) {
    if (!display->IsConnected()) {
      display->NotifyClientOfDisConnectedState();
    } else {
      display->NotifyClientOfConnectedState();
    }
  }

#ifdef ENABLE_ANDROID_WA
  notify_client_ = true;
#endif

#ifndef USE_MUTEX
  spin_lock_.unlock();
#endif
}

NativeDisplay *DrmDisplayManager::CreateVirtualDisplay(uint32_t display_index) {
  NativeDisplay *latest_display;
  std::unique_ptr<VirtualDisplay> display(
      new VirtualDisplay(fd_, buffer_handler_.get(), display_index, 0));
  virtual_displays_.emplace(display_index, std::move(display));
  latest_display = virtual_displays_.at(display_index).get();
  return latest_display;
}

void DrmDisplayManager::DestroyVirtualDisplay(uint32_t display_index) {
  auto it = virtual_displays_.find(display_index);
  if (it != virtual_displays_.end()) {
    virtual_displays_.at(display_index).reset(nullptr);
    virtual_displays_.erase(display_index);
  }
}

std::vector<NativeDisplay *> DrmDisplayManager::GetAllDisplays() {
#ifndef USE_MUTEX
  spin_lock_.lock();
#else
  std::lock_guard<std::mutex> lock(mLock);
#endif
  std::vector<NativeDisplay *> all_displays;
  size_t size = displays_.size();
  for (size_t i = 0; i < size; ++i) {
    all_displays.emplace_back(displays_.at(i).get());
  }
#ifndef USE_MUTEX
  spin_lock_.unlock();
#endif
  return all_displays;
}

void DrmDisplayManager::RegisterHotPlugEventCallback(
    std::shared_ptr<DisplayHotPlugEventCallback> callback) {
#ifndef USE_MUTEX
  spin_lock_.lock();
#else
  std::lock_guard<std::mutex> lock(mLock);
#endif
  callback_ = callback;
#ifndef USE_MUTEX
  spin_lock_.unlock();
#endif
}

void DrmDisplayManager::ForceRefresh() {
#ifndef USE_MUTEX
  spin_lock_.lock();
#else
  std::lock_guard<std::mutex> lock(mLock);
#endif
  ignore_updates_ = false;
  size_t size = displays_.size();
  for (size_t i = 0; i < size; ++i) {
    displays_.at(i)->ForceRefresh();
  }

  release_lock_ = true;
#ifndef USE_MUTEX
  spin_lock_.unlock();
#endif
}

void DrmDisplayManager::IgnoreUpdates() {
#ifndef USE_MUTEX
  spin_lock_.lock();
#else
  mLock.lock();
#endif
  ignore_updates_ = true;
#ifndef USE_MUTEX
  spin_lock_.unlock();
#else
  mLock.unlock();
#endif

  size_t size = displays_.size();
  for (size_t i = 0; i < size; ++i) {
    displays_.at(i)->IgnoreUpdates();
  }
}

bool DrmDisplayManager::IsDrmMasterByDefault() {
#ifndef USE_MUTEX
  spin_lock_.lock();
#else
  std::lock_guard<std::mutex> lock(mLock);
#endif
  if (drm_master_) {
    spin_lock_.unlock();
    return drm_master_;
  }
  drm_magic_t magic = 0;
  int ret = 0;
  ret = drmGetMagic(fd_, &magic);
  if (ret)
    ETRACE("Failed to call drmGetMagic : %s", PRINTERROR());
  else {
    ret = drmAuthMagic(fd_, magic);
    if (ret)
      ETRACE("Failed to call drmAuthMagic : %s", PRINTERROR());
    else
      drm_master_ = true;
  }
#ifndef USE_MUTEX
  spin_lock_.unlock();
#endif
  return drm_master_;
}

void DrmDisplayManager::setDrmMaster(bool must_set) {
#ifndef USE_MUTEX
  spin_lock_.lock();
#else
  std::lock_guard<std::mutex> lock(mLock);
#endif
  if (drm_master_) {
    spin_lock_.unlock();
    return;
  }
  int ret = 0;
  uint8_t retry_times = 0;
  do {
    ret = drmSetMaster(fd_);
    if (!must_set)
      retry_times++;
    if (ret) {
      ETRACE("Failed to call drmSetMaster : %s", PRINTERROR());
      drm_master_ = false;
      usleep(10000);
    } else {
      ITRACE("Successfully set as DRM master.");
      drm_master_ = true;
    }
  } while (ret && retry_times < 10);
#ifndef USE_MUTEX
  spin_lock_.unlock();
#endif
}

void DrmDisplayManager::DropDrmMaster() {
#ifndef USE_MUTEX
  spin_lock_.lock();
#else
  std::lock_guard<std::mutex> lock(mLock);
#endif
  if (!drm_master_) {
#ifndef USE_MUTEX
    spin_lock_.unlock();
#endif
    return;
  }
  int ret = 0;
  uint8_t retry_times = 0;
  do {
    ret = drmDropMaster(fd_);
    retry_times++;
    if (ret) {
      ETRACE("Failed to call drmDropMaster : %s", PRINTERROR());
      usleep(10000);
    } else {
      ITRACE("Successfully drop DRM master.");
      drm_master_ = false;
    }
  } while (ret && retry_times < 10);
#ifndef USE_MUTEX
  spin_lock_.unlock();
#endif
}

void DrmDisplayManager::HandleLazyInitialization() {
#ifndef USE_MUTEX
  spin_lock_.lock();
#else
  std::lock_guard<std::mutex> lock(mLock);
#endif
  if (release_lock_) {
    device_.DisableWatch();
    release_lock_ = false;
  }
#ifndef USE_MUTEX
  spin_lock_.unlock();
#endif
}

uint32_t DrmDisplayManager::GetConnectedPhysicalDisplayCount() {
  return connected_display_count_;
}

DisplayManager *DisplayManager::CreateDisplayManager() {
  return new DrmDisplayManager();
}

void DrmDisplayManager::EnableHDCPSessionForDisplay(
    uint32_t connector, HWCContentType content_type) {
  size_t size = displays_.size();
  for (size_t i = 0; i < size; i++) {
    if (displays_.at(i)->GetConnectorID() == connector) {
      displays_.at(i)->SetHDCPState(HWCContentProtection::kDesired,
                                    content_type);
    }
  }
}

void DrmDisplayManager::EnableHDCPSessionForAllDisplays(
    HWCContentType content_type) {
  size_t size = displays_.size();
  for (size_t i = 0; i < size; i++) {
    displays_.at(i)->SetHDCPState(HWCContentProtection::kDesired, content_type);
  }
}

void DrmDisplayManager::DisableHDCPSessionForDisplay(uint32_t connector) {
  size_t size = displays_.size();
  for (size_t i = 0; i < size; i++) {
    if (displays_.at(i)->GetConnectorID() == connector) {
      displays_.at(i)->SetHDCPState(HWCContentProtection::kUnDesired,
                                    HWCContentType::kInvalid);
    }
  }
}

void DrmDisplayManager::DisableHDCPSessionForAllDisplays() {
  size_t size = displays_.size();
  for (size_t i = 0; i < size; i++) {
    displays_.at(i)->SetHDCPState(HWCContentProtection::kUnDesired,
                                  HWCContentType::kInvalid);
  }
}

void DrmDisplayManager::SetHDCPSRMForAllDisplays(const int8_t *SRM,
                                                 uint32_t SRMLength) {
  size_t size = displays_.size();
  for (size_t i = 0; i < size; i++) {
    displays_.at(i)->SetHDCPSRM(SRM, SRMLength);
  }
}

void DrmDisplayManager::SetHDCPSRMForDisplay(uint32_t connector,
                                             const int8_t *SRM,
                                             uint32_t SRMLength) {
  size_t size = displays_.size();
  for (size_t i = 0; i < size; i++) {
    if (displays_.at(i)->GetConnectorID() == connector) {
      displays_.at(i)->SetHDCPSRM(SRM, SRMLength);
    }
  }
}

void DrmDisplayManager::RemoveUnreservedPlanes() {
  size_t size = displays_.size();
  for (uint8_t i = 0; i < size; i++) {
    if (!displays_.at(i)->IsConnected() || displays_.at(i)->IsPlanesUpdated())
      continue;
    std::vector<uint32_t> reserved_planes = device_.GetDisplayReservedPlanes(i);
    if (!reserved_planes.empty() && reserved_planes.size() < 4)
      displays_.at(i)->ReleaseUnreservedPlanes(reserved_planes);
    displays_.at(i)->SetPlanesUpdated(true);
  }
}

FrameBufferManager *DrmDisplayManager::GetFrameBufferManager() {
  return frame_buffer_manager_.get();
}

#ifdef ENABLE_PANORAMA
NativeDisplay *DrmDisplayManager::CreateVirtualPanoramaDisplay(
    uint32_t display_index) {
  NativeDisplay *latest_display;
  latest_display = (NativeDisplay *)new VirtualPanoramaDisplay(
      fd_, buffer_handler_.get(), display_index, 0);
  return latest_display;
}
#endif

}  // namespace hwcomposer
