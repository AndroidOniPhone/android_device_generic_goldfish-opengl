// Copyright (C) 2018 The Android Open Source Project
// Copyright (C) 2018 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ResourceTracker.h"

#include "../OpenglSystemCommon/EmulatorFeatureInfo.h"
#include "HostVisibleMemoryVirtualization.h"
#include "Resources.h"
#include "VkEncoder.h"

#include "android/base/AlignedBuf.h"
#include "android/base/synchronization/AndroidLock.h"

#include "gralloc_cb.h"
#include "goldfish_address_space.h"
#include "goldfish_vk_private_defs.h"

#include <unordered_map>

#include <log/log.h>
#include <stdlib.h>
#include <sync/sync.h>

#define RESOURCE_TRACKER_DEBUG 0

#if RESOURCE_TRACKER_DEBUG
#define D(fmt,...) ALOGD("%s: " fmt, __func__, ##__VA_ARGS__);
#else
#ifndef D
#define D(fmt,...)
#endif
#endif

using android::aligned_buf_alloc;
using android::aligned_buf_free;
using android::base::guest::AutoLock;
using android::base::guest::Lock;

namespace goldfish_vk {

#define MAKE_HANDLE_MAPPING_FOREACH(type_name, map_impl, map_to_u64_impl, map_from_u64_impl) \
    void mapHandles_##type_name(type_name* handles, size_t count) override { \
        for (size_t i = 0; i < count; ++i) { \
            map_impl; \
        } \
    } \
    void mapHandles_##type_name##_u64(const type_name* handles, uint64_t* handle_u64s, size_t count) override { \
        for (size_t i = 0; i < count; ++i) { \
            map_to_u64_impl; \
        } \
    } \
    void mapHandles_u64_##type_name(const uint64_t* handle_u64s, type_name* handles, size_t count) override { \
        for (size_t i = 0; i < count; ++i) { \
            map_from_u64_impl; \
        } \
    } \

#define DEFINE_RESOURCE_TRACKING_CLASS(class_name, impl) \
class class_name : public VulkanHandleMapping { \
public: \
    virtual ~class_name() { } \
    GOLDFISH_VK_LIST_HANDLE_TYPES(impl) \
}; \

