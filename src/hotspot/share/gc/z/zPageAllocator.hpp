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

#ifndef SHARE_GC_Z_ZPAGEALLOCATOR_HPP
#define SHARE_GC_Z_ZPAGEALLOCATOR_HPP

#include "gc/z/zAllocationFlags.hpp"
#include "gc/z/zArray.hpp"
#include "gc/z/zList.hpp"
#include "gc/z/zLock.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zPageCache.hpp"
#include "gc/z/zPageType.hpp"
#include "gc/z/zPhysicalMemory.hpp"
#include "gc/z/zSafeDelete.hpp"
#include "gc/z/zVirtualMemory.hpp"

class ThreadClosure;
class ZGeneration;
class ZPageAllocation;
class ZPageAllocator;
class ZPageAllocatorStats;
class ZWorkers;
class ZUncommitter;
class ZUnmapper;

class ZSafePageRecycle {
private:
  ZPageAllocator*        _page_allocator;
  ZActivatedArray<ZPage> _unsafe_to_recycle;

public:
  ZSafePageRecycle(ZPageAllocator* page_allocator);

  void activate();
  void deactivate();

  ZPage* register_and_clone_if_activated(ZPage* page);
};

class ZPageAllocator {
  friend class VMStructs;
  friend class ZUnmapper;
  friend class ZUncommitter;

private:
  mutable ZLock              _lock;
  ZPageCache                 _cache;
  ZVirtualMemoryManager      _virtual;
  ZPhysicalMemoryManager     _physical;
  const size_t               _min_capacity;
  const size_t               _initial_capacity;
  const size_t               _max_capacity;
  volatile size_t            _current_max_capacity;

  /**
   * @brief 当前的堆容量. ?? 似乎不等于堆数组的总尺寸 ??
   */
  volatile size_t            _capacity;

  /**
   * @brief 内存交还期间有效, 代表被交还的字节数
   */
  volatile size_t            _claimed;

  /**
   * @brief 已分配的字节数
   */
  volatile size_t            _used;

  /**
   * @brief 页表提交/晋升后更新分代的分配字节数
   */
  size_t                     _used_generations[2];
  struct {
    size_t                   _used_high;
    size_t                   _used_low;
  } _collection_stats[2];

  /**
   * @brief 暂停的分配任务, 可以理解为正在等待内存, gc结束后被唤醒
   */
  ZList<ZPageAllocation>     _stalled;
  ZUnmapper*                 _unmapper;
  ZUncommitter*              _uncommitter;
  mutable ZSafeDelete<ZPage> _safe_destroy;
  mutable ZSafePageRecycle   _safe_recycle;
  bool                       _initialized;

  size_t increase_capacity(size_t size);
  void decrease_capacity(size_t size, bool set_max_capacity);

  void increase_used(size_t size);
  void decrease_used(size_t size);

  void increase_used_generation(ZGenerationId id, size_t size);
  void decrease_used_generation(ZGenerationId id, size_t size);

  bool commit_page(ZPage* page);
  void uncommit_page(ZPage* page);

  void map_page(const ZPage* page) const;
  void unmap_page(const ZPage* page) const;

  void destroy_page(ZPage* page);

  /**
   * @brief 检查当前的最大剩余容量是否充足
   */
  bool is_alloc_allowed(size_t size) const;

  /**
   * 首先检查是否到了堆容量上限
   * 这个阶段会尝试从页表分配缓存中取出分配好的页表, 放到当前分配器中, 并调整堆容量(_capacity), 但不会发生内存分配
   */
  bool alloc_page_common_inner(ZPageType type, size_t size, ZList<ZPage>* pages);

  /**
   * 调用alloc_page_common_inner成功后, 修改已分配字节数(_used)
   */
  bool alloc_page_common(ZPageAllocation* allocation);

  /**
   * minor-gc的入口点. TODO 看看返回值代表什么
   */
  bool alloc_page_stall(ZPageAllocation* allocation);

  /**
   * 这个阶段首先尝试从页表分配缓存中取出分配好的页表
   * 失败时挂起当前分配任务并启动minor-gc
   */
  bool alloc_page_or_stall(ZPageAllocation* allocation);
  bool should_defragment(const ZPage* page) const;
  bool is_alloc_satisfied(ZPageAllocation* allocation) const;
  ZPage* alloc_page_create(ZPageAllocation* allocation);

  /**
   * @brief 这个阶段执行真正的内存分配动作, ?? 将当前分配器中缓存的页表合并为一个真实页表 ??
   */
  ZPage* alloc_page_finalize(ZPageAllocation* allocation);
  void free_pages_alloc_failed(ZPageAllocation* allocation);

  void satisfy_stalled();

  /**
   * 销毁页表并释放内存
   * 被使用过的页在延时有效期内不会被回收
   */
  size_t uncommit(uint64_t* timeout);

  /**
   * 将失败结果通知到各个等待内存的页表分配任务
   */
  void notify_out_of_memory();

  /**
   * minor-gc结束, 但是仍然有等待内存的页表分配任务时调用到该函数
   * 可能会触发major-gc
   */
  void restart_gc() const;

public:
  ZPageAllocator(size_t min_capacity,
                 size_t initial_capacity,
                 size_t soft_max_capacity,
                 size_t max_capacity);

  bool is_initialized() const;

  bool prime_cache(ZWorkers* workers, size_t size);

  size_t initial_capacity() const;
  size_t min_capacity() const;
  size_t max_capacity() const;
  size_t soft_max_capacity() const;
  size_t capacity() const;
  size_t used() const;
  size_t used_generation(ZGenerationId id) const;
  size_t unused() const;

  void promote_used(size_t size);

  ZPageAllocatorStats stats(ZGeneration* generation) const;

  void reset_statistics(ZGenerationId id);

  ZPage* alloc_page(ZPageType type, size_t size, ZAllocationFlags flags, ZPageAge age);
  void recycle_page(ZPage* page);
  void safe_destroy_page(ZPage* page);
  void free_page(ZPage* page);
  void free_pages(const ZArray<ZPage*>* pages);

  void enable_safe_destroy() const;
  void disable_safe_destroy() const;

  void enable_safe_recycle() const;
  void disable_safe_recycle() const;

  bool is_alloc_stalling() const;
  bool is_alloc_stalling_for_old() const;
  void handle_alloc_stalling_for_young();
  void handle_alloc_stalling_for_old(bool cleared_soft_refs);

  void threads_do(ThreadClosure* tc) const;
};

class ZPageAllocatorStats {
private:
  size_t _min_capacity;
  size_t _max_capacity;
  size_t _soft_max_capacity;
  size_t _capacity;
  size_t _used;
  size_t _used_high;
  size_t _used_low;
  size_t _used_generation;
  size_t _freed;
  size_t _promoted;
  size_t _compacted;
  size_t _allocation_stalls;

public:
  ZPageAllocatorStats(size_t min_capacity,
                      size_t max_capacity,
                      size_t soft_max_capacity,
                      size_t capacity,
                      size_t used,
                      size_t used_high,
                      size_t used_low,
                      size_t used_generation,
                      size_t freed,
                      size_t promoted,
                      size_t compacted,
                      size_t allocation_stalls);

  size_t min_capacity() const;
  size_t max_capacity() const;
  size_t soft_max_capacity() const;
  size_t capacity() const;
  size_t used() const;
  size_t used_high() const;
  size_t used_low() const;
  size_t used_generation() const;
  size_t freed() const;
  size_t promoted() const;
  size_t compacted() const;
  size_t allocation_stalls() const;
};

#endif // SHARE_GC_Z_ZPAGEALLOCATOR_HPP
