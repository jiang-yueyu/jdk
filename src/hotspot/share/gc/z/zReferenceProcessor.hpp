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

#ifndef SHARE_GC_Z_ZREFERENCEPROCESSOR_HPP
#define SHARE_GC_Z_ZREFERENCEPROCESSOR_HPP

#include "gc/shared/referenceDiscoverer.hpp"
#include "gc/z/zAddress.hpp"
#include "gc/z/zValue.hpp"

class ConcurrentGCTimer;
class ReferencePolicy;
class ZWorkers;

class ZReferenceProcessor : public ReferenceDiscoverer {
  friend class ZReferenceProcessorTask;

private:
  static const size_t reference_type_count = REF_PHANTOM + 1;
  typedef size_t Counters[reference_type_count];

  ZWorkers* const      _workers;
  ReferencePolicy*     _soft_reference_policy;
  bool                 _uses_clear_all_soft_reference_policy;
  ZPerWorker<Counters> _encountered_count;
  ZPerWorker<Counters> _discovered_count;
  ZPerWorker<Counters> _enqueued_count;

  /**
   * 保存一个Referenced对象的地址, 对应Reference::discovered字段的链式结构
   */
  ZPerWorker<zaddress> _discovered_list;
  ZContended<zaddress> _pending_list;
  zaddress             _pending_list_tail;

  bool is_inactive(zaddress reference, oop referent, ReferenceType type) const;

  /**
   * @return 对象属于年轻代 || 所属页表的年龄等于所属分代的最新年龄 || 对象被强引用
   */
  bool is_strongly_live(oop referent) const;

  /**
   * @return type==REF_SOFT && lru策略认为引用无需淘汰
   */
  bool is_softly_live(zaddress reference, ReferenceType type) const;

  bool should_discover(zaddress reference, ReferenceType type) const;

  /**
   * 拿到引用的目标对象, 如果已经是null则返回false
   * 否则会执行相应的clean_barrier, 如果地址是年轻代且处于young-mark阶段时做一次标记, 否则:
   * 如果引用类型是soft或者weak, 没有被强引用时返回false
   * 如果引用类型是phantom, 没有被强/final引用时返回false
   * 如果引用类型是final, 仅被final引用时返回false, 且会将引用对象赋值到next字段让它自身成环
   */
  bool try_make_inactive(zaddress reference, ReferenceType type) const;

  /**
   * 在discovered链表头上插入reference对象, 并将头节点指向reference
   * 如果引用类型是FinalReference, 插入节点前会按照finalizable=true执行mark_barrier
   */
  void discover(zaddress reference, ReferenceType type);

  void verify_empty() const;

  void process_worker_discovered_list(zaddress discovered_list);
  void work();
  void collect_statistics();

  zaddress swap_pending_list(zaddress pending_list);

public:
  ZReferenceProcessor(ZWorkers* workers);

  /**
   * @param clear_all_soft_references true时设置为始终清理, 否则设置为lru淘汰
   */
  void set_soft_reference_policy(bool clear_all_soft_references);
  bool uses_clear_all_soft_reference_policy() const;

  void reset_statistics();

  virtual bool discover_reference(oop reference, ReferenceType type);
  void process_references();
  void enqueue_references();

  void verify_pending_references();
};

#endif // SHARE_GC_Z_ZREFERENCEPROCESSOR_HPP