#define CREATE_MAPPING_IMPL_FOR_TYPE(type_name) \
    MAKE_HANDLE_MAPPING_FOREACH(type_name, \
        handles[i] = new_from_host_##type_name(handles[i]); ResourceTracker::get()->register_##type_name(handles[i]);, \
        handle_u64s[i] = (uint64_t)new_from_host_##type_name(handles[i]), \
        handles[i] = (type_name)new_from_host_u64_##type_name(handle_u64s[i]); ResourceTracker::get()->register_##type_name(handles[i]);)

#define UNWRAP_MAPPING_IMPL_FOR_TYPE(type_name) \
    MAKE_HANDLE_MAPPING_FOREACH(type_name, \
        handles[i] = get_host_##type_name(handles[i]), \
        handle_u64s[i] = (uint64_t)get_host_u64_##type_name(handles[i]), \
        handles[i] = (type_name)get_host_##type_name((type_name)handle_u64s[i]))

#define DESTROY_MAPPING_IMPL_FOR_TYPE(type_name) \
    MAKE_HANDLE_MAPPING_FOREACH(type_name, \
        ResourceTracker::get()->unregister_##type_name(handles[i]); delete_goldfish_##type_name(handles[i]), \
        (void)handle_u64s[i]; delete_goldfish_##type_name(handles[i]), \
        (void)handles[i]; delete_goldfish_##type_name((type_name)handle_u64s[i]))

DEFINE_RESOURCE_TRACKING_CLASS(CreateMapping, CREATE_MAPPING_IMPL_FOR_TYPE)
DEFINE_RESOURCE_TRACKING_CLASS(UnwrapMapping, UNWRAP_MAPPING_IMPL_FOR_TYPE)
DEFINE_RESOURCE_TRACKING_CLASS(DestroyMapping, DESTROY_MAPPING_IMPL_FOR_TYPE)

class ResourceTracker::Impl {
public:
    Impl() = default;
    CreateMapping createMapping;
    UnwrapMapping unwrapMapping;
    DestroyMapping destroyMapping;
    DefaultHandleMapping defaultMapping;

#define HANDLE_DEFINE_TRIVIAL_INFO_STRUCT(type) \
    struct type##_Info { \
        uint32_t unused; \
    }; \

    GOLDFISH_VK_LIST_TRIVIAL_HANDLE_TYPES(HANDLE_DEFINE_TRIVIAL_INFO_STRUCT)

    struct VkDevice_Info {
        VkPhysicalDevice physdev;
        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceMemoryProperties memProps;
        HostVisibleMemoryVirtualizationInfo hostVisibleVirtInfo;
    };

    struct VkDeviceMemory_Info {
        VkDeviceSize allocationSize = 0;
        VkDeviceSize mappedSize = 0;
        uint8_t* mappedPtr = nullptr;
        uint32_t memoryTypeIndex = 0;
        bool directMapped = false;
        std::unique_ptr<GoldfishAddressSpaceBlock>
            goldfishAddressSpaceBlock = {};
        HostVisibleMemoryVirtualizationInfo
            hostVisibleVirtInfo;
    };

#define HANDLE_REGISTER_IMPL_IMPL(type) \
    std::unordered_map<type, type##_Info> info_##type; \
    void register_##type(type obj) { \
        AutoLock lock(mLock); \
        info_##type[obj] = type##_Info(); \
    } \

#define HANDLE_UNREGISTER_IMPL_IMPL(type) \
    void unregister_##type(type obj) { \
        AutoLock lock(mLock); \
        info_##type.erase(obj); \
    } \

    GOLDFISH_VK_LIST_HANDLE_TYPES(HANDLE_REGISTER_IMPL_IMPL)
    GOLDFISH_VK_LIST_TRIVIAL_HANDLE_TYPES(HANDLE_UNREGISTER_IMPL_IMPL)

    void unregister_VkDevice(VkDevice device) {
        AutoLock lock(mLock);
        info_VkDevice.erase(device);
    }

    void unregister_VkDeviceMemory(VkDeviceMemory mem) {
        AutoLock lock(mLock);

        auto it = info_VkDeviceMemory.find(mem);
        if (it == info_VkDeviceMemory.end()) return;

        auto& memInfo = it->second;

        if (memInfo.mappedPtr && !memInfo.directMapped) {
            aligned_buf_free(memInfo.mappedPtr);
        }

        // Direct mapping is erased by GoldfishAddressSpaceBlock's
        // dtor
        info_VkDeviceMemory.erase(mem);
    }

    void setDeviceInfo(VkDevice device,
                       VkPhysicalDevice physdev,
                       VkPhysicalDeviceProperties props,
                       VkPhysicalDeviceMemoryProperties memProps) {
        AutoLock lock(mLock);
        auto& info = info_VkDevice[device];
        info.physdev = physdev;
        info.props = props;
        info.memProps = memProps;
        initHostVisibleMemoryVirtualizationInfo(
            physdev, &memProps, &info.hostVisibleVirtInfo);
    }

    void setDeviceMemoryInfo(VkDevice device,
                             VkDeviceMemory memory,
                             VkDeviceSize allocationSize,
                             VkDeviceSize mappedSize,
                             uint8_t* ptr,
                             uint32_t memoryTypeIndex) {
        AutoLock lock(mLock);
        auto& deviceInfo = info_VkDevice[device];
        auto& info = info_VkDeviceMemory[memory];

        info.allocationSize = allocationSize;
        info.mappedSize = mappedSize;
        info.mappedPtr = ptr;
        info.memoryTypeIndex = memoryTypeIndex;
        info.hostVisibleVirtInfo = deviceInfo.hostVisibleVirtInfo;
    }

    bool isMemoryTypeHostVisible(VkDevice device, uint32_t typeIndex) const {
        AutoLock lock(mLock);
        const auto it = info_VkDevice.find(device);

        if (it == info_VkDevice.end()) return false;

        const auto& info = it->second;
        return info.memProps.memoryTypes[typeIndex].propertyFlags &
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    }

    uint8_t* getMappedPointer(VkDeviceMemory memory) {
        AutoLock lock(mLock);
        const auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) return nullptr;

        const auto& info = it->second;
        return info.mappedPtr;
    }

    VkDeviceSize getMappedSize(VkDeviceMemory memory) {
        AutoLock lock(mLock);
        const auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) return 0;

        const auto& info = it->second;
        return info.mappedSize;
    }

    VkDeviceSize getNonCoherentExtendedSize(VkDevice device, VkDeviceSize basicSize) const {
        AutoLock lock(mLock);
        const auto it = info_VkDevice.find(device);
        if (it == info_VkDevice.end()) return basicSize;
        const auto& info = it->second;

        VkDeviceSize nonCoherentAtomSize =
            info.props.limits.nonCoherentAtomSize;
        VkDeviceSize atoms =
            (basicSize + nonCoherentAtomSize - 1) / nonCoherentAtomSize;
        return atoms * nonCoherentAtomSize;
    }

    bool isValidMemoryRange(const VkMappedMemoryRange& range) const {
        AutoLock lock(mLock);
        const auto it = info_VkDeviceMemory.find(range.memory);
        if (it == info_VkDeviceMemory.end()) return false;
        const auto& info = it->second;

        if (!info.mappedPtr) return false;

        VkDeviceSize offset = range.offset;
        VkDeviceSize size = range.size;

        if (size == VK_WHOLE_SIZE) {
            return offset <= info.mappedSize;
        }

        return offset + size <= info.mappedSize;
    }

    void setupFeatures(const EmulatorFeatureInfo* features) {
        if (!features || mFeatureInfo) return;
        mFeatureInfo.reset(new EmulatorFeatureInfo);
        *mFeatureInfo = *features;

        if (mFeatureInfo->hasDirectMem) {
            mGoldfishAddressSpaceBlockProvider.reset(
                new GoldfishAddressSpaceBlockProvider);
        }
    }

    bool usingDirectMapping() const {
        return mFeatureInfo->hasDirectMem;
    }

    void deviceMemoryTransform_tohost(
        VkDeviceMemory* memory, uint32_t memoryCount,
        VkDeviceSize* offset, uint32_t offsetCount,
        VkDeviceSize* size, uint32_t sizeCount,
        uint32_t* typeIndex, uint32_t typeIndexCount,
        uint32_t* typeBits, uint32_t typeBitsCount) {
        
        if (memoryCount != offsetCount ||
            offsetCount != sizeCount ||
            sizeCount != memoryCount) {
            ALOGE("FATAL: Mismatched counts for device memory info");
            abort();
        }

        for (uint32_t i = 0; i < memoryCount; ++i) {
            // TODO
            (void)memory;
            (void)offset;
            (void)size;
        }

        for (uint32_t i = 0; i < typeIndexCount; ++i) {
            // TODO
            (void)typeIndex;
        }

        for (uint32_t i = 0; i < typeBitsCount; ++i) {
            // TODO
            (void)typeBits;
        }
    }

    void deviceMemoryTransform_fromhost(
        VkDeviceMemory* memory, uint32_t memoryCount,
        VkDeviceSize* offset, uint32_t offsetCount,
        VkDeviceSize* size, uint32_t sizeCount,
        uint32_t* typeIndex, uint32_t typeIndexCount,
        uint32_t* typeBits, uint32_t typeBitsCount) {
        
        if (memoryCount != offsetCount ||
            offsetCount != sizeCount ||
            sizeCount != memoryCount) {
            ALOGE("FATAL: Mismatched counts for device memory info");
            abort();
        }

        for (uint32_t i = 0; i < memoryCount; ++i) {
            // TODO
            (void)memory;
            (void)offset;
            (void)size;
        }

        for (uint32_t i = 0; i < typeIndexCount; ++i) {
            // TODO
            (void)typeIndex;
        }

        for (uint32_t i = 0; i < typeBitsCount; ++i) {
            // TODO
            (void)typeBits;
        }
    }

    VkResult on_vkEnumerateInstanceVersion(
        void*,
        VkResult,
        uint32_t* apiVersion) {
        if (apiVersion) {
            *apiVersion = VK_MAKE_VERSION(1, 0, 0);
        }
        return VK_SUCCESS;
    }

    VkResult on_vkEnumerateDeviceExtensionProperties(
        void*,
        VkResult,
        VkPhysicalDevice,
        const char*,
        uint32_t* pPropertyCount,
        VkExtensionProperties*) {
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }

    void on_vkGetPhysicalDeviceProperties2(
        void*,
        VkPhysicalDevice,
        VkPhysicalDeviceProperties2*) {
        // no-op
    }

    void on_vkGetPhysicalDeviceMemoryProperties(
        void*,
        VkPhysicalDevice,
        VkPhysicalDeviceMemoryProperties*) { }

    VkResult on_vkCreateDevice(
        void* context,
        VkResult input_result,
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo*,
        const VkAllocationCallbacks*,
        VkDevice* pDevice) {

        if (input_result != VK_SUCCESS) return input_result;

        VkEncoder* enc = (VkEncoder*)context;

        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceMemoryProperties memProps;
        enc->vkGetPhysicalDeviceProperties(physicalDevice, &props);
        enc->vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

        setDeviceInfo(*pDevice, physicalDevice, props, memProps);

        return input_result;
    }

    VkResult on_vkAllocateMemory(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkMemoryAllocateInfo* pAllocateInfo,
        const VkAllocationCallbacks*,
        VkDeviceMemory* pMemory) {

        if (input_result != VK_SUCCESS) return input_result;

        VkDeviceSize allocationSize = pAllocateInfo->allocationSize;
        VkDeviceSize mappedSize = getNonCoherentExtendedSize(device, allocationSize);
        uint8_t* mappedPtr = nullptr;
        bool hostVisible =
            isMemoryTypeHostVisible(device, pAllocateInfo->memoryTypeIndex);
        if (hostVisible && !mGoldfishAddressSpaceBlockProvider) {
            mappedPtr = (uint8_t*)aligned_buf_alloc(4096, mappedSize);
            D("host visible alloc (non-direct): "
              "size 0x%llx host ptr %p mapped size 0x%llx",
              (unsigned long long)allocationSize, mappedPtr,
              (unsigned long long)mappedSize);
        }

        setDeviceMemoryInfo(
            device, *pMemory, allocationSize, mappedSize, mappedPtr,
            pAllocateInfo->memoryTypeIndex);

        bool doDirectMap =
            hostVisible && mGoldfishAddressSpaceBlockProvider;

        if (doDirectMap) {
            VkEncoder* enc = (VkEncoder*)context;

            uint64_t directMappedAddr = 0;

            VkResult directMapResult =
                enc->vkMapMemoryIntoAddressSpaceGOOGLE(
                    device, *pMemory, &directMappedAddr);

            if (directMapResult != VK_SUCCESS) {
                return directMapResult;
            }

            AutoLock lock(mLock);

            auto it = info_VkDeviceMemory.find(*pMemory);
            if (it == info_VkDeviceMemory.end()) {
                return VK_ERROR_INITIALIZATION_FAILED;
            }

            auto& info = it->second;
            info.mappedPtr = (uint8_t*)(uintptr_t)directMappedAddr;
            info.directMapped = true;

            D("host visible alloc (direct): "
              "size 0x%llx host ptr %p mapped size 0x%llx",
              (unsigned long long)allocationSize, info.mappedPtr,
              (unsigned long long)mappedSize);
        }

        return input_result;
    }

    VkResult on_vkMapMemory(
        void*,
        VkResult host_result,
        VkDevice,
        VkDeviceMemory memory,
        VkDeviceSize offset,
        VkDeviceSize size,
        VkMemoryMapFlags,
        void** ppData) {

        if (host_result != VK_SUCCESS) return host_result;

        AutoLock lock(mLock);

        auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) return VK_ERROR_MEMORY_MAP_FAILED;

        auto& info = it->second;

        if (!info.mappedPtr) return VK_ERROR_MEMORY_MAP_FAILED;

        if (size != VK_WHOLE_SIZE &&
            (info.mappedPtr + offset + size > info.mappedPtr + info.allocationSize)) {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }

        *ppData = info.mappedPtr + offset;

        return host_result;
    }

    void on_vkUnmapMemory(
        void*,
        VkDevice,
        VkDeviceMemory) {
        // no-op
    }

    void unwrap_VkNativeBufferANDROID(
        const VkImageCreateInfo* pCreateInfo,
        VkImageCreateInfo* local_pCreateInfo) {

        if (!pCreateInfo->pNext) return;

        const VkNativeBufferANDROID* nativeInfo =
            reinterpret_cast<const VkNativeBufferANDROID*>(pCreateInfo->pNext);

        if (VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID != nativeInfo->sType) {
            return;
        }

        const cb_handle_t* cb_handle =
            reinterpret_cast<const cb_handle_t*>(nativeInfo->handle);

        if (!cb_handle) return;

        VkNativeBufferANDROID* nativeInfoOut =
            reinterpret_cast<VkNativeBufferANDROID*>(
                const_cast<void*>(
                    local_pCreateInfo->pNext));

        if (!nativeInfoOut->handle) {
            ALOGE("FATAL: Local native buffer info not properly allocated!");
            abort();
        }

        *(uint32_t*)(nativeInfoOut->handle) = cb_handle->hostHandle;
    }

    void unwrap_vkAcquireImageANDROID_nativeFenceFd(int fd, int*) {
        if (fd != -1) {
            sync_wait(fd, 3000);
        }
    }

    // Action of vkMapMemoryIntoAddressSpaceGOOGLE:
    // 1. preprocess (on_vkMapMemoryIntoAddressSpaceGOOGLE_pre):
    //    uses address space device to reserve the right size of
    //    memory.
    // 2. the reservation results in a physical address. the physical
    //    address is set as |*pAddress|.
    // 3. after pre, the API call is encoded to the host, where the
    //    value of pAddress is also sent (the physical address).
    // 4. the host will obtain the actual gpu pointer and send it
    //    back out in |*pAddress|.
    // 5. postprocess (on_vkMapMemoryIntoAddressSpaceGOOGLE) will run,
    //    using the mmap() method of GoldfishAddressSpaceBlock to obtain
    //    a pointer in guest userspace corresponding to the host pointer.
    VkResult on_vkMapMemoryIntoAddressSpaceGOOGLE_pre(
        void*,
        VkResult,
        VkDevice,
        VkDeviceMemory memory,
        uint64_t* pAddress) {

        AutoLock lock(mLock);

        auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        auto& memInfo = it->second;
        memInfo.goldfishAddressSpaceBlock.reset(
            new GoldfishAddressSpaceBlock);
        auto& block = *(memInfo.goldfishAddressSpaceBlock);

        block.allocate(
            mGoldfishAddressSpaceBlockProvider.get(),
            memInfo.mappedSize);

        *pAddress = block.physAddr();

        return VK_SUCCESS;
    }

    VkResult on_vkMapMemoryIntoAddressSpaceGOOGLE(
        void*,
        VkResult input_result,
        VkDevice,
        VkDeviceMemory memory,
        uint64_t* pAddress) {

        if (input_result != VK_SUCCESS) {
            return input_result;
        }

        // Now pAddress points to the gpu addr from host.
        AutoLock lock(mLock);

        auto it = info_VkDeviceMemory.find(memory);
        if (it == info_VkDeviceMemory.end()) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        auto& memInfo = it->second;
        auto& block = *(memInfo.goldfishAddressSpaceBlock);

        uint64_t gpuAddr = *pAddress;

        void* userPtr = block.mmap(gpuAddr);

        *pAddress = (uint64_t)(uintptr_t)userPtr;

        return input_result;
    }

