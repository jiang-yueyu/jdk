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

#include "precompiled.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/z/zAbort.inline.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zAllocator.inline.hpp"
#include "gc/z/zBarrier.inline.hpp"
#include "gc/z/zCollectedHeap.hpp"
#include "gc/z/zForwarding.inline.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zIndexDistributor.inline.hpp"
#include "gc/z/zIterator.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zRelocate.hpp"
#include "gc/z/zRelocationSet.inline.hpp"
#include "gc/z/zRootsIterator.hpp"
#include "gc/z/zStackWatermark.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zTask.hpp"
#include "gc/z/zUncoloredRoot.inline.hpp"
#include "gc/z/zVerify.hpp"
#include "gc/z/zWorkers.hpp"
#include "prims/jvmtiTagMap.hpp"
#include "runtime/atomic.hpp"
#include "utilities/debug.hpp"

static const ZStatCriticalPhase ZCriticalPhaseRelocationStall("Relocation Stall");
static const ZStatSubPhase ZSubPhaseConcurrentRelocateRememberedSetFlipPromotedYoung("Concurrent Relocate Remset FP", ZGenerationId::young);

ZRelocateQueue::ZRelocateQueue()
  : _lock(),
    _queue(),
    _nworkers(0),
    _nsynchronized(0),
    _synchronize(false),
    _is_active(false),
    _needs_attention(0) {}

bool ZRelocateQueue::needs_attention() const {
  return Atomic::load(&_needs_attention) != 0;
}

void ZRelocateQueue::inc_needs_attention() {
  const int needs_attention = Atomic::add(&_needs_attention, 1);
  assert(needs_attention == 1 || needs_attention == 2, "Invalid state");
}

void ZRelocateQueue::dec_needs_attention() {
  const int needs_attention = Atomic::sub(&_needs_attention, 1);
  assert(needs_attention == 0 || needs_attention == 1, "Invalid state");
}

void ZRelocateQueue::activate(uint nworkers) {
  _is_active = true;
  join(nworkers);
}

void ZRelocateQueue::deactivate() {
  Atomic::store(&_is_active, false);
  clear();
}

bool ZRelocateQueue::is_active() const {
  return Atomic::load(&_is_active);
}

void ZRelocateQueue::join(uint nworkers) {
  assert(nworkers != 0, "Must request at least one worker");
  assert(_nworkers == 0, "Invalid state");
  assert(_nsynchronized == 0, "Invalid state");

  log_debug(gc, reloc)("Joining workers: %u", nworkers);

  _nworkers = nworkers;
}

void ZRelocateQueue::resize_workers(uint nworkers) {
  assert(nworkers != 0, "Must request at least one worker");
  assert(_nworkers == 0, "Invalid state");
  assert(_nsynchronized == 0, "Invalid state");

  log_debug(gc, reloc)("Resize workers: %u", nworkers);

  ZLocker<ZConditionLock> locker(&_lock);
  _nworkers = nworkers;
}

void ZRelocateQueue::leave() {
  ZLocker<ZConditionLock> locker(&_lock);
  _nworkers--;

  assert(_nsynchronized <= _nworkers, "_nsynchronized: %u _nworkers: %u", _nsynchronized, _nworkers);

  log_debug(gc, reloc)("Leaving workers: left: %u _synchronize: %d _nsynchronized: %u", _nworkers, _synchronize, _nsynchronized);

  // Prune done forwardings
  const bool forwardings_done = prune();

  // Check if all workers synchronized
  const bool last_synchronized = _synchronize && _nworkers == _nsynchronized;

  if (forwardings_done || last_synchronized) {
    _lock.notify_all();
  }
}

void ZRelocateQueue::add_and_wait(ZForwarding* forwarding) {
  ZStatTimer timer(ZCriticalPhaseRelocationStall);
  ZLocker<ZConditionLock> locker(&_lock);

  if (forwarding->is_done()) {
    return;
  }

  _queue.append(forwarding);
  if (_queue.length() == 1) {
    // Queue became non-empty
    inc_needs_attention();
    _lock.notify_all();
  }

  while (!forwarding->is_done()) {
    _lock.wait();
  }
}

bool ZRelocateQueue::prune() {
  if (_queue.is_empty()) {
    return false;
  }

  bool done = false;

  for (int i = 0; i < _queue.length();) {
    const ZForwarding* const forwarding = _queue.at(i);
    if (forwarding->is_done()) {
      done = true;

      _queue.delete_at(i);
    } else {
      i++;
    }
  }

  if (_queue.is_empty()) {
    dec_needs_attention();
  }

  return done;
}

ZForwarding* ZRelocateQueue::prune_and_claim() {
  if (prune()) {
    _lock.notify_all();
  }

  for (int i = 0; i < _queue.length(); i++) {
    ZForwarding* const forwarding = _queue.at(i);
    if (forwarding->claim()) {
      return forwarding;
    }
  }

  return nullptr;
}

class ZRelocateQueueSynchronizeThread {
private:
  ZRelocateQueue* const _queue;

public:
  ZRelocateQueueSynchronizeThread(ZRelocateQueue* queue)
    : _queue(queue) {
    _queue->synchronize_thread();
  }

  ~ZRelocateQueueSynchronizeThread() {
    _queue->desynchronize_thread();
  }
};

void ZRelocateQueue::synchronize_thread() {
  _nsynchronized++;

  log_debug(gc, reloc)("Synchronize worker _nsynchronized %u", _nsynchronized);

  assert(_nsynchronized <= _nworkers, "_nsynchronized: %u _nworkers: %u", _nsynchronized, _nworkers);
  if (_nsynchronized == _nworkers) {
    // All workers synchronized
    _lock.notify_all();
  }
}

void ZRelocateQueue::desynchronize_thread() {
  _nsynchronized--;

  log_debug(gc, reloc)("Desynchronize worker _nsynchronized %u", _nsynchronized);

  assert(_nsynchronized < _nworkers, "_nsynchronized: %u _nworkers: %u", _nsynchronized, _nworkers);
}

