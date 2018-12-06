/*
 * Copyright (C) 2018 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef ANDROID_INCLUDE_HARDWARE_GOLDFISH_ADDRESS_SPACE_H
#define ANDROID_INCLUDE_HARDWARE_GOLDFISH_ADDRESS_SPACE_H

#include <inttypes.h>

class GoldfishAddressSpaceBlock;

#ifdef GOLDFISH_OPENGL_BUILD_FOR_HOST
class GoldfishAddressSpaceBlockProvider {};
#else
class GoldfishAddressSpaceBlockProvider {
public:
    GoldfishAddressSpaceBlockProvider();
    ~GoldfishAddressSpaceBlockProvider();

private:
   GoldfishAddressSpaceBlockProvider(const GoldfishAddressSpaceBlockProvider &rhs);
   GoldfishAddressSpaceBlockProvider &operator=(const GoldfishAddressSpaceBlockProvider &rhs);

   bool is_opened();
   int m_fd;

   friend class GoldfishAddressSpaceBlock;
};
#endif

class GoldfishAddressSpaceBlock {
public:
    GoldfishAddressSpaceBlock();
    ~GoldfishAddressSpaceBlock();

    bool allocate(GoldfishAddressSpaceBlockProvider *provider, size_t size);
    uint64_t physAddr() const;
    uint64_t hostAddr() const;
    void *mmap(uint64_t opaque);
    void *guestPtr() const;
    void replace(GoldfishAddressSpaceBlock *x);

private:
    void destroy();
    GoldfishAddressSpaceBlock &operator=(const GoldfishAddressSpaceBlock &);

#ifdef GOLDFISH_OPENGL_BUILD_FOR_HOST
    void     *m_guest_ptr;
#else
    void     *m_mmaped_ptr;
    uint64_t  m_phys_addr;
    uint64_t  m_host_addr;
    uint64_t  m_offset;
    size_t    m_size;
    int       m_fd;
#endif
};

#endif