private:
    mutable Lock mLock;
    std::unique_ptr<EmulatorFeatureInfo> mFeatureInfo;
    std::unique_ptr<GoldfishAddressSpaceBlockProvider> mGoldfishAddressSpaceBlockProvider;
};
ResourceTracker::ResourceTracker() : mImpl(new ResourceTracker::Impl()) { }
ResourceTracker::~ResourceTracker() { }
VulkanHandleMapping* ResourceTracker::createMapping() {
    return &mImpl->createMapping;
}
VulkanHandleMapping* ResourceTracker::unwrapMapping() {
    return &mImpl->unwrapMapping;
}
VulkanHandleMapping* ResourceTracker::destroyMapping() {
    return &mImpl->destroyMapping;
}
VulkanHandleMapping* ResourceTracker::defaultMapping() {
    return &mImpl->defaultMapping;
}
static ResourceTracker* sTracker = nullptr;
// static
ResourceTracker* ResourceTracker::get() {
    if (!sTracker) {
        // To be initialized once on vulkan device open.
        sTracker = new ResourceTracker;
    }
    return sTracker;
}

#define HANDLE_REGISTER_IMPL(type) \
    void ResourceTracker::register_##type(type obj) { \
        mImpl->register_##type(obj); \
    } \
    void ResourceTracker::unregister_##type(type obj) { \
        mImpl->unregister_##type(obj); \
    } \

