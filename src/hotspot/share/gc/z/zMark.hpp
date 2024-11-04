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

#ifndef SHARE_GC_Z_ZMARK_HPP
#define SHARE_GC_Z_ZMARK_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zMarkStack.hpp"
#include "gc/z/zMarkStackAllocator.hpp"
#include "gc/z/zMarkStackEntry.hpp"
#include "gc/z/zMarkTerminate.hpp"
#include "oops/oopsHierarchy.hpp"
#include "utilities/globalDefinitions.hpp"

class Thread;
class ZGeneration;
class ZMarkContext;
class ZPageTable;
class ZWorkers;

class ZMark {
  friend class ZMarkTask;

public:
  static const bool Resurrect     = true;
  static const bool DontResurrect = false;

  static const bool GCThread      = true;
  static const bool AnyThread     = false;

  static const bool Follow        = true;
  static const bool DontFollow    = false;

  static const bool Strong        = false;
  static const bool Finalizable   = true;

private:
  ZGeneration* const  _generation;
  ZPageTable* const   _page_table;
  ZMarkStackAllocator _allocator;
  ZMarkStripeSet      _stripes;
  ZMarkTerminate      _terminate;
  volatile size_t     _work_nproactiveflush;
  volatile size_t     _work_nterminateflush;
  size_t              _nproactiveflush;
  size_t              _nterminateflush;
  size_t              _ntrycomplete;
  size_t              _ncontinue;
  uint                _nworkers;

  /**
   * MIN2(round_down_power_of_2(nworkers), ZMarkStripesMax)
   */
  size_t calculate_nstripes(uint nworkers) const;

  /**
   * @return 是对象数组时返回true
   */
  bool is_array(zaddress addr) const;

  /**
   * 编码为partial_array后推入标记栈中
   */
  void push_partial_array(zpointer* addr, size_t length, bool finalizable);

  /**
   * 对数组元素执行mark_barrier
   */
  void follow_array_elements_small(zpointer* addr, size_t length, bool finalizable);

  /**
   * 对于长数组, 将它切割成长度为512的若干个段, 头部的段当作一个普通短数组处理, 其余的段推入标记栈中
   */
  void follow_array_elements_large(zpointer* addr, size_t length, bool finalizable);

  /**
   * 仅用于对象数组, 基元数组不需要标记元素
   * 如果数组长度小于512则立即执行mark_barrier
   * 否则切割为最大长度为512的多个段, 首个段的元素执行mark_barrier, 余下的段按照partial_array推入标记栈中
   */
  void follow_array_elements(zpointer* addr, size_t length, bool finalizable);

  /**
   * 取到地址和长度以后执行follow_array_elements
   */
  void follow_partial_array(ZMarkStackEntry entry, bool finalizable);

  /**
   * 使用ZMarkBarrierFollowOopClosure对元素执行标记; 如果是full-gc或者老年代正处于标记阶段, 也会标记类对象
   * @see ZMarkBarrierFollowOopClosure
   */
  void follow_array_object(objArrayOop obj, bool finalizable);

  /**
   * 使用ZMarkBarrierFollowOopClosure标记类和字段值里的对象
   * @see ZMarkBarrierFollowOopClosure
   */
  void follow_object(oop obj, bool finalizable);

  /**
   * 标记entry中取出的地址, 并将对象里的字段值对象推入标记栈中
   */
  void mark_and_follow(ZMarkContext* context, ZMarkStackEntry entry);

  bool rebalance_work(ZMarkContext* context);

  /**
   * 执行出栈和标记, 直到栈被清空
   */
  bool drain(ZMarkContext* context);

  /**
   * 遍历当前标记任务的_stripes, 将标记栈转移到工作位置上
   */
  bool try_steal_local(ZMarkContext* context);
  bool try_steal_global(ZMarkContext* context);

  /**
   * 将标记任务转移到标记栈中 ?? TODO 看看stripe有什么作用 ??
   */
  bool try_steal(ZMarkContext* context);
  bool flush();
  bool try_proactive_flush();
  bool try_terminate(ZMarkContext* context);
  void leave();
  bool try_end();

  ZWorkers* workers() const;

  /**
   * 清空当前标记栈中的任务, 然后将stripe中的任务转移到标记栈中, 循环执行直到任务清空
   * Returning true means marking finished successfully after marking as far as it could.
   * Returning false means that marking finished unsuccessfully due to abort or resizing.
   * @param partial 如果为true, 则任务转移完成后立即返回, 不会执行后续的终止流程
   * @return true代表成功完成所有标记, false代表因为终止或调整工作线程数量而导致失败
   */
  bool follow_work(bool partial);

  void verify_all_stacks_empty() const;
  void verify_worker_stacks_empty() const;

public:
  ZMark(ZGeneration* generation, ZPageTable* page_table);

  bool is_initialized() const;

  /**
   * @tparam follow 可见地址标记的时候follow都是true
   */
  template <bool resurrect, bool gc_thread, bool follow, bool finalizable>
  void mark_object(zaddress addr);

  /**
   * 1. 将计数器清零
   * 2. 设置工作线程数
   * 3. 根据工作线程数计算条纹数nstripes
   * 4. 更新统计值
   */
  void start();

  /**
   *
   * 扫描strong+weak所有的oop-storage中的对象, classloader及其class module 常量等对象, 函数调用栈中的对象, 对扫描到的gcroot对象执行ZMarkYoungOopClosure
   * 此处的标记会将指针颜色调整为ZPointerLoadGoodMask | ZPointerMarkedYoung | ZPointerRememberedMask, 并将对象推入到标记栈中
   */
  void mark_young_roots();
  void mark_old_roots();
  void mark_follow();

  /**
   * 如果标记任务执行完毕, 更新统计值并返回true
   */
  bool end();
  void free();

  /**
   * 对当前线程调用flush_and_free(Thread*)
   */
  void flush_and_free();

  /**
   * 将线程独享的标记栈转移到全局标记器条纹_stripes中.
   * 如果当前线程是java线程, 还会?? TODO ??. 启用诊断参数时才会进入到该分支, 先不管
   */
  bool flush_and_free(Thread* thread);

  // Following work
  void prepare_work();
  void finish_work();
  void resize_workers(uint nworkers);

  /**
   * 按照partial=false执行follow_work
   */
  void follow_work_complete();

  /**
   * 按照partial=true执行follow_work
   */
  bool follow_work_partial();

  bool try_terminate_flush();
};

#endif // SHARE_GC_Z_ZMARK_HPP
