/*
 * Copyright (c) 2015, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZFORWARDING_HPP
#define SHARE_GC_Z_ZFORWARDING_HPP

#include "gc/z/zArray.hpp"
#include "gc/z/zAttachedArray.hpp"
#include "gc/z/zForwardingEntry.hpp"
#include "gc/z/zGenerationId.hpp"
#include "gc/z/zLock.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zPageType.hpp"
#include "gc/z/zVirtualMemory.hpp"

class ObjectClosure;
class ZForwardingAllocator;
class ZPage;
class ZRelocateQueue;

typedef size_t ZForwardingCursor;

class ZForwarding {
  friend class VMStructs;
  friend class ZForwardingTest;

  enum class ZPublishState : int8_t {
    none,      // No publishing done yet
    published, // OC published remset field info, which YC will reject or accept
    reject,    // YC remset scanning accepted OC published remset field info
    accept     // YC remset scanning rejected OC published remset field info
  };

private:
  typedef ZAttachedArray<ZForwarding, ZForwardingEntry> AttachedArray;
  typedef ZArray<volatile zpointer*> PointerArray;

  const ZVirtualMemory   _virtual;
  const size_t           _object_alignment_shift;
  const AttachedArray    _entries;
  ZPage* const           _page;
  ZPageAge               _from_age;
  ZPageAge               _to_age;

  /**
   * 一次性的原子锁, 不会被归零
   * 仅在执行转移集任务时被使用, 代表该转发表被某个执行器线程独占
   */
  volatile bool          _claimed;
  mutable ZConditionLock _ref_lock;

  /**
   * 引用计数, 归零时代表已经失效, 小于0代表正在执行原地转移
   */
  volatile int32_t       _ref_count;
  volatile bool          _done;

  // Relocated remembered set fields support
  // none:      仅在这个阶段会收集二级指针
  // published: 这个阶段代表收集已经结束, 仅在该阶段会执行回调函数, 只能从none阶段流转
  // reject:    A previous YC has already handled the field
  // accept:    A previous YC has determined that there's no concurrency between
  //            OC relocation and YC remembered fields scanning - not possible
  //            since the page has been retained (still being relocated) and
  //            we are in the process of scanning fields
  // ?? TODO ??
  volatile ZPublishState _relocated_remembered_fields_state;

  /**
   * 这个数组在old-gc处于转移阶段, 且young-gc同时处于标记阶段时被使用
   * 当old-gc正在转移对象时, 旧的remembered_set会发生转移, 而young-gc不会等待这一步完成,
   * 这个数组被用于转移带有remembered_set标记的对象
   * none - 初始状态, old和young都没有插入意向
   * published - old-gc已经完成所有的转移任务
   * reject - ?? TODO ??
   * accept - 当young-gc开始时, 转发表上的转发任务已经完成
   */
  //
  // ?? TODO 这个是深坑, 后面慢慢看 ??
  // This requires some synchronization between the OC and YC, and this is
  // mainly done via the _relocated_remembered_fields_state in each ZForwarding.
  // The values corresponds to:
  //
  // none:      Starting state - neither OC nor YC has stated their intentions
  // published: The OC has completed relocating all objects, and published an array
  //            of all to-space fields that should have a remembered set entry.
  // reject:    The OC relocation of the page happened concurrently with the YC
  //            remset scanning. Two situations:
  //            a) The page had not been released yet: The YC eagerly relocated and
  //            scanned the to-space objects with remset entries.
  //            b) The page had been released: The YC accepts the array published in
  //            (published).
  // accept:    The YC found that the forwarding/page had already been relocated when
  //            the YC started.
  //
  // Central to this logic is the ZRemembered::scan_forwarding function, where
  // the YC tries to "retain" the forwarding/page. If it succeeds it means that
  // the OC has not finished (or maybe not even started) the relocation of all objects.
  //
  // When the YC manages to retaining the page it will bring the state from:
  //  none      -> reject - Started collecting remembered set info
  //  published -> reject - Rejected the OC's remembered set info
  //  reject    -> reject - An earlier YC had already handled the remembered set info
  //  accept    ->        - Invalid state - will not happen
  //
  // When the YC fails to retain the page the state transitions are:
  // none      -> x - The page was relocated before the YC started
  // published -> x - The OC completed relocation before YC visited this forwarding.
  //                  The YC will use the remembered set info collected by the OC.
  // reject    -> x - A previous YC has already handled the remembered set info
  // accept    -> x - See above
  //
  // x is:
  //  reject        - if the relocation finished while the current YC was running
  //  accept        - if the relocation finished before the current YC started
  //
  // Note the subtlety that even though the relocation could released the page
  // and made it non-retainable, the relocation code might not have gotten to
  // the point where the page is removed from the page table. It could also be
  // the case that the relocated page became in-place relocated, and we therefore
  // shouldn't be scanning it this YC.
  //
  // The (reject) state is the "dangerous" state, where both OC and YC work on
  // the same forwarding/page somewhat concurrently. While (accept) denotes that
  // that the entire relocation of a page (including freeing/reusing it) was
  // completed before the current YC started.
  //
  // After all remset entries of relocated objects have been scanned, the code
  // proceeds to visit all pages in the page table, to scan all pages not part
  // of the OC relocation set. Pages with virtual addresses that doesn't match
  // any of the once in the OC relocation set will be visited. Pages with
  // virtual address that *do* have a corresponding forwarding entry has two
  // cases:
  //
  // a) The forwarding entry is marked with (reject). This means that the
  //    corresponding page is guaranteed to be one that has been relocated by the
  //    current OC during the active YC. Any remset entry is guaranteed to have
  //    already been scanned by the scan_forwarding code.
  //
  // b) The forwarding entry is marked with (accept). This means that the page was
  //    *not* created by the OC relocation during this YC, which means that the
  //    page must be scanned.
  //
  PointerArray           _relocated_remembered_fields_array;
  uint32_t               _relocated_remembered_fields_publish_young_seqnum;

  // In-place relocation support
  bool                   _in_place;
  zoffset_end            _in_place_top_at_start;

  // Debugging
  volatile Thread*       _in_place_thread;

  ZForwardingEntry* entries() const;
  ZForwardingEntry at(ZForwardingCursor* cursor) const;
  ZForwardingEntry first(uintptr_t from_index, ZForwardingCursor* cursor) const;
  ZForwardingEntry next(ZForwardingCursor* cursor) const;

  uintptr_t index(zoffset from_offset);

  ZForwardingEntry find(uintptr_t from_index, ZForwardingCursor* cursor) const;
  zaddress find(zoffset from_offset, ZForwardingCursor* cursor);

  zoffset insert(uintptr_t from_index, zoffset to_offset, ZForwardingCursor* cursor);
  zaddress insert(zoffset from_offset, zaddress to_addr, ZForwardingCursor* cursor);

  template <typename Function>
  void object_iterate_forwarded_via_livemap(Function function);

  ZForwarding(ZPage* page, ZPageAge to_age, size_t nentries);

