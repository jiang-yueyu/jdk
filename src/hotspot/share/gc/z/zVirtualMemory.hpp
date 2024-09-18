/*
 * Copyright (c) 2015, 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#ifndef SHARE_GC_Z_ZVIRTUALMEMORY_HPP
#define SHARE_GC_Z_ZVIRTUALMEMORY_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zMemory.hpp"

class ZVirtualMemory {
  friend class VMStructs;

private:
  zoffset     _start;
  zoffset_end _end;

public:
  ZVirtualMemory();
  ZVirtualMemory(zoffset start, size_t size);

  bool is_null() const;
  zoffset start() const;
  zoffset_end end() const;
  size_t size() const;

  ZVirtualMemory split(size_t size);
};

/**
 * 虚拟内存
 * 虚拟内存块只会被初始化一次, 程序在启动阶段会尝试选择一个起始地址, 然后分配一整块连续的虚拟内存, 失败时再尝试分配多个零散的地址段
 * 分配出来的虚拟内存地址段相当于占位符, 分配真实内存时会跳过虚拟内存地址段
 * 完成真实内存的分配后, 会通过类似于mmap的系统调用, 把相对零散的真实内存映射到相对集中的虚拟内存上, 程序通过直接访问虚拟内存, 得到读写连续地址段的假象
 * 因为虚拟内存地址段已经被注册为进程级别的内存占位符, 所以地址段的地址在程序内部可以自由使用
 * 在内存分配的逻辑实现上, 虚拟机使用全局互斥锁+空闲列表维护地址段, 小的页表会被分配在低地址上, 大页表会被分配在高地址上
 */
class ZVirtualMemoryManager {
  friend class ZMapperTest;

private:
  static size_t calculate_min_range(size_t size);

  ZMemoryManager _manager;
  size_t         _reserved;
  bool           _initialized;

  // Platform specific implementation
  void pd_initialize_before_reserve();
  void pd_initialize_after_reserve();
  bool pd_reserve(zaddress_unsafe addr, size_t size);
  void pd_unreserve(zaddress_unsafe addr, size_t size);

  bool reserve_contiguous(zoffset start, size_t size);
  bool reserve_contiguous(size_t size);
  size_t reserve_discontiguous(zoffset start, size_t size, size_t min_range);
  size_t reserve_discontiguous(size_t size);
  bool reserve(size_t max_capacity);

  DEBUG_ONLY(size_t force_reserve_discontiguous(size_t size);)

public:
  ZVirtualMemoryManager(size_t max_capacity);

  bool is_initialized() const;

  size_t reserved() const;
  zoffset lowest_available_address() const;

  ZVirtualMemory alloc(size_t size, bool force_low_address);
  void free(const ZVirtualMemory& vmem);
};

#endif // SHARE_GC_Z_ZVIRTUALMEMORY_HPP