GOLDFISH_VK_LIST_HANDLE_TYPES(HANDLE_REGISTER_IMPL)

void ResourceTracker::setDeviceInfo(
    VkDevice device,
    VkPhysicalDevice physdev,
    VkPhysicalDeviceProperties props,
    VkPhysicalDeviceMemoryProperties memProps) {
    mImpl->setDeviceInfo(device, physdev, props, memProps);
}

bool ResourceTracker::isMemoryTypeHostVisible(
    VkDevice device, uint32_t typeIndex) const {
    return mImpl->isMemoryTypeHostVisible(device, typeIndex);
}

uint8_t* ResourceTracker::getMappedPointer(VkDeviceMemory memory) {
    return mImpl->getMappedPointer(memory);
}

VkDeviceSize ResourceTracker::getMappedSize(VkDeviceMemory memory) {
    return mImpl->getMappedSize(memory);
}

VkDeviceSize ResourceTracker::getNonCoherentExtendedSize(VkDevice device, VkDeviceSize basicSize) const {
    return mImpl->getNonCoherentExtendedSize(device, basicSize);
}

bool ResourceTracker::isValidMemoryRange(const VkMappedMemoryRange& range) const {
    return mImpl->isValidMemoryRange(range);
}

void ResourceTracker::setupFeatures(const EmulatorFeatureInfo* features) {
    mImpl->setupFeatures(features);
}