public:
  /**
   * round_up_power_of_2(页表内存活对象的数量 * 2)
   */
  static uint32_t nentries(const ZPage* page);
  static ZForwarding* alloc(ZForwardingAllocator* allocator, ZPage* page, ZPageAge to_age);

  ZPageType type() const;
  ZPageAge from_age() const;
  ZPageAge to_age() const;
  zoffset start() const;
  zoffset_end end() const;
  size_t size() const;
  size_t object_alignment_shift() const;

  bool is_promotion() const;

  // Visit from-objects
  template <typename Function>
  void object_iterate(Function function);

  template <typename Function>
  void address_unsafe_iterate_via_table(Function function);

  // Visit to-objects
  template <typename Function>
  void object_iterate_forwarded(Function function);

  template <typename Function>
  void object_iterate_forwarded_via_table(Function function);

  template <typename Function>
  void oops_do_in_forwarded(Function function);

  template <typename Function>
  void oops_do_in_forwarded_via_table(Function function);

  /**
   * 对_claim变量加原子锁
   */
  bool claim();

  // In-place relocation support
  bool in_place_relocation() const;

  /**
   * 等待当前线程独占转发表. 这一步会反转引用计数值并等待它变为-1
   */
  void in_place_relocation_claim_page();

  /**
   * 设置_in_place标记, 记录当前线程 & 起始时刻的页表地址的顶部
   * @param relocated_watermark 仅用于记录日志
   */
  void in_place_relocation_start(zoffset relocated_watermark);

  /**
   * 如果此次转移不能够晋升到老年代, 将页表的livemap的年龄置零
   * 释放掉执行任务的线程标记
   * ?? TODO 看看里面的todo项 ??
   */
  void in_place_relocation_finish();
  bool in_place_relocation_is_below_top_at_start(zoffset addr) const;

  /**
   * 声明对page的引用
   * false代表该转发表已经失效, 否则增加引用计数并返回true
   * 如果此时正在执行原地转移, 会等待转移结束并返回false
   */
  bool retain_page(ZRelocateQueue* queue);

  /**
   * 解除对page的引用
   * 和retain_page配对
   */
  void release_page();

  /**
   * 等待引用计数归零并返回_page
   */
  ZPage* detach_page();

  /**
   * 直接返回_page. 只有引用计数非零的时候才能调用
   */
  ZPage* page();

  /**
   * 标记_done=true, 任务执行完以后被ZRelocateTask调用
   */
  void mark_done();
  bool is_done() const;

  zaddress find(zaddress from_addr, ZForwardingCursor* cursor);
  zaddress find(zaddress_unsafe from_addr, ZForwardingCursor* cursor);
  zaddress find(zaddress_unsafe from_addr);

  zaddress insert(zaddress from_addr, zaddress to_addr, ZForwardingCursor* cursor);

  // Relocated remembered set fields support

  /**
   * 如果_relocated_remembered_fields_state是none, 将指针加入到_relocated_remembered_fields_array当作
   * 否则_relocated_remembered_fields_state必定是reject
   */
  void relocated_remembered_fields_register(volatile zpointer* p);

  /**
   * 仅在老年代页表的转移阶段被调用
   * 更新_relocated_remembered_fields_publish_young_seqnum为当前年轻代年龄
   * 如果处于YGC的标记阶段, 调用relocated_remembered_fields_publish流转状态为publish
   */
  void relocated_remembered_fields_after_relocate();

  /**
   * 仅在YGC的标记阶段被调用
   * 调用前的状态必然为none reject之一, 调用后状态为published
   * none到published的流转无任何操作, reject到published的状态会清空_relocated_remembered_fields_array
   */
  void relocated_remembered_fields_publish();

  /**
   * 仅在YGC的标记阶段被调用
   * 调用前的状态必然为none published reject之一, 调用后状态为reject
   * none到reject的流转无任何操作, published到reject的流转会清空_relocated_remembered_fields_array
   */
  void relocated_remembered_fields_notify_concurrent_scan_of();

  /**
   * @return _relocated_remembered_fields_state == reject
   */
  bool relocated_remembered_fields_is_concurrently_scanned() const;

  template <typename Function>
  void relocated_remembered_fields_apply_to_published(Function function);

  /**
   * 仅用于debug模式下的校验, 正式版本是不会编译进去的
   */
  bool relocated_remembered_fields_published_contains(volatile zpointer* p);

  void verify() const;
};

#endif // SHARE_GC_Z_ZFORWARDING_HPP
