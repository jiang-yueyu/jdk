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

#ifndef SHARE_GC_Z_ZRELOCATE_HPP
#define SHARE_GC_Z_ZRELOCATE_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zRelocationSet.hpp"

class ZForwarding;
class ZGeneration;
class ZWorkers;

typedef size_t ZForwardingCursor;

class ZRelocateQueue {
private:
  ZConditionLock       _lock;
  ZArray<ZForwarding*> _queue;
  uint                 _nworkers;
  uint                 _nsynchronized;
  bool                 _synchronize;
  volatile bool        _is_active;
  volatile int         _needs_attention;

  bool needs_attention() const;
  void inc_needs_attention();
  void dec_needs_attention();

  /**
   * 移除掉已经完成的转发表
   * @return 发生过移除操作
   */
  bool prune();

  /**
   * 首先移除掉已经完成的转发表, 返回第一个能加锁的转发表
   */
  ZForwarding* prune_and_claim();

public:
  ZRelocateQueue();

  void activate(uint nworkers);
  void deactivate();
  bool is_active() const;

  void join(uint nworkers);
  void resize_workers(uint nworkers);
  void leave();

  /**
   * 等待转发表上的转发任务被gc线程处理完
   */
  void add_and_wait(ZForwarding* forwarding);

  /**
   * 在concurrent_relocate阶段被调用
   * 清理掉已完成的转发表, 返回第一个能加锁的转发表
   */
  ZForwarding* synchronize_poll();
  void synchronize_thread();
  void desynchronize_thread();

  void clear();

  void synchronize();
  void desynchronize();
};

class ZRelocate {
  friend class ZRelocateTask;

private:
  ZGeneration* const _generation;
  ZRelocateQueue     _queue;

  ZWorkers* workers() const;
  void work(ZRelocationSetParallelIterator* iter);

public:
  ZRelocate(ZGeneration* generation);

  void start();

  static void add_remset(volatile zpointer* p);

  /**
   * 如果from_age是old或者需要提前晋升, 返回old, 否则返回年龄+1
   */
  static ZPageAge compute_to_age(ZPageAge from_age);

  /**
   * 1. 首先在转发表上执行一次地址查找, 如果能查到值代表对象已经被转移, 直接返回转移后的地址
   * 2. 如果转发表仍然有效, 且目标页表能够分配出相同尺寸的对象, 则直接把对象数据拷贝到新的对象地址上, 然后把新地址插入到转发表
   * 此时插入失败代表其他线程抢先完成了转移任务, 此时回滚内存分配, 并返回其他线程的转移结果
   * 3. 走到这一步代表转发表已经失效, 或者目标页表内存不足, 此时会插入到任务队列中, concurrent_relocate阶段会处理这部分任务
   * @return 转移后的地址
   */
  zaddress relocate_object(ZForwarding* forwarding, zaddress_unsafe from_addr);

  /**
   * 仅执行查表
   */
  zaddress forward_object(ZForwarding* forwarding, zaddress_unsafe from_addr);

  /**
   * 首先执行转移队列里的转发表
   * 然后尝试对转移集里的转发表加原子锁, 加锁成功后执行转发任务
   * 转移的执行过程如下
   * 1. 遍历页表上的对象, 对每个对象执行转移
   * - 首先尝试在目标页表上分配对象并转移
   * - 如果失败则分配一个页表当作目标页表
   * - 再失败时执行原地转移, 并将当前页表当作目标页表
   * 2. 修改被回收的字节数
   * 3. 对于原地转移的情况:
   * - 如果不能晋升到老年代, 将页表的livemap的年龄置零
   * - 释放掉执行任务的线程标记
   * 4. 如果转移的起始页表已经是老年代 ?? TODO 涉及到remembered_set, 深坑 ??
   * 5. 对于原地转移的情况:
   * - 等待转发表的引用计数归零
   * - 如果是老年代到老年代的转移 ?? TODO 涉及到remembered_set, 深坑 ??
   * - 获取到转移目标年龄的转移目标页表, 将它作为分配器的共享页表
   *    否则:
   * - 等待转发表的引用计数归零
   * - ?? TODO 涉及到remembered_set, 深坑 ??
   * - 释放页表
   * 6. 设置转发表上的完成标记
   * 7. ?? TODO 深坑, 放到后面看 ??
   */
  void relocate(ZRelocationSet* relocation_set);

  /**
   * 对入参的页表执行年龄晋升的任务
   * 如果需要执行分代晋升, 会遍历页表内的对象的每个对象字段, 更新指针颜色到ZPointerStoreGoodMask
   * 执行年龄晋升. 如果需要执行分代晋升, 则是对页表生成一份拷贝, 在拷贝上执行年龄晋升
   * 如果需要执行分代晋升, 会置换page_table的页表对象, 置换成页表拷贝, 并更新相关的计数和统计值
   * 最后遍历分代晋升的页表, 追加到转移集的已转移页表里面. ?? TODO 这一步涉及到remembered_set ??
   */
  void flip_age_pages(const ZArray<ZPage*>* pages);

  void synchronize();
  void desynchronize();

  ZRelocateQueue* queue();

  bool is_queue_active() const;
};

#endif // SHARE_GC_Z_ZRELOCATE_HPP
