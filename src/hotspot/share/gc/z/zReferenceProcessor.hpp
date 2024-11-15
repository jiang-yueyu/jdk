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

/**
 * 全局单例
 */
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
   * 保存一个Referenced对象的地址, 对应Reference::discovered字段的链式结构. ?? TODO PerWork似乎可以理解成thread_local ??
   * 在work()函数中被转移到_pending_list的头部
   */
  ZPerWorker<zaddress> _discovered_list;

  /**
   * Reference::discovered字段串成的列表, 在enqueue_references()函数中被转移到全局pending列表的头部
   */
  ZContended<zaddress> _pending_list;
  zaddress             _pending_list_tail;

  /**
   * @return 如果是final引用, 在next非空时返回true, 否则在referent为空时返回true
   */
  bool is_inactive(zaddress reference, oop referent, ReferenceType type) const;

  /**
   * @return 对象属于年轻代 || 所属页表的年龄等于所属分代的最新年龄 || 对象被强引用
   */
  bool is_strongly_live(oop referent) const;

  /**
   * @return type==REF_SOFT && 软引用管理策略认为引用无需淘汰
   */
  bool is_softly_live(zaddress reference, ReferenceType type) const;

  /**
   * 按如下顺序执行判定
   * 1. 引用已经失效(final引用的next非空或者非final引用的referent为空)则返回false
   * 2. 引用对象属于年轻代则返回false
   * 3. 引用目标仍存在强引用时返回false
   * 4. 引用目标被判定为无需清理时返回false
   * 5. 最终返回true
   */
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

  /**
   * 通过discovered字段遍历discovered_list, 对其中的每个reference对象检查是否需要清理, 并将其从列表中移除, 需要清理对象时将reference对象添加到keep列表中
   * 如果keep列表非空, 将其插入到pending_list的头部
   */
  void process_worker_discovered_list(zaddress discovered_list);

  /**
   * 在old-gc第四步被ZReferenceProcessorTask调用 
   * 剥离当前的discovered列表, 将其中需要清理的元素添加到当前的pending列表中, 循环执行直到discovered为空
   */
  void work();
  void collect_statistics();

  /**
   * 和全局pending列表做交换
   */
  zaddress swap_pending_list(zaddress pending_list);

public:
  ZReferenceProcessor(ZWorkers* workers);

  /**
   * @param clear_all_soft_references true时设置为始终清理, 否则设置为lru淘汰
   */
  void set_soft_reference_policy(bool clear_all_soft_references);
  bool uses_clear_all_soft_reference_policy() const;

  void reset_statistics();

  /**
   * 如果判定为无需发现则返回false, 否则处理发现流程并返回true
   * - 需要发现的判定:
   * 1. 引用已经失效(final引用的next非空或者非final引用的referent为空)则返回false
   * 2. 引用对象属于年轻代则返回false
   * 3. 引用目标仍存在强引用时返回false
   * 4. 引用目标被判定为无需清理时返回false
   * 5. 其他情况返回true
   * - 发现流程:
   * 在discovered链表头上插入reference对象, 并将头节点指向reference
   * 如果引用类型是FinalReference, 插入节点前会按照finalizable=true执行mark_barrier
   */
  virtual bool discover_reference(oop reference, ReferenceType type);

  /**
   * 在old-gc第四步被调用, 异步执行work()并等待执行完成
   * 剥离当前的discovered列表, 将其中需要清理的元素添加到当前的pending列表中,
   * 循环执行直到discovered为空
   */
  void process_references();

  /**
   * 如果pending列表为空直接返回, 否则加锁后将当前pending列表转移到全局pending列表的头部
   */
  void enqueue_references();

  void verify_pending_references();
};

#endif // SHARE_GC_Z_ZREFERENCEPROCESSOR_HPP