bool ResourceTracker::usingDirectMapping() const {
    return mImpl->usingDirectMapping();
}

void ResourceTracker::deviceMemoryTransform_tohost(
    VkDeviceMemory* memory, uint32_t memoryCount,
    VkDeviceSize* offset, uint32_t offsetCount,
    VkDeviceSize* size, uint32_t sizeCount,
    uint32_t* typeIndex, uint32_t typeIndexCount,
    uint32_t* typeBits, uint32_t typeBitsCount) {
    mImpl->deviceMemoryTransform_tohost(
        memory, memoryCount,
        offset, offsetCount,
        size, sizeCount,
        typeIndex, typeIndexCount,
        typeBits, typeBitsCount);
}

void ResourceTracker::deviceMemoryTransform_fromhost(
    VkDeviceMemory* memory, uint32_t memoryCount,
    VkDeviceSize* offset, uint32_t offsetCount,
    VkDeviceSize* size, uint32_t sizeCount,
    uint32_t* typeIndex, uint32_t typeIndexCount,
    uint32_t* typeBits, uint32_t typeBitsCount) {
    mImpl->deviceMemoryTransform_fromhost(
        memory, memoryCount,
        offset, offsetCount,
        size, sizeCount,
        typeIndex, typeIndexCount,
        typeBits, typeBitsCount);
}

