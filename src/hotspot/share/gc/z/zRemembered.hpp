/*
 * Copyright (c) 2021, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZREMEMBERED_HPP
#define SHARE_GC_Z_ZREMEMBERED_HPP

#include "gc/z/zAddress.hpp"
#include "utilities/bitMap.hpp"

template <typename T> class GrowableArrayView;
class OopClosure;
class ZForwarding;
class ZForwardingTable;
class ZMark;
class ZPage;
class ZPageAllocator;
class ZPageTable;
struct ZRememberedSetContaining;

/**
 * 年轻代会持有记忆器
 * 用法
 * - 如果一个二级指针的地址是老年代, 则该地址所属页表的记忆集会记住这个地址
 */
class ZRemembered {
  friend class ZRememberedScanMarkFollowTask;
  friend class ZRemsetTableIterator;

private:
  ZPageTable* const             _page_table;
  const ZForwardingTable* const _old_forwarding_table;
  ZPageAllocator* const         _page_allocator;

  // Optimization aid for faster old pages iteration
  struct FoundOld {
    CHeapBitMap   _allocated_bitmap_0;
    CHeapBitMap   _allocated_bitmap_1;
    BitMap* const _bitmaps[2];
    int           _current;

    FoundOld();

    /**
     * 在0/1之间切换
     */
    void flip();
    void clear_previous();

    void register_page(ZPage* page);

    BitMap* current_bitmap();
    BitMap* previous_bitmap();
  } _found_old;

  // Old pages iteration optimization aid
  void flip_found_old_sets();
  void clear_found_old_previous_set();

  /**
   * 遍历数组中的元组, 对其中的对象地址进行必要的转移或者重映射; 如果元组中的字段属于对象, 则对字段地址执行function
   */
  template <typename Function>
  void oops_do_forwarded_via_containing(GrowableArrayView<ZRememberedSetContaining>* array, Function function) const;

  bool should_scan_page(ZPage* page) const;

  bool scan_page_and_clear_remset(ZPage* page) const;

 /**
  * - 约束条件:
  * * 仅在YGC阶段被调用
  * * context一定是ZRememberedScanForwardingContext
  * - 执行流程:
  * * 如果能给转发器加上原子锁:
  * ** 将转发器的状态流转为拒绝, 如果此时已经完成转移则清空已经记住的字段地址
  * ** 清空context中的_containing_array
  * ** 从页表记忆集的previous存储器中提取出字段地址和对象地址, 存入_containing_array当中
  * ** 释放掉原子锁
  * ** 遍历_containing_array中的元组, 对其中的对象地址进行必要的转移或者重映射; 如果元组中的字段地址属于该对象, 将字段地址值染色为color_remset_good, 如果地址属于年轻代则做一次标记并让二级指针被所属页表记忆集的current存储器记住
  * * 否则:
  * ** 如果此时已经完成转移, 遍历已经记住的字段地址, 将字段地址值染色为color_remset_good, 如果地址属于年轻代则做一次标记并让二级指针被所属页表记忆集的current存储器记住, 完成后将记住的字段地址清空
  * ** 如果转发器的_relocated_remembered_fields_publish_young_seqnum等于最新的年轻代年龄, 则拒绝掉老年代发布出来的字段地址, 否则将字段地址标记为接受
  * - 只要有任一地址属于年轻代, 则返回true
  */
  bool scan_forwarding(ZForwarding* forwarding, void* context) const;

public:
  ZRemembered(ZPageTable* page_table,
              const ZForwardingTable* old_forwarding_table,
              ZPageAllocator* page_allocator);

  /**
   * 让二级指针被地址所属页表的current存储器记住
   */
  void remember(volatile zpointer* p) const;

  // Scan all remembered sets and follow
  // ?? TODO ??, 然后执行mark_follow
  void scan_and_follow(ZMark* mark);

  // Save the current remembered sets,
  // and switch over to empty remembered sets.
  // 在mark_start阶段被翻转, 对换previous和current
  void flip();

  /**
   * 将地址值染色为color_remset_good, 如果地址属于年轻代则做一次标记
   * 如果地址非空且属于年轻代, 则让二级指针被所属页表记忆集的current存储器记住, 并返回true
   * 否则返回false
   * @return true - 指针对应的地址非空且属于年轻代
   */
  bool scan_field(volatile zpointer* p) const;

  // Verification
  bool is_remembered(volatile zpointer* p) const;

  // Register pages with the remembered set
  void register_found_old(ZPage* page);
};

#endif // SHARE_GC_Z_ZREMEMBERED_HPP