ZForwarding* ZRelocateQueue::synchronize_poll() {
  // Fast path avoids locking
  if (!needs_attention()) {
    return nullptr;
  }

  // Slow path to get the next forwarding and/or synchronize
  ZLocker<ZConditionLock> locker(&_lock);

  {
    ZForwarding* const forwarding = prune_and_claim();
    if (forwarding != nullptr) {
      // Don't become synchronized while there are elements in the queue
      return forwarding;
    }
  }

  if (!_synchronize) {
    return nullptr;
  }

  ZRelocateQueueSynchronizeThread rqst(this);

  do {
    _lock.wait();

    ZForwarding* const forwarding = prune_and_claim();
    if (forwarding != nullptr) {
      return forwarding;
    }
  } while (_synchronize);

  return nullptr;
}

void ZRelocateQueue::clear() {
  assert(_nworkers == 0, "Invalid state");

  if (_queue.is_empty()) {
    return;
  }

  ZArrayIterator<ZForwarding*> iter(&_queue);
  for (ZForwarding* forwarding; iter.next(&forwarding);) {
    assert(forwarding->is_done(), "All should be done");
  }

  assert(false, "Clear was not empty");

  _queue.clear();
  dec_needs_attention();
}

void ZRelocateQueue::synchronize() {
  ZLocker<ZConditionLock> locker(&_lock);
  _synchronize = true;

  inc_needs_attention();

  log_debug(gc, reloc)("Synchronize all workers 1 _nworkers: %u _nsynchronized: %u", _nworkers, _nsynchronized);

  while (_nworkers != _nsynchronized) {
    _lock.wait();
    log_debug(gc, reloc)("Synchronize all workers 2 _nworkers: %u _nsynchronized: %u", _nworkers, _nsynchronized);
  }
}

void ZRelocateQueue::desynchronize() {
  ZLocker<ZConditionLock> locker(&_lock);
  _synchronize = false;

  log_debug(gc, reloc)("Desynchronize all workers _nworkers: %u _nsynchronized: %u", _nworkers, _nsynchronized);

  assert(_nsynchronized <= _nworkers, "_nsynchronized: %u _nworkers: %u", _nsynchronized, _nworkers);

  dec_needs_attention();

  _lock.notify_all();
}

ZRelocate::ZRelocate(ZGeneration* generation)
  : _generation(generation),
    _queue() {}

ZWorkers* ZRelocate::workers() const {
  return _generation->workers();
}

void ZRelocate::start() {
  _queue.activate(workers()->active_workers());
}

void ZRelocate::add_remset(volatile zpointer* p) {
  ZGeneration::young()->remember(p);
}

/**
 * 在目标页表上分配一个相同尺寸的空对象, 把对象拷贝过去, 然后把新对象的地址插入到转发表中
 * 插入失败代表其他线程已经执行了移动任务, 此时把内存分配动作回滚掉
 * @return null代表目标页表无法分配内存
 */
static zaddress relocate_object_inner(ZForwarding* forwarding, zaddress from_addr, ZForwardingCursor* cursor) {
  assert(ZHeap::heap()->is_object_live(from_addr), "Should be live");

  // Allocate object
  const size_t size = ZUtils::object_size(from_addr);

  ZAllocatorForRelocation* allocator = ZAllocator::relocation(forwarding->to_age());

  const zaddress to_addr = allocator->alloc_object(size);

  if (is_null(to_addr)) {
    // Allocation failed
    return zaddress::null;
  }

  // Copy object
  ZUtils::object_copy_disjoint(from_addr, to_addr, size);

  // Insert forwarding
  const zaddress to_addr_final = forwarding->insert(from_addr, to_addr, cursor);

  if (to_addr_final != to_addr) {
    // Already relocated, try undo allocation
    allocator->undo_alloc_object(to_addr, size);
  }

  return to_addr_final;
}

zaddress ZRelocate::relocate_object(ZForwarding* forwarding, zaddress_unsafe from_addr) {
  ZForwardingCursor cursor;

  // Lookup forwarding
  zaddress to_addr = forwarding->find(from_addr, &cursor);
  if (!is_null(to_addr)) {
    // Already relocated
    return to_addr;
  }

  // Relocate object
  if (forwarding->retain_page(&_queue)) {
    assert(_generation->is_phase_relocate(), "Must be");
    to_addr = relocate_object_inner(forwarding, safe(from_addr), &cursor);
    forwarding->release_page();

    if (!is_null(to_addr)) {
      // Success
      return to_addr;
    }

    // Failed to relocate object. Signal and wait for a worker thread to
    // complete relocation of this page, and then forward the object.
    _queue.add_and_wait(forwarding);
  }

  // Forward object
  return forward_object(forwarding, from_addr);
}

zaddress ZRelocate::forward_object(ZForwarding* forwarding, zaddress_unsafe from_addr) {
  const zaddress to_addr = forwarding->find(from_addr);
  assert(!is_null(to_addr), "Should be forwarded: " PTR_FORMAT, untype(from_addr));
  return to_addr;
}

static ZPage* alloc_page(ZAllocatorForRelocation* allocator, ZPageType type, size_t size) {
  if (ZStressRelocateInPlace) {
    // Simulate failure to allocate a new page. This will
    // cause the page being relocated to be relocated in-place.
    return nullptr;
  }

  ZAllocationFlags flags;
  flags.set_non_blocking();
  flags.set_gc_relocation();

  return allocator->alloc_page_for_relocation(type, size, flags);
}

/**
 * 更新分代的统计值, 当页表为空时将其释放
 */
static void retire_target_page(ZGeneration* generation, ZPage* page) {
  if (generation->is_young() && page->is_old()) {
    generation->increase_promoted(page->used());
  } else {
    generation->increase_compacted(page->used());
  }

  // Free target page if it is empty. We can end up with an empty target
  // page if we allocated a new target page, and then lost the race to
  // relocate the remaining objects, leaving the target page empty when
  // relocation completed.
  if (page->used() == 0) {
    ZHeap::heap()->free_page(page);
  }
}

class ZRelocateSmallAllocator {
private:
  ZGeneration* const _generation;
  volatile size_t    _in_place_count;

public:
  ZRelocateSmallAllocator(ZGeneration* generation)
    : _generation(generation),
      _in_place_count(0) {}

  ZPage* alloc_and_retire_target_page(ZForwarding* forwarding, ZPage* target) {
    ZAllocatorForRelocation* const allocator = ZAllocator::relocation(forwarding->to_age());
    ZPage* const page = alloc_page(allocator, forwarding->type(), forwarding->size());
    if (page == nullptr) {
      Atomic::inc(&_in_place_count);
    }

    if (target != nullptr) {
      // Retire the old target page
      retire_target_page(_generation, target);
    }

    return page;
  }

  void share_target_page(ZPage* page) {
    // Does nothing
  }

  void free_target_page(ZPage* page) {
    if (page != nullptr) {
      retire_target_page(_generation, page);
    }
  }

  zaddress alloc_object(ZPage* page, size_t size) const {
    return (page != nullptr) ? page->alloc_object(size) : zaddress::null;
  }

  void undo_alloc_object(ZPage* page, zaddress addr, size_t size) const {
    page->undo_alloc_object(addr, size);
  }

  size_t in_place_count() const {
    return _in_place_count;
  }
};

class ZRelocateMediumAllocator {
private:
  ZGeneration* const _generation;
  ZConditionLock     _lock;
  ZPage*             _shared[ZAllocator::_relocation_allocators];
  bool               _in_place;
  volatile size_t    _in_place_count;

public:
  ZRelocateMediumAllocator(ZGeneration* generation)
    : _generation(generation),
      _lock(),
      _shared(),
      _in_place(false),
      _in_place_count(0) {}

  ~ZRelocateMediumAllocator() {
    for (uint i = 0; i < ZAllocator::_relocation_allocators; ++i) {
      if (_shared[i] != nullptr) {
        retire_target_page(_generation, _shared[i]);
      }
    }
  }

  ZPage* shared(ZPageAge age) {
    return _shared[static_cast<uint>(age) - 1];
  }

  void set_shared(ZPageAge age, ZPage* page) {
    _shared[static_cast<uint>(age) - 1] = page;
  }

  ZPage* alloc_and_retire_target_page(ZForwarding* forwarding, ZPage* target) {
    ZLocker<ZConditionLock> locker(&_lock);

    // Wait for any ongoing in-place relocation to complete
    while (_in_place) {
      _lock.wait();
    }

    // Allocate a new page only if the shared page is the same as the
    // current target page. The shared page will be different from the
    // current target page if another thread shared a page, or allocated
    // a new page.
    const ZPageAge to_age = forwarding->to_age();
    if (shared(to_age) == target) {
      ZAllocatorForRelocation* const allocator = ZAllocator::relocation(forwarding->to_age());
      ZPage* const to_page = alloc_page(allocator, forwarding->type(), forwarding->size());
      set_shared(to_age, to_page);
      if (to_page == nullptr) {
        Atomic::inc(&_in_place_count);
        _in_place = true;
      }

      // This thread is responsible for retiring the shared target page
      if (target != nullptr) {
        retire_target_page(_generation, target);
      }
    }

    return shared(to_age);
  }

  void share_target_page(ZPage* page) {
    const ZPageAge age = page->age();

    ZLocker<ZConditionLock> locker(&_lock);
    assert(_in_place, "Invalid state");
    assert(shared(age) == nullptr, "Invalid state");
    assert(page != nullptr, "Invalid page");

    set_shared(age, page);
    _in_place = false;

    _lock.notify_all();
  }

  void free_target_page(ZPage* page) {
    // Does nothing
  }

  zaddress alloc_object(ZPage* page, size_t size) const {
    return (page != nullptr) ? page->alloc_object_atomic(size) : zaddress::null;
  }

  void undo_alloc_object(ZPage* page, zaddress addr, size_t size) const {
    page->undo_alloc_object_atomic(addr, size);
  }

  size_t in_place_count() const {
    return _in_place_count;
  }
};

template <typename Allocator>
class ZRelocateWork : public StackObj {
private:
  Allocator* const   _allocator;
  ZForwarding*       _forwarding;
  ZPage*             _target[ZAllocator::_relocation_allocators];
  ZGeneration* const _generation;

  /**
   * 统计值, 被其他线程转移走且存在晋升的对象尺寸
   */
  size_t             _other_promoted;

  /**
   * 统计值, 被其他线程转移走且不存在晋升的对象尺寸
   */
  size_t             _other_compacted;

  /**
   * 获取指定年龄的目标转移页表, 不是所有的调用场景都能接受null值
   */
  ZPage* target(ZPageAge age) {
    return _target[static_cast<uint>(age) - 1];
  }

  /**
   * 设置该年龄的目标转移页表
   */
  void set_target(ZPageAge age, ZPage* page) {
    _target[static_cast<uint>(age) - 1] = page;
  }

  size_t object_alignment() const {
    return (size_t)1 << _forwarding->object_alignment_shift();
  }

  void increase_other_forwarded(size_t unaligned_object_size) {
    const size_t aligned_size = align_up(unaligned_object_size, object_alignment());
    if (_forwarding->is_promotion()) {
      _other_promoted += aligned_size;
    } else {
      _other_compacted += aligned_size;
    }
  }

  /**
   * 如果对象地址已经被转移, 则返回转移后的地址
   * 如果无法在目标页表上分配出相同尺寸的对象, 返回null
   * 将对象复制到目标地址上, 然后把目标地址插入到转发表中
   * 插入失败代表已经被其他线程转移走了, 回滚内存分配
   * @return 转移后的地址
   */
  zaddress try_relocate_object_inner(zaddress from_addr) {
    ZForwardingCursor cursor;

    const size_t size = ZUtils::object_size(from_addr);

    /**
     * 这一步的返回可以是null
     */
    ZPage* const to_page = target(_forwarding->to_age());

    // Lookup forwarding
    {
      const zaddress to_addr = _forwarding->find(from_addr, &cursor);
      if (!is_null(to_addr)) {
        // Already relocated
        increase_other_forwarded(size);
        return to_addr;
      }
    }

    /**
     * 如果to_page是null则直接返回null
     */
    // Allocate object
    const zaddress allocated_addr = _allocator->alloc_object(to_page, size);
    if (is_null(allocated_addr)) {
      // Allocation failed
      return zaddress::null;
    }

    // Copy object. Use conjoint copying if we are relocating
    // in-place and the new object overlaps with the old object.
    if (_forwarding->in_place_relocation() && allocated_addr + size > from_addr) {
      ZUtils::object_copy_conjoint(from_addr, allocated_addr, size);
    } else {
      ZUtils::object_copy_disjoint(from_addr, allocated_addr, size);
    }

    // Insert forwarding
    const zaddress to_addr = _forwarding->insert(from_addr, allocated_addr, &cursor);
    if (to_addr != allocated_addr) {
      // Already relocated, undo allocation
      _allocator->undo_alloc_object(to_page, to_addr, size);
      increase_other_forwarded(size);
    }

    return to_addr;
  }

  /**
   * 遍历对象字段
   * 如果处于YGC的标记阶段, 将字段偏移量存入到转发表中, 否则立即让偏移量被remembered_set记住
   */
  void update_remset_old_to_old(zaddress from_addr, zaddress to_addr) const {
    // Old-to-old relocation - move existing remset bits

    // If this is called for an in-place relocated page, then this code has the
    // responsibility to clear the old remset bits. Extra care is needed because:
    //
    // 1) The to-object copy can overlap with the from-object copy
    // 2) Remset bits of old objects need to be cleared
    //
    // A watermark is used to keep track of how far the old remset bits have been removed.

    const bool in_place = _forwarding->in_place_relocation();
    ZPage* const from_page = _forwarding->page();
    const uintptr_t from_local_offset = from_page->local_offset(from_addr);

    // Note: even with in-place relocation, the to_page could be another page
    ZPage* const to_page = ZHeap::heap()->page(to_addr);

    // Uses _relaxed version to handle that in-place relocation resets _top
    assert(ZHeap::heap()->is_in_page_relaxed(from_page, from_addr), "Must be");
    assert(to_page->is_in(to_addr), "Must be");


    // Read the size from the to-object, since the from-object
    // could have been overwritten during in-place relocation.
    const size_t size = ZUtils::object_size(to_addr);

    // If a young generation collection started while the old generation
    // relocated  objects, the remember set bits were flipped from "current"
    // to "previous".
    //
    // We need to select the correct remembered sets bitmap to ensure that the
    // old remset bits are found.
    //
    // Note that if the young generation marking (remset scanning) finishes
    // before the old generation relocation has relocated this page, then the
    // young generation will visit this page's previous remembered set bits and
    // moved them over to the current bitmap.
    //
    // If the young generation runs multiple cycles while the old generation is
    // relocating, then the first cycle will have consume the the old remset,
    // bits and moved associated objects to a new old page. The old relocation
    // could find either the the two bitmaps. So, either it will find the original
    // remset bits for the page, or it will find an empty bitmap for the page. It
    // doesn't matter for correctness, because the young generation marking has
    // already taken care of the bits.

    const bool active_remset_is_current = ZGeneration::old()->active_remset_is_current();

    // When in-place relocation is done and the old remset bits are located in
    // the bitmap that is going to be used for the new remset bits, then we
    // need to clear the old bits before the new bits are inserted.
    const bool iterate_current_remset = active_remset_is_current && !in_place;

    /**
     * 从current或previous存储器中拿到对象字段的地址, 让它被相应的存储器记住
     * 因为向remembered_set存入的是二级指针, 实际上就是对象地址+字段偏移量, 所以拿到的值是一致的
     */
    BitMap::Iterator iter = iterate_current_remset
        ? from_page->remset_iterator_limited_current(from_local_offset, size)
        : from_page->remset_iterator_limited_previous(from_local_offset, size);

    for (BitMap::idx_t field_bit : iter) {
      const uintptr_t field_local_offset = ZRememberedSet::to_offset(field_bit);

      // Add remset entry in the to-page
      const uintptr_t offset = field_local_offset - from_local_offset;
      const zaddress to_field = to_addr + offset;
      log_trace(gc, reloc)("Remember: from: " PTR_FORMAT " to: " PTR_FORMAT " current: %d marking: %d page: " PTR_FORMAT " remset: " PTR_FORMAT,
          untype(from_page->start() + field_local_offset), untype(to_field), active_remset_is_current, ZGeneration::young()->is_phase_mark(), p2i(to_page), p2i(to_page->remset_current()));

      volatile zpointer* const p = (volatile zpointer*)to_field;

      if (ZGeneration::young()->is_phase_mark()) {
        // Young generation remembered set scanning needs to know about this
        // field. It will take responsibility to add a new remember set entry if needed.
        _forwarding->relocated_remembered_fields_register(p);
      } else {
        to_page->remember(p);
        if (in_place) {
          assert(to_page->is_remembered(p), "p: " PTR_FORMAT, p2i(p));
        }
      }
    }
  }

  static bool add_remset_if_young(volatile zpointer* p, zaddress addr) {
    if (ZHeap::heap()->is_young(addr)) {
      ZRelocate::add_remset(p);
      return true;
    }

    return false;
  }

