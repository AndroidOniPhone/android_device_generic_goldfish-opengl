// Copyright (C) 2018 The Android Open Source Project
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
#include "VulkanStream.h"

#include "android/base/Pool.h"

#include <vector>

#include <cutils/log.h>
#include <inttypes.h>

namespace goldfish_vk {

class VulkanStream::Impl : public android::base::Stream {
public:
    Impl() { }

    ~Impl() { }

    bool valid() { return true; }

    void alloc(void **ptrAddr, size_t bytes) {
        if (!bytes) {
            *ptrAddr = nullptr;
            return;
        }

        *ptrAddr = mPool.alloc(bytes);
    }

    void loadStringInPlace(char **forOutput) {
        size_t len = this->getBe32();

        *forOutput = mPool.allocArray<char>(len + 1);

        memset(*forOutput, 0x0, len + 1);

        if (len > 0)
        {
            this->read(*forOutput, len);
        }
    }

    void loadStringArrayInPlace(char*** forOutput) {
        size_t count = this->getBe32();

        if (!count) {
            *forOutput = nullptr;
            return;
        }

        *forOutput = mPool.allocArray<char *>(count);

        char **stringsForOutput = *forOutput;

        for (size_t i = 0; i < count; i++) {
            loadStringInPlace(stringsForOutput + i);
        }
    }

    ssize_t write(const void *buffer, size_t size) override {
        return bufferedWrite(buffer, size);
    }

    ssize_t read(void *buffer, size_t size) override {
        commitWrite();
        return readFully(buffer, size);
    }

private:
    size_t oustandingWriteBuffer() const {
        return mWritePos;
    }

    size_t remainingWriteBufferSize() const {
        return mWriteBuffer.size() - mWritePos;
    }

    void commitWrite() {
        if (!valid()) {
            ALOGE("FATAL: Tried to commit write to vulkan pipe with invalid pipe!");
            abort();
        }

        mWritePos = 0;
    }

    ssize_t bufferedWrite(const void *buffer, size_t size) {
        if (size > remainingWriteBufferSize()) {
            mWriteBuffer.resize(1 << (mWritePos + size));
        }
        memcpy(mWriteBuffer.data() + mWritePos, buffer, size);
        mWritePos += size;
        return size;
    }

    ssize_t readFully(void *buffer, size_t size) {
        return size;
    }

    android::base::Pool mPool { 8, 4096, 64 };

    size_t mWritePos = 0;
    std::vector<uint8_t> mWriteBuffer;
};

VulkanStream::VulkanStream() : mImpl(new VulkanStream::Impl()) {}

bool VulkanStream::valid() {
    return mImpl->valid();
}

void VulkanStream::alloc(void** ptrAddr, size_t bytes) {
    mImpl->alloc(ptrAddr, bytes);
}

void VulkanStream::loadStringInPlace(char** forOutput) {
    mImpl->loadStringInPlace(forOutput);
}

void VulkanStream::loadStringArrayInPlace(char*** forOutput) {
    mImpl->loadStringArrayInPlace(forOutput);
}

ssize_t VulkanStream::read(void *buffer, size_t size) {
    return mImpl->read(buffer, size);
}

ssize_t VulkanStream::write(const void *buffer, size_t size) {
    return mImpl->write(buffer, size);
}

} // namespace goldfish_vk