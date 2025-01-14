/*
 * Copyright (c) 2016, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZMARKSTACK_HPP
#define SHARE_GC_Z_ZMARKSTACK_HPP

#include "gc/z/zGlobals.hpp"
#include "gc/z/zMarkStackEntry.hpp"
#include "utilities/globalDefinitions.hpp"

class ZMarkTerminate;

template <typename T, size_t S>
class ZStack {
private:
  size_t        _top;
  ZStack<T, S>* _next;
  T             _slots[S];

  bool is_full() const;

public:
  ZStack();

  bool is_empty() const;

  /**
   * 栈未满时入栈并返回true, 否则返回false
   */
  bool push(T value);
  bool pop(T& value);

  ZStack<T, S>* next() const;
  ZStack<T, S>** next_addr();
};

template <typename T>
class ZCACHE_ALIGNED ZStackList {
private:
  uintptr_t   _base;
  T* volatile _head;

  T* encode_versioned_pointer(const T* stack, uint32_t version) const;
  void decode_versioned_pointer(const T* vstack, T** stack, uint32_t* version) const;

public:
  explicit ZStackList(uintptr_t base);

  bool is_empty() const;

  void push(T* stack);
  T* pop();

  void clear();
};

using ZMarkStack = ZStack<ZMarkStackEntry, ZMarkStackSlots>;
using ZMarkStackList = ZStackList<ZMarkStack>;
using ZMarkStackMagazine = ZStack<ZMarkStack*, ZMarkStackMagazineSlots>;
using ZMarkStackMagazineList = ZStackList<ZMarkStackMagazine>;

static_assert(sizeof(ZMarkStack) == ZMarkStackSize, "ZMarkStack size mismatch");
static_assert(sizeof(ZMarkStackMagazine) <= ZMarkStackSize, "ZMarkStackMagazine size too large");

/**
 * 标记栈的容器, 分为溢出列表和发布列表两个存储器.
 * 条纹本身是栈的容器, 也会用于分配工作线程内部的标记栈, 一个条纹和一个栈分片一一对应
 * ?? TODO 两个存储器有什么作用 ??
 */
class ZMarkStripe {
private:
  ZCACHE_ALIGNED ZMarkStackList _published;
  ZCACHE_ALIGNED ZMarkStackList _overflowed;

public:
  explicit ZMarkStripe(uintptr_t base = 0);

  /**
   * 两个存储器均为空
   */
  bool is_empty() const;

  /**
   * 把一个标记栈插入到栈列表中, 根据publish决定将标记栈插入到哪个列表, 然后唤醒一个gc工作线程以受理任务
   */
  void publish_stack(ZMarkStack* stack, ZMarkTerminate* terminate, bool publish);

  /**
   * 优先从溢出列表中取出标记栈, 列表空时再从发布列表中取
   */
  ZMarkStack* steal_stack();
};

class ZMarkStripeSet {
private:
  /**
   * 0,1,3,7,15其中之一
   * 
   * 数值变更的时机:
   * 1. try_terminate会将条纹数减半
   */
  size_t      _nstripes_mask;
  ZMarkStripe _stripes[ZMarkStripesMax];

public:
  explicit ZMarkStripeSet(uintptr_t base);

  /**
   * @param nstripes 1,2,4,8,16其中之一
   */
  void set_nstripes(size_t nstripes);
  size_t nstripes() const;

  bool is_empty() const;

  /**
   * 根据地址偏移量计算取到下标
   */
  size_t stripe_id(const ZMarkStripe* stripe) const;
  ZMarkStripe* stripe_at(size_t index);

  /**
   * 根据地址偏移量取到入参stripe的相邻stripe, 地址回环
   */
  ZMarkStripe* stripe_next(ZMarkStripe* stripe);
  ZMarkStripe* stripe_for_worker(uint nworkers, uint worker_id);

  /**
   * 将地址值右移21位后, 和_nstripes_mask做与运算, 返回该下标上的条纹
   */
  ZMarkStripe* stripe_for_addr(uintptr_t addr);
};

class ZMarkStackAllocator;

class ZMarkThreadLocalStacks {
private:
  ZMarkStackMagazine* _magazine;

  /**
   * 分片后的标记栈, 和标记条纹一一对应
   */
  ZMarkStack*         _stacks[ZMarkStripesMax];

  /**
   * 如果_magazine为null则尝试分配一个_magazine出来, 分配失败时返回null
   * 尝试从_manazine中取出一个stack对象, 失败时将magazine原地转换为stack
   */
  ZMarkStack* allocate_stack(ZMarkStackAllocator* allocator);

  /**
   * 如果_magazine为null, 将stack原地转换为_magazine
   * 否则将stack推入_magazine中, 直到_magazine被装满, 此时stack被直接放弃
   * ?? TODO _magazine看上去像一个缓冲池, 看看是不是这么用的 ??
   */
  void free_stack(ZMarkStackAllocator* allocator, ZMarkStack* stack);

  bool push_slow(ZMarkStackAllocator* allocator,
                 ZMarkStripe* stripe,
                 ZMarkStack** stackp,
                 ZMarkTerminate* terminate,
                 ZMarkStackEntry entry,
                 bool publish);

  /**
   * 优先取stackp, 如果*stackp为null, 则从stripe中取标记栈, 从取到的栈上执行出栈
   * 如果栈为空, 则进入到栈的回收流程
   * @param allocator 用于执行stack/magazine的回收
   * @param stripe 工作条纹, 会从栈列表中取出一个栈
   */
  bool pop_slow(ZMarkStackAllocator* allocator,
                ZMarkStripe* stripe,
                ZMarkStack** stackp,
                ZMarkStackEntry& entry);

public:
  ZMarkThreadLocalStacks();

  bool is_empty(const ZMarkStripeSet* stripes) const;

  /**
   * 将栈放置到指定的位置上
   * @param stripes 仅用于定位下标
   * @param stripe 仅用于定位下标
   */
  void install(ZMarkStripeSet* stripes,
               ZMarkStripe* stripe,
               ZMarkStack* stack);

  /**
   * 将指定位置上的标记栈转移出来
   * @param stripes 仅用于定位下标
   * @param stripe 仅用于定位下标
   */
  ZMarkStack* steal(ZMarkStripeSet* stripes,
                    ZMarkStripe* stripe);

  /**
   * 如果stripe中的栈存在且未满, 则推入这个栈中并返回true, 此时publish参数并无意义
   * 否则执行push_slow ?? TODO 做了什么 ??
   */
  bool push(ZMarkStackAllocator* allocator,
            ZMarkStripeSet* stripes,
            ZMarkStripe* stripe,
            ZMarkTerminate* terminate,
            ZMarkStackEntry entry,
            bool publish);

  /**
   * 首先从工作条纹对应的栈中取值, 取不到时再从stripe容器的栈中取值
   * @param stripes 仅用于定位下标
   * @param stripe 工作条纹, 首先用于定位标记栈, 这个栈为空时, 则从条纹自身的栈列表中取出一个栈
   */
  bool pop(ZMarkStackAllocator* allocator,
           ZMarkStripeSet* stripes,
           ZMarkStripe* stripe,
           ZMarkStackEntry& entry);

  /**
   * 将分片的各个栈转移到相应的标记条纹中
   * @return 发生过转移就会返回true
   */
  bool flush(ZMarkStackAllocator* allocator,
             ZMarkStripeSet* stripes,
             ZMarkTerminate* terminate);

  void free(ZMarkStackAllocator* allocator);
};

#endif // SHARE_GC_Z_ZMARKSTACK_HPP