  static void update_remset_promoted_filter_and_remap_per_field(volatile zpointer* p) {
    const zpointer ptr = Atomic::load(p);

    assert(ZPointer::is_old_load_good(ptr), "Should be at least old load good: " PTR_FORMAT, untype(ptr));

    if (ZPointer::is_store_good(ptr)) {
      // Already has a remset entry
      return;
    }

    if (ZPointer::is_load_good(ptr)) {
      if (!is_null_any(ptr)) {
        const zaddress addr = ZPointer::uncolor(ptr);
        add_remset_if_young(p, addr);
      }
      // No need to remap it is already load good
      return;
    }

    if (is_null_any(ptr)) {
      // Eagerly remap to skip adding a remset entry just to get deferred remapping
      ZBarrier::remap_young_relocated(p, ptr);
      return;
    }

    const zaddress_unsafe addr_unsafe = ZPointer::uncolor_unsafe(ptr);
    ZForwarding* const forwarding = ZGeneration::young()->forwarding(addr_unsafe);

    if (forwarding == nullptr) {
      // Object isn't being relocated
      const zaddress addr = safe(addr_unsafe);
      if (!add_remset_if_young(p, addr)) {
        // Not young - eagerly remap to skip adding a remset entry just to get deferred remapping
        ZBarrier::remap_young_relocated(p, ptr);
      }
      return;
    }

    const zaddress addr = forwarding->find(addr_unsafe);

    if (!is_null(addr)) {
      // Object has already been relocated
      if (!add_remset_if_young(p, addr)) {
        // Not young - eagerly remap to skip adding a remset entry just to get deferred remapping
        ZBarrier::remap_young_relocated(p, ptr);
      }
      return;
    }

    // Object has not been relocated yet
    // Don't want to eagerly relocate objects, so just add a remset
    ZRelocate::add_remset(p);
    return;
  }

  void update_remset_promoted(zaddress to_addr) const {
    ZIterator::basic_oop_iterate(to_oop(to_addr), update_remset_promoted_filter_and_remap_per_field);
  }

  /**
   * 如果目标年龄不是老年代, 直接返回
   * 如果是老年代到老年代的转移 ?? TODO ??
   * 否则 ?? TODO ??
   * ?? TODO remset水很深, 后面慢慢看 ??
   */
  void update_remset_for_fields(zaddress from_addr, zaddress to_addr) const {
    if (_forwarding->to_age() != ZPageAge::old) {
      // No remembered set in young pages
      return;
    }

    // Need to deal with remset when moving objects to the old generation
    if (_forwarding->from_age() == ZPageAge::old) {
      update_remset_old_to_old(from_addr, to_addr);
      return;
    }

    // Normal promotion
    update_remset_promoted(to_addr);
  }

  /**
   * 在现有的目标页表上执行分配和转移, 不涉及到页表分配
   * 如果对象地址已经被转移, 则返回转移后的地址
   * 如果无法在目标页表上分配出相同尺寸的对象, 返回null
   * 将对象复制到目标地址上, 然后把目标地址插入到转发表中
   * 插入失败代表已经被其他线程转移走了, 回滚内存分配
   * 新地址值为null代表转移失败, 否则 ?? TODO ??
   * ?? TODO remset水很深, 后面慢慢看 ??
   */
  bool try_relocate_object(zaddress from_addr) {
    const zaddress to_addr = try_relocate_object_inner(from_addr);

    if (is_null(to_addr)) {
      return false;
    }

    update_remset_for_fields(from_addr, to_addr);

    return true;
  }

  void start_in_place_relocation_prepare_remset(ZPage* from_page) {
    if (_forwarding->from_age() != ZPageAge::old) {
      // Only old pages have use remset bits
      return;
    }

    if (ZGeneration::old()->active_remset_is_current()) {
      // We want to iterate over and clear the remset bits of the from-space page,
      // and insert current bits in the to-space page. However, with in-place
      // relocation, the from-space and to-space pages are the same. Clearing
      // is destructive, and is difficult to perform before or during the iteration.
      // However, clearing of the current bits has to be done before exposing the
      // to-space objects in the forwarding table.
      //
      // To solve this tricky dependency problem, we start by stashing away the
      // current bits in the previous bits, and clearing the current bits
      // (implemented by swapping the bits). This way, the current bits are
      // cleared before copying the objects (like a normal to-space page),
      // and the previous bits are representing a copy of the current bits
      // of the from-space page, and are used for iteration.
      from_page->swap_remset_bitmaps();
    }
  }

  /**
   * 1. 首先等待当前线程独占转发表
   * 2. 给转发表设置原地转移的初始标记
   * 3. 如果需要晋升则复制一份页表对象作为目标页表, 否则直接将自身当作目标
   * 4. 调整目标页表的年龄
   * 5. 如果需要晋升到老年代, 则执行页表置换 & 分代内存尺寸调整 & 将旧页表注册到待回收列表中
   * 6. 返回新页表
   * @param relocated_watermark 仅用于记录日志
   */
  ZPage* start_in_place_relocation(zoffset relocated_watermark) {
    _forwarding->in_place_relocation_claim_page();
    _forwarding->in_place_relocation_start(relocated_watermark);

    ZPage* const from_page = _forwarding->page();

    const ZPageAge to_age = _forwarding->to_age();
    const bool promotion = _forwarding->is_promotion();

    /**
     * 对于原地转移并晋升的情况, 需要复制一份页表对象出来, 新页表置换掉旧页表, 旧页表对象等待后续回收
     * 这里的复制并不会复制页表的顶部地址, 因为这个情况下目标页表是当作一个空页表对待的
     */
    // Promotions happen through a new cloned page
    ZPage* const to_page = promotion ? from_page->clone_limited() : from_page;
    to_page->reset(to_age, ZPageResetType::InPlaceRelocation);

    // Clear remset bits for all objects that were relocated
    // before this page became an in-place relocated page.
    start_in_place_relocation_prepare_remset(from_page);

    if (promotion) {
      // Register the the promotion
      ZGeneration::young()->in_place_relocate_promote(from_page, to_page);
      ZGeneration::young()->register_in_place_relocate_promoted(from_page);
    }

    return to_page;
  }

