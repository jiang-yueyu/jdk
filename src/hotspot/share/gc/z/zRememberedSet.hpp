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

#ifndef SHARE_GC_Z_ZREMEMBEREDSET_HPP
#define SHARE_GC_Z_ZREMEMBEREDSET_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zBitMap.hpp"

class OopClosure;
class ZPage;

struct ZRememberedSetContaining {
  zaddress_unsafe _field_addr;
  zaddress_unsafe _addr;
};

/**
 * 遍历页表记忆集previous容器的反序迭代器, 从中提取出字段地址和对象地址. 提取出的字段不一定属于提取出的对象 ?? TODO 所以取两个值是为了干嘛 ??
 * Iterates over all (object, oop fields) pairs where the field address has
 * been marked as remembered, and fill in that information in a
 * ZRememberedSetContaining
 *
 * Note that it's not guaranteed that _field_addr belongs to the recorded
 * _addr. The entry could denote a stale remembered set field and _addr could
 * just be the nearest object. The users are responsible for filtering that
 * out.
 */
class ZRememberedSetContainingIterator {
private:
  ZPage* const             _page;

  /**
   * 页表记忆集previous容器的反序迭代器
   */
  ZBitMap::ReverseIterator _remset_iter;

  zaddress_unsafe          _obj;

  /**
   * _obj的区段迭代器, 和_remset_iter规则一致
   */
  ZBitMap::ReverseIterator _obj_remset_iter;

  size_t to_index(zaddress_unsafe addr);
  zaddress_unsafe to_addr(BitMap::idx_t index);

public:
  ZRememberedSetContainingIterator(ZPage* page);

  /**
   * 如果_obj非null, 则从_obj的区段迭代器中提取最后一个字段地址, 成功时向containing返回_obj地址和字段地址, 并返回true, 失败时将_obj置空
   * 继续反序遍历页表记忆集的previous容器, 如果能取到值:
   * * 将这个地址作为字段地址, 然后查找到该字段对于的对象地址, 一并返回给containing
   * * 如果此时对象地址为空, 返回false ?? TODO 对应什么情况 ??
   * * 然后根据对象地址和字段地址调整_remset_iter和_obj_remset_iter两个迭代器
   * * 返回true
   * 否则返回false
   */
  bool next(ZRememberedSetContaining* containing);
};

// Like ZRememberedSetContainingIterator, but with stale remembered set fields
// filtered out.
class ZRememberedSetContainingInLiveIterator {
private:
  ZRememberedSetContainingIterator _iter;
  zaddress                         _addr;
  size_t                           _addr_size;
  size_t                           _count;
  size_t                           _count_skipped;
  ZPage* const                     _page;

public:
  ZRememberedSetContainingInLiveIterator(ZPage* page);

  bool next(ZRememberedSetContaining* containing);

  void print_statistics() const;
};

// Reader Note
// ZRememberedSet包含current和previous两个存储器
// 每个存储器都是一个连续内存块, 用1bit代表一个指针值的状态
// 通过全局变量_current决定内存块代表current还是previous
// 记录的是地址的指针/对象二级指针
// ?? 记录它的目的是什么 ??
//
// The remembered set of a ZPage.
//
// There's one bit per potential object field address within the ZPage.
//
// New entries are added to the "current" active bitmap, while the
// "previous" bitmap is used by the GC to find pointers from old
// gen to young gen.
class ZRememberedSet {
  friend class ZRememberedSetContainingIterator;

public:
  /**
   * 决定存储器是current还是previous
   * 在mark_start阶段被翻转
   */
  static int _current;

  ZMovableBitMap _bitmap[2];

  CHeapBitMap* current();
  const CHeapBitMap* current() const;

  CHeapBitMap* previous();
  const CHeapBitMap* previous() const;

  template <typename Function>
  void iterate_bitmap(Function function, CHeapBitMap* bitmap);

  static uintptr_t to_offset(BitMap::idx_t index);
  static BitMap::idx_t to_index(uintptr_t offset);
  static BitMap::idx_t to_bit_size(size_t size);

public:
  /**
   * 在mark_start阶段被翻转, 对换previous和current
   */
  static void flip();

  ZRememberedSet();

  bool is_initialized() const;
  void initialize(size_t page_size);

  void resize(size_t page_size);

  /**
   * 仅用于assert
   */
  bool at_current(uintptr_t offset) const;
  bool at_previous(uintptr_t offset) const;
  bool set_current(uintptr_t offset);
  void unset_non_par_current(uintptr_t offset);
  void unset_range_non_par_current(uintptr_t offset, size_t size);

  // Visit all set offsets.
  template <typename Function /* void(uintptr_t offset) */>
  void iterate_previous(Function function);

  template <typename Function /* void(uintptr_t offset) */>
  void iterate_current(Function function);

  bool is_cleared_current() const;
  bool is_cleared_previous() const;

  void clear_all();
  void clear_current();
  void clear_previous();

  /**
   * 仅当previous为空时被调用, 将current存储器的bit存入previous, 然后清空current
   * ?? TODO 什么场景下调的 ??
   */
  void swap_remset_bitmaps();

  ZBitMap::ReverseIterator iterator_reverse_previous();
  BitMap::Iterator iterator_limited_current(uintptr_t offset, size_t size);
  BitMap::Iterator iterator_limited_previous(uintptr_t offset, size_t size);
};

#endif // SHARE_GC_Z_ZREMEMBEREDSET_HPP