VkResult ResourceTracker::on_vkEnumerateInstanceVersion(
    void* context,
    VkResult input_result,
    uint32_t* apiVersion) {
    return mImpl->on_vkEnumerateInstanceVersion(context, input_result, apiVersion);
}

VkResult ResourceTracker::on_vkEnumerateDeviceExtensionProperties(
    void* context,
    VkResult input_result,
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    return mImpl->on_vkEnumerateDeviceExtensionProperties(
        context, input_result, physicalDevice, pLayerName, pPropertyCount, pProperties);
}

void ResourceTracker::on_vkGetPhysicalDeviceProperties2(
    void* context,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties) {
    mImpl->on_vkGetPhysicalDeviceProperties2(context, physicalDevice, pProperties);
}

void ResourceTracker::on_vkGetPhysicalDeviceMemoryProperties(
    void* context,
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    mImpl->on_vkGetPhysicalDeviceMemoryProperties(
        context, physicalDevice, pMemoryProperties);
}

VkResult ResourceTracker::on_vkCreateDevice(
    void* context,
    VkResult input_result,
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {
    return mImpl->on_vkCreateDevice(
        context, input_result, physicalDevice, pCreateInfo, pAllocator, pDevice);
}

VkResult ResourceTracker::on_vkAllocateMemory(
    void* context,
    VkResult input_result,
    VkDevice device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory* pMemory) {
    return mImpl->on_vkAllocateMemory(
        context, input_result, device, pAllocateInfo, pAllocator, pMemory);
}

VkResult ResourceTracker::on_vkMapMemory(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkDeviceMemory memory,
    VkDeviceSize offset,
    VkDeviceSize size,
    VkMemoryMapFlags flags,
    void** ppData) {
    return mImpl->on_vkMapMemory(
        context, input_result, device, memory, offset, size, flags, ppData);
}

void ResourceTracker::on_vkUnmapMemory(
    void* context,
    VkDevice device,
    VkDeviceMemory memory) {
    mImpl->on_vkUnmapMemory(context, device, memory);
}

void ResourceTracker::unwrap_VkNativeBufferANDROID(
    const VkImageCreateInfo* pCreateInfo,
    VkImageCreateInfo* local_pCreateInfo) {
    mImpl->unwrap_VkNativeBufferANDROID(pCreateInfo, local_pCreateInfo);
}

void ResourceTracker::unwrap_vkAcquireImageANDROID_nativeFenceFd(int fd, int* fd_out) {
    mImpl->unwrap_vkAcquireImageANDROID_nativeFenceFd(fd, fd_out);
}

VkResult ResourceTracker::on_vkMapMemoryIntoAddressSpaceGOOGLE_pre(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkDeviceMemory memory,
    uint64_t* pAddress) {
    return mImpl->on_vkMapMemoryIntoAddressSpaceGOOGLE_pre(
        context, input_result, device, memory, pAddress);
}

VkResult ResourceTracker::on_vkMapMemoryIntoAddressSpaceGOOGLE(
    void* context,
    VkResult input_result,
    VkDevice device,
    VkDeviceMemory memory,
    uint64_t* pAddress) {
    return mImpl->on_vkMapMemoryIntoAddressSpaceGOOGLE(
        context, input_result, device, memory, pAddress);
}

} // namespace goldfish_vk