  /**
   * 1. 尝试在现有的目标页表上执行转移
   * - 如果对象地址已经被转移, 则返回转移后的地址
   * - 如果无法在目标页表上分配出相同尺寸的对象, 返回null
   * - 将对象复制到目标地址上, 然后把目标地址插入到转发表中
   * - 插入失败代表已经被其他线程转移走了, 回滚内存分配
   * - ?? 对目标地址的使用涉及到remembered_set, 深坑 ??
   * 2. 如果转移不成功则分配一个页表进行转移
   * - 分配一个新页表当作目标页表, 当旧的目标页表为空时将其释放
   * - 这一步的target(to_age)可以返回null
   * - 如果分配成功, 重新执行步骤1
   * 3. 再不成功时执行原地转移
   * - 首先等待当前线程独占转发表
   * - 给转发表设置原地转移的初始标记
   * - 如果需要晋升则复制一份页表对象作为目标页表, 否则直接将自身当作目标
   * - 调整目标页表的年龄
   * - 如果需要晋升到老年代, 则执行页表置换 & 分代内存尺寸调整 & 将旧页表注册到待回收列表中
   * - 返回新页表, 将这个页表当作目标页表
   */
  void relocate_object(oop obj) {
    const zaddress addr = to_zaddress(obj);
    assert(ZHeap::heap()->is_object_live(addr), "Should be live");

    /**
     * 首先在已有的目标页表上执行转移
     */
    while (!try_relocate_object(addr)) {
      // Allocate a new target page, or if that fails, use the page being
      // relocated as the new target, which will cause it to be relocated
      // in-place.
      const ZPageAge to_age = _forwarding->to_age();

      /**
       * 分配一个新页表当作目标页表, 当旧的目标页表为空时将其释放
       * 这一步的target(to_age)可以返回null
       * 如果这一步分配不出页表, 则执行原地转移
       */
      ZPage* to_page = _allocator->alloc_and_retire_target_page(_forwarding, target(to_age));
      set_target(to_age, to_page);
      if (to_page != nullptr) {
        continue;
      }

      // Start in-place relocation to block other threads from accessing
      // the page, or its forwarding table, until it has been released
      // (relocation completed).
      to_page = start_in_place_relocation(ZAddress::offset(addr));
      set_target(to_age, to_page);
    }
  }

public:
  ZRelocateWork(Allocator* allocator, ZGeneration* generation)
    : _allocator(allocator),
      _forwarding(nullptr),
      _target(),
      _generation(generation),
      _other_promoted(0),
      _other_compacted(0) {}

  ~ZRelocateWork() {
    for (uint i = 0; i < ZAllocator::_relocation_allocators; ++i) {
      _allocator->free_target_page(_target[i]);
    }
    // Report statistics on-behalf of non-worker threads
    _generation->increase_promoted(_other_promoted);
    _generation->increase_compacted(_other_compacted);
  }

  bool active_remset_is_current() const {
    // Normal old-to-old relocation can treat the from-page remset as a
    // read-only copy, and then copy over the appropriate remset bits to the
    // cleared to-page's 'current' remset bitmap.
    //
    // In-place relocation is more complicated. Since, the same page is both
    // a from-page and a to-page, we need to remove the old remset bits, and
    // add remset bits that corresponds to the new locations of the relocated
    // objects.
    //
    // Depending on how long ago (in terms of number of young GC's and the
    // current young GC's phase), the page was allocated, the active
    // remembered set will be in either the 'current' or 'previous' bitmap.
    //
    // If the active bits are in the 'previous' bitmap, we know that the
    // 'current' bitmap was cleared at some earlier point in time, and we can
    // simply set new bits in 'current' bitmap, and later when relocation has
    // read all the old remset bits, we could just clear the 'previous' remset
    // bitmap.
    //
    // If, on the other hand, the active bits are in the 'current' bitmap, then
    // that bitmap will be used to both read the old remset bits, and the
    // destination for the remset bits that we copy when an object is copied
    // to it's new location within the page. We need to *carefully* remove all
    // all old remset bits, without clearing out the newly set bits.
    return ZGeneration::old()->active_remset_is_current();
  }

  /**
   * 只有老年代到老年代的转移才需要执行这个函数
   * 如果是原地转移, ?? TODO ??
   */
  void clear_remset_before_reuse(ZPage* page, bool in_place) {
    if (_forwarding->from_age() != ZPageAge::old) {
      // No remset bits
      return;
    }

    if (in_place) {
      // Clear 'previous' remset bits. For in-place relocated pages, the previous
      // remset bits are always used, even when active_remset_is_current().
      page->clear_remset_previous();

      return;
    }

    // Normal relocate

    // Clear active remset bits
    if (active_remset_is_current()) {
      page->clear_remset_current();
    } else {
      page->clear_remset_previous();
    }

    // Verify that inactive remset bits are all cleared
    if (active_remset_is_current()) {
      page->verify_remset_cleared_previous();
    } else {
      page->verify_remset_cleared_current();
    }
  }

  /**
   * 如果此次转移不能够晋升到老年代, 将页表的livemap的年龄置零
   * 释放掉执行任务的线程标记
   * ?? TODO 看看里面的todo项 ??
   */
  void finish_in_place_relocation() {
    // We are done with the from_space copy of the page
    _forwarding->in_place_relocation_finish();
  }

