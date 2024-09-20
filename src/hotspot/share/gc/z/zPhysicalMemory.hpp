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

#ifndef SHARE_GC_Z_ZPHYSICALMEMORY_HPP
#define SHARE_GC_Z_ZPHYSICALMEMORY_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zArray.hpp"
#include "gc/z/zMemory.hpp"
#include "memory/allocation.hpp"
#include OS_HEADER(gc/z/zPhysicalMemoryBacking)

class ZPhysicalMemorySegment : public CHeapObj<mtGC> {
private:
  zoffset     _start;
  zoffset_end _end;
  bool        _committed;

public:
  ZPhysicalMemorySegment();
  ZPhysicalMemorySegment(zoffset start, size_t size, bool committed);

  zoffset start() const;
  zoffset_end end() const;
  size_t size() const;

  bool is_committed() const;
  void set_committed(bool committed);
};

class ZPhysicalMemory {
private:
  ZArray<ZPhysicalMemorySegment> _segments;

  void insert_segment(int index, zoffset start, size_t size, bool committed);
  void replace_segment(int index, zoffset start, size_t size, bool committed);
  void remove_segment(int index);

public:
  ZPhysicalMemory();
  ZPhysicalMemory(const ZPhysicalMemorySegment& segment);
  ZPhysicalMemory(const ZPhysicalMemory& pmem);
  const ZPhysicalMemory& operator=(const ZPhysicalMemory& pmem);

  bool is_null() const;
  size_t size() const;

  int nsegments() const;
  const ZPhysicalMemorySegment& segment(int index) const;

  void add_segments(const ZPhysicalMemory& pmem);
  void remove_segments();

  void add_segment(const ZPhysicalMemorySegment& segment);
  bool commit_segment(int index, size_t size);
  bool uncommit_segment(int index, size_t size);

  ZPhysicalMemory split(size_t size);
  ZPhysicalMemory split_committed();
};

/**
 * 真实内存
 * 因为物理内存一般不会直接不会暴露给用户进程, 暂且把此处的内存称为真实内存
 * 在虚拟机访问堆内存的过程中, 程序直接访问的是虚拟内存, 操作系统根据注册好的内存映射将读写操作转发到真实内存上, 所以真实内存可以是零散的小块地址段, 只要总长度足够即可
 * 在windows系统上直接将真实内存分配为若干个2M大小的内存块
 * 程序在启动阶段就已经定义好内存分配的地址, 然后按照提取逻辑地址段-分配内存-映射到虚拟内存的顺序分配可访问的堆内存
 */
class ZPhysicalMemoryManager {
private:
  ZPhysicalMemoryBacking _backing;
  ZMemoryManager         _manager;

  void pretouch_view(zaddress addr, size_t size) const;
  void map_view(zaddress_unsafe addr, const ZPhysicalMemory& pmem) const;
  void unmap_view(zaddress_unsafe addr, size_t size) const;

public:
  ZPhysicalMemoryManager(size_t max_capacity);

  bool is_initialized() const;

  void warn_commit_limits(size_t max_capacity) const;
  void try_enable_uncommit(size_t min_capacity, size_t max_capacity);


  /**
   * 从freelist中提取地址段放到pmem中, 地址段可以是零散的
   */
  void alloc(ZPhysicalMemory& pmem, size_t size);

  /**
   * 将地址段退回到freelist当中
   */
  void free(const ZPhysicalMemory& pmem);

  /**
   * 这一步相当于执行若干次malloc, 只不过起始地址已经指定好了
   */
  bool commit(ZPhysicalMemory& pmem);

  /**
   * 这一步相当于执行若干次free
   */
  bool uncommit(ZPhysicalMemory& pmem);

  /**
   * 对地址段做一次写操作. TODO 注释没看懂, 有空研究下这步的目的是什么
   */
  void pretouch(zoffset offset, size_t size) const;

  /**
   * 将pmem中零散的地址段映射到offset上, 对程序而言offset就是一个连续的可用内存空间
   */
  void map(zoffset offset, const ZPhysicalMemory& pmem) const;

  /**
   * 解除内存映射, 并继续保留虚拟地址占位符
   */
  void unmap(zoffset offset, size_t size) const;
};

#endif // SHARE_GC_Z_ZPHYSICALMEMORY_HPP