  /**
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
   */
  void do_forwarding(ZForwarding* forwarding) {
    _forwarding = forwarding;

    _forwarding->page()->log_msg(" (relocate page)");

    ZVerify::before_relocation(_forwarding);

    // Relocate objects
    _forwarding->object_iterate([&](oop obj) { relocate_object(obj); });

    ZVerify::after_relocation(_forwarding);

    // Verify
    if (ZVerifyForwarding) {
      _forwarding->verify();
    }

    /**
     * 修改被释放的字节数
     * 转移结束后, 未被转移走的对象就代表需要被回收, 但是这里并不关注对象的字节数
     * 内存回收是以页表为单位进行的, 直接计入整个页表的字节数
     */
    _generation->increase_freed(_forwarding->page()->size());

    // Deal with in-place relocation
    const bool in_place = _forwarding->in_place_relocation();
    if (in_place) {
      finish_in_place_relocation();
    }

    // Old from-space pages need to deal with remset bits
    if (_forwarding->from_age() == ZPageAge::old) {
      _forwarding->relocated_remembered_fields_after_relocate();
    }

    /**
     * ?? TODO 和这一步配对的retain_page在哪里 ??
     */
    // Release relocated page
    _forwarding->release_page();

    if (in_place) {
      // Wait for all other threads to call release_page
      ZPage* const page = _forwarding->detach_page();

      // Ensure that previous remset bits are cleared
      clear_remset_before_reuse(page, true /* in_place */);

      page->log_msg(" (relocate page done in-place)");

      /**
       * 对于中型页表, 这一步的target_page不能是null
       */
      // Different pages when promoting
      ZPage* const target_page = target(_forwarding->to_age());
      _allocator->share_target_page(target_page);

    } else {
      // Wait for all other threads to call release_page
      ZPage* const page = _forwarding->detach_page();

      // Ensure that all remset bits are cleared
      // Note: cleared after detach_page, when we know that
      // the young generation isn't scanning the remset.
      clear_remset_before_reuse(page, false /* in_place */);

      page->log_msg(" (relocate page done normal)");

      // Free page
      ZHeap::heap()->free_page(page);
    }
  }
};

/**
 * 未启用ZBufferStoreBarriers时不执行任何操作
 */
class ZRelocateStoreBufferInstallBasePointersThreadClosure : public ThreadClosure {
public:
  virtual void do_thread(Thread* thread) {
    JavaThread* const jt = JavaThread::cast(thread);
    ZStoreBarrierBuffer* buffer = ZThreadLocalData::store_barrier_buffer(jt);
    buffer->install_base_pointers();
  }
};

/**
 * 未启用ZBufferStoreBarriers时不执行任何操作
 * Installs the object base pointers (object starts), for the fields written
 * in the store buffer. The code that searches for the object start uses that
 * liveness information stored in the pages. That information is lost when the
 * pages have been relocated and then destroyed.
 */
class ZRelocateStoreBufferInstallBasePointersTask : public ZTask {
private:
  ZJavaThreadsIterator _threads_iter;

public:
  ZRelocateStoreBufferInstallBasePointersTask(ZGeneration* generation)
    : ZTask("ZRelocateStoreBufferInstallBasePointersTask"),
      _threads_iter(generation->id_optional()) {}

  virtual void work() {
    ZRelocateStoreBufferInstallBasePointersThreadClosure fix_store_buffer_cl;
    _threads_iter.apply(&fix_store_buffer_cl);
  }
};

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
  */
class ZRelocateTask : public ZRestartableTask {
private:
  ZRelocationSetParallelIterator _iter;
  ZGeneration* const             _generation;
  ZRelocateQueue* const          _queue;
  ZRelocateSmallAllocator        _small_allocator;
  ZRelocateMediumAllocator       _medium_allocator;

public:
  ZRelocateTask(ZRelocationSet* relocation_set, ZRelocateQueue* queue)
    : ZRestartableTask("ZRelocateTask"),
      _iter(relocation_set),
      _generation(relocation_set->generation()),
      _queue(queue),
      _small_allocator(_generation),
      _medium_allocator(_generation) {}

  ~ZRelocateTask() {
    _generation->stat_relocation()->at_relocate_end(_small_allocator.in_place_count(), _medium_allocator.in_place_count());

    // Signal that we're not using the queue anymore. Used mostly for asserts.
    _queue->deactivate();
  }

  virtual void work() {
    ZRelocateWork<ZRelocateSmallAllocator> small(&_small_allocator, _generation);
    ZRelocateWork<ZRelocateMediumAllocator> medium(&_medium_allocator, _generation);

    const auto do_forwarding = [&](ZForwarding* forwarding) {
      ZPage* const page = forwarding->page();
      if (page->is_small()) {
        small.do_forwarding(forwarding);
      } else {
        medium.do_forwarding(forwarding);
      }

      // Absolute last thing done while relocating a page.
      //
      // We don't use the SuspendibleThreadSet when relocating pages.
      // Instead the ZRelocateQueue is used as a pseudo STS joiner/leaver.
      //
      // After the mark_done call a safepointing could be completed and a
      // new GC phase could be entered.
      forwarding->mark_done();
    };

    /**
     * 如果能够加上原子锁, 执行do_forwarding
     */
    const auto claim_and_do_forwarding = [&](ZForwarding* forwarding) {
      if (forwarding->claim()) {
        do_forwarding(forwarding);
      }
    };

    /**
     * 遍历转移集里的转发表, 如果能加上原子锁则执行do_forwarding
     */
    const auto do_forwarding_one_from_iter = [&]() {
      ZForwarding* forwarding;

      if (_iter.next(&forwarding)) {
        claim_and_do_forwarding(forwarding);
        return true;
      }

      return false;
    };

    for (;;) {
      // As long as there are requests in the relocate queue, there are threads
      // waiting in a VM state that does not allow them to be blocked. The
      // worker thread needs to finish relocate these pages, and allow the
      // other threads to continue and proceed to a blocking state. After that,
      // the worker threads are allowed to safepoint synchronize.
      for (ZForwarding* forwarding; (forwarding = _queue->synchronize_poll()) != nullptr;) {
        do_forwarding(forwarding);
      }

      if (!do_forwarding_one_from_iter()) {
        // No more work
        break;
      }

      if (_generation->should_worker_resize()) {
        break;
      }
    }

    _queue->leave();
  }

  virtual void resize_workers(uint nworkers) {
    _queue->resize_workers(nworkers);
  }
};

static void remap_and_maybe_add_remset(volatile zpointer* p) {
  const zpointer ptr = Atomic::load(p);

  if (ZPointer::is_store_good(ptr)) {
    // Already has a remset entry
    return;
  }

  // Remset entries are used for two reasons:
  // 1) Young marking old-to-young pointer roots
  // 2) Deferred remapping of stale old-to-young pointers
  //
  // This load barrier will up-front perform the remapping of (2),
  // and the code below only has to make sure we register up-to-date
  // old-to-young pointers for (1).
  const zaddress addr = ZBarrier::load_barrier_on_oop_field_preloaded(p, ptr);

  if (is_null(addr)) {
    // No need for remset entries for null pointers
    return;
  }

  if (ZHeap::heap()->is_old(addr)) {
    // No need for remset entries for pointers to old gen
    return;
  }

  ZRelocate::add_remset(p);
}

class ZRelocateAddRemsetForFlipPromoted : public ZRestartableTask {
private:
  ZStatTimerYoung                _timer;
  ZArrayParallelIterator<ZPage*> _iter;

public:
  ZRelocateAddRemsetForFlipPromoted(ZArray<ZPage*>* pages)
    : ZRestartableTask("ZRelocateAddRemsetForFlipPromoted"),
      _timer(ZSubPhaseConcurrentRelocateRememberedSetFlipPromotedYoung),
      _iter(pages) {}

  virtual void work() {
    SuspendibleThreadSetJoiner sts_joiner;

    for (ZPage* page; _iter.next(&page);) {
      page->object_iterate([&](oop obj) {
        ZIterator::basic_oop_iterate_safe(obj, remap_and_maybe_add_remset);
      });

      SuspendibleThreadSet::yield();
      if (ZGeneration::young()->should_worker_resize()) {
        return;
      }
    }
  }
};

void ZRelocate::relocate(ZRelocationSet* relocation_set) {
  {
    // 未启用ZBufferStoreBarriers时不执行任何操作
    // Install the store buffer's base pointers before the
    // relocate task destroys the liveness information in
    // the relocated pages.
    ZRelocateStoreBufferInstallBasePointersTask buffer_task(_generation);
    workers()->run(&buffer_task);
  }

  {
    ZRelocateTask relocate_task(relocation_set, &_queue);
    workers()->run(&relocate_task);
  }

  /**
   * ?? TODO 深坑, 放到后面看 ??
   */
  if (relocation_set->generation()->is_young()) {
    ZRelocateAddRemsetForFlipPromoted task(relocation_set->flip_promoted_pages());
    workers()->run(&task);
  }
}

ZPageAge ZRelocate::compute_to_age(ZPageAge from_age) {
  if (from_age == ZPageAge::old) {
    return ZPageAge::old;
  }

  const uint age = static_cast<uint>(from_age);
  if (age >= ZGeneration::young()->tenuring_threshold()) {
    return ZPageAge::old;
  }

  return static_cast<ZPageAge>(age + 1);
}

/**
 * 如果需要执行分代晋升, 会遍历页表内的对象的每个对象字段, 更新指针颜色到ZPointerStoreGoodMask
 * 执行年龄晋升. 如果需要执行分代晋升, 则是对页表生成一份拷贝, 在拷贝上执行年龄晋升
 * 如果需要执行分代晋升, 会置换page_table的页表对象, 置换成页表拷贝, 并更新相关的计数和统计值
 * 最后遍历分代晋升的页表, 追加到转移集的已转移页表里面. ?? TODO 这一步涉及到remembered_set ??
 */
class ZFlipAgePagesTask : public ZTask {
private:
  ZArrayParallelIterator<ZPage*> _iter;

public:
  ZFlipAgePagesTask(const ZArray<ZPage*>* pages)
    : ZTask("ZPromotePagesTask"),
      _iter(pages) {}

  virtual void work() {
    SuspendibleThreadSetJoiner sts_joiner;
    ZArray<ZPage*> promoted_pages;

    for (ZPage* prev_page; _iter.next(&prev_page);) {
      const ZPageAge from_age = prev_page->age();
      const ZPageAge to_age = ZRelocate::compute_to_age(from_age);
      assert(from_age != ZPageAge::old, "invalid age for a young collection");

      // Figure out if this is proper promotion
      const bool promotion = to_age == ZPageAge::old;

      if (promotion) {
        // Before promoting an object (and before relocate start), we must ensure that all
        // contained zpointers are store good. The marking code ensures that for non-null
        // pointers, but null pointers are ignored. This code ensures that even null pointers
        // are made store good, for the promoted objects.
        prev_page->object_iterate([&](oop obj) {
          ZIterator::basic_oop_iterate_safe(obj, ZBarrier::promote_barrier_on_young_oop_field);
        });
      }

      // Logging
      prev_page->log_msg(promotion ? " (flip promoted)" : " (flip survived)");

      // Setup to-space page
      ZPage* const new_page = promotion ? prev_page->clone_limited_promote_flipped() : prev_page;
      new_page->reset(to_age, ZPageResetType::FlipAging);

      if (promotion) {
        ZGeneration::young()->flip_promote(prev_page, new_page);
        // Defer promoted page registration times the lock is taken
        promoted_pages.push(prev_page);
      }

      SuspendibleThreadSet::yield();
    }

    ZGeneration::young()->register_flip_promoted(promoted_pages);
  }
};

void ZRelocate::flip_age_pages(const ZArray<ZPage*>* pages) {
  ZFlipAgePagesTask flip_age_task(pages);
  workers()->run(&flip_age_task);
}

void ZRelocate::synchronize() {
  _queue.synchronize();
}

void ZRelocate::desynchronize() {
  _queue.desynchronize();
}

ZRelocateQueue* ZRelocate::queue() {
  return &_queue;
}

bool ZRelocate::is_queue_active() const {
  return _queue.is_active();
}
