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

#ifndef SHARE_GC_Z_ZBARRIER_HPP
#define SHARE_GC_Z_ZBARRIER_HPP

#include "gc/z/zAddress.hpp"
#include "memory/allStatic.hpp"
#include "memory/iterator.hpp"

// == Shift based load barrier ==
//
// The load barriers of ZGC check if a loaded value is safe to expose or not, and
// then shifts the pointer to remove metadata bits, such that it points to mapped
// memory.
//
// A pointer is safe to expose if it does not have any load-bad bits set in its
// metadata bits. In the C++ code and non-nmethod generated code, that is checked
// by testing the pointer value against a load-bad mask, checking that no bad bit
// is set, followed by a shift, removing the metadata bits if they were good.
// However, for nmethod code, the test + shift sequence is optimized in such
// a way that the shift both tests if the pointer is exposable or not, and removes
// the metadata bits, with the same instruction. This is a speculative optimization
// that assumes that the loaded pointer is frequently going to be load-good or null
// when checked. Therefore, the nmethod load barriers just apply the shift with the
// current "good" shift (which is patched with nmethod entry barriers for each GC
// phase). If the result of that shift was a raw null value, then the ZF flag is set.
// If the result is a good pointer, then the very last bit that was removed by the
// shift, must have been a 1, which would have set the CF flag. Therefore, the "above"
// branch condition code is used to take a slowpath only iff CF == 0 and ZF == 0.
// CF == 0 implies it was not a good pointer, and ZF == 0 implies the resulting address
// was not a null value. Then we decide that the pointer is bad. This optimization
// is necessary to get satisfactory performance, but does come with a few constraints:
//
// 1) The load barrier can only recognize 4 different good patterns across all GC phases.
//    The reason is that when a load barrier applies the currently good shift, then
//    the value of said shift may differ only by 3, until we risk shifting away more
//    than the low order three zeroes of an address, given a bad pointer, which would
//    yield spurious false positives.
//
// 2) Those bit patterns must have only a single bit set. We achieve that by moving
//    non-relocation work to store barriers.
//
// Another consequence of this speculative optimization, is that when the compiled code
// takes a slow path, it needs to reload the oop, because the shifted oop is now
// broken after being shifted with a different shift to what was used when the oop
// was stored.

typedef bool (*ZBarrierFastPath)(zpointer);
typedef zpointer (*ZBarrierColor)(zaddress, zpointer);

class ZGeneration;

void z_assert_is_barrier_safe();

class ZBarrier : public AllStatic {
  friend class ZContinuation;
  friend class ZStoreBarrierBuffer;
  friend class ZUncoloredRoot;

private:
  static void assert_transition_monotonicity(zpointer ptr, zpointer heal_ptr);

  /**
   * 使用cas将指针的值更新为目标值, 失败时通过fast_path检查最新值是否已经处理过, 决定直接返回或者继续重试
   * @param fast_path 检查指针最新值是否已经处理过
   * @param p 待更新的指针
   * @param ptr 指针的旧值
   * @param heal_ptr 待更新的目标值
   * @param allow_null 预检查, 传入true且尝试将null赋值给有值指针时直接返回
   */
  static void self_heal(ZBarrierFastPath fast_path, volatile zpointer* p, zpointer ptr, zpointer heal_ptr, bool allow_null);

  /**
   * 1. 如果通过fast_path判断出已经通过屏障, 则立即返回去除染色后的原始地址
   * 2. 然后判断指针是否是null或load_good, 不是的话执行转移
   * 3. 对第2步返回的地址执行slow_path, 得到执行屏障逻辑后的地址
   * 4. 如果指针非null, 将第3步得到的地址染色后更新回二级指针上
   * 5. 返回第3步得到的地址
   * @param fast_path 判断一个指针是否已经通过屏障
   * @param slow_path 执行屏障逻辑
   * @param color 将通过屏障的地址值和旧指针的颜色, 染色生成新指针
   * @param p 原始二级指针, 会将更新后的地址值染色成指针, 并赋值给二级指针
   * @param o 二级指针指向的java对象
   * @return 如果根据fast_path判断出已通过屏障, 则返回原始地址; 否则返回执行屏障逻辑后的地址
   */
  template <typename ZBarrierSlowPath>
  static zaddress barrier(ZBarrierFastPath fast_path, ZBarrierSlowPath slow_path, ZBarrierColor color, volatile zpointer* p, zpointer o, bool allow_null = false);

  /**
   * 1. 如果指针是null则返回null
   * 2. 如果指针已经是load_good, 则返回去除染色后的原始地址
   * 3. 尝试执行对象转移, 返回转移后的地址
   */
  static zaddress make_load_good(zpointer ptr);
  static zaddress make_load_good_no_relocate(zpointer ptr);

  /**
   * 1. 如果地址没有对应的转发器, 则立即返回原始地址
   * 2. 在转发器上执行一次地址查找, 如果能查到值代表对象已经被转移, 直接返回转移后的地址
   * 3. 如果转发器仍然有效, 且目标页表能够分配出相同尺寸的对象, 则直接把对象数据拷贝到新的对象地址上, 然后把新地址插入到转发器, 此时插入失败代表其他线程抢先完成了转移任务, 此时回滚内存分配, 并返回其他线程的转移结果
   * 4. 走到这一步代表转发表已经失效, 或者目标页表内存不足, 此时会插入到任务队列中, concurrent_relocate阶段会处理这部分任务
   */
  static zaddress relocate_or_remap(zaddress_unsafe addr, ZGeneration* generation);
  static zaddress remap(zaddress_unsafe addr, ZGeneration* generation);
  static void remember(volatile zpointer* p);
  static void mark_and_remember(volatile zpointer* p, zaddress addr);

  // Fast paths in increasing strength level
  static bool is_load_good_or_null_fast_path(zpointer ptr);
  static bool is_mark_good_fast_path(zpointer ptr);
  static bool is_store_good_fast_path(zpointer ptr);
  static bool is_store_good_or_null_fast_path(zpointer ptr);
  static bool is_store_good_or_null_any_fast_path(zpointer ptr);

  /**
   * 非null && ZPointer::is_load_good && ZPointer::is_marked_young
   */
  static bool is_mark_young_good_fast_path(zpointer ptr);
  static bool is_finalizable_good_fast_path(zpointer ptr);

  // Slow paths
  /**
   * 如果地址是年轻代, 且gc处于标记阶段, 则做一次标记
   * 和blocking_keep_alive_on_phantom_slow_path完全相同
   * @param p 无作用, 仅用于保持函数签名
   * @param addr 待检查的地址
   * @return 如果传入的地址为null, 或是没有强引用的老年代对象地址, 则返回null, 否则返回addr 
   * @see blocking_keep_alive_on_phantom_slow_path
   */
  static zaddress blocking_keep_alive_on_weak_slow_path(volatile zpointer* p, zaddress addr);

  /**
   * 如果地址是年轻代, 且gc处于标记阶段, 则做一次标记
   * 和blocking_load_barrier_on_phantom_slow_path完全相同
   * @param p 无作用, 仅用于保持函数签名
   * @param addr 待检查的地址
   * @return 如果传入的地址为null, 或是没有强/final引用的老年代对象地址, 则返回null, 否则返回addr
   * @see blocking_load_barrier_on_phantom_slow_path
   */
  static zaddress blocking_keep_alive_on_phantom_slow_path(volatile zpointer* p, zaddress addr);

  /**
   * 如果地址是年轻代, 且gc处于标记阶段, 则做一次标记
   * 和blocking_keep_alive_on_weak_slow_path完全相同
   * @param p 无作用, 仅用于保持函数签名
   * @param addr 待检查的地址
   * @return 如果传入的地址为null, 或是没有强引用的老年代对象地址, 则返回null, 否则返回addr 
   * @see blocking_keep_alive_on_weak_slow_path
   */
  static zaddress blocking_load_barrier_on_weak_slow_path(volatile zpointer* p, zaddress addr);

  /**
   * 如果地址是年轻代, 且gc处于标记阶段, 则做一次标记
   * 和blocking_keep_alive_on_phantom_slow_path完全相同
   * @param p 无作用, 仅用于保持函数签名
   * @param addr 待检查的地址
   * @return 如果传入的地址为null, 或是没有强/final引用的老年代对象地址, 则返回null, 否则返回addr
   * @see blocking_keep_alive_on_phantom_slow_path
   */
  static zaddress blocking_load_barrier_on_phantom_slow_path(volatile zpointer* p, zaddress addr);

  static zaddress mark_slow_path(zaddress addr);

  /**
   * 如果地址对应的是年轻代对象, 则执行一次标记
   */
  static zaddress mark_young_slow_path(zaddress addr);
  
  /**
   * 如果地址是年轻代对象则执行标记; 如果此时正在执行major-gc也会执行标记
   * 标记规则为<ZMark::DontResurrect, ZMark::GCThread, ZMark::Follow, ZMark::Strong>
   */
  static zaddress mark_from_young_slow_path(zaddress addr);
  static zaddress mark_from_old_slow_path(zaddress addr);
  static zaddress mark_finalizable_slow_path(zaddress addr);
  static zaddress mark_finalizable_from_old_slow_path(zaddress addr);

  /**
   * 如果addr非空, 执行mark<ZMark::Resurrect, ZMark::AnyThread, ZMark::Follow, ZMark::Strong>(addr)
   * @return 始终返回addr自身
   */
  static zaddress keep_alive_slow_path(zaddress addr);
  static zaddress heap_store_slow_path(volatile zpointer* p, zaddress addr, zpointer prev, bool heal);
  static zaddress native_store_slow_path(zaddress addr);
  static zaddress no_keep_alive_heap_store_slow_path(volatile zpointer* p, zaddress addr);

  /**
   * 什么都不做直接返回地址
   */
  static zaddress promote_slow_path(zaddress addr);

  // Helpers for non-strong oop refs barriers
  static zaddress blocking_keep_alive_load_barrier_on_weak_oop_field_preloaded(volatile zpointer* p, zpointer o);
  static zaddress blocking_keep_alive_load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o);

  /**
   * 如果是年轻代且处于gc标记阶段, 则做一次标记, 否则无操作
   * @return 如果传入的地址为null, 或是没有强引用的老年代对象地址, 则返回null, 否则返回对象地址
   */
  static zaddress blocking_load_barrier_on_weak_oop_field_preloaded(volatile zpointer* p, zpointer o);
  static zaddress blocking_load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o);

  // Verification
  static void verify_on_weak(volatile zpointer* referent_addr) NOT_DEBUG_RETURN;

public:

  static zpointer load_atomic(volatile zpointer* p);

  // Helpers for relocation
  static ZGeneration* remap_generation(zpointer ptr);
  static void remap_young_relocated(volatile zpointer* p, zpointer o);

  // Helpers for marking

  /**
   * 调用ZGeneration::mark_object_if_active. 如果地址是年轻代, 则finalizable参数必定为ZMark::Strong, 否则传递该参数
   */
  template <bool resurrect, bool gc_thread, bool follow, bool finalizable>
  static void mark(zaddress addr);

  /**
   * 传递resurrect gc_thread follow三个参数, 补充参数finalizable=ZMark::Strong调用ZGeneration::mark_object
   */
  template <bool resurrect, bool gc_thread, bool follow>
  static void mark_young(zaddress addr);

  
  /**
   * 如果地址属于年轻代, 传递resurrect gc_thread follow三个参数, 补充参数finalizable=ZMark::Strong调用ZGeneration::mark_object, 否则无操作
   */
  template <bool resurrect, bool gc_thread, bool follow>
  static void mark_if_young(zaddress addr);

  // Load barrier

  /**
   * 按照load_good进行染色, 如果prev是null, 染色为ZPointerMarkGoodMask | ZPointerRemembered | ZPointerRememberedMask, 否则染色为addr | ZPointerLoadGoodMask | ZPointerRememberedMask | (prev & (ZPointerMarkedMask & (~ZPointerLoadMetadataMask)))
   * 解引用p后调用load_barrier_on_oop_field_preloaded
   */
  static zaddress load_barrier_on_oop_field(volatile zpointer* p);

  /**
   * 按照load_good进行染色, 如果prev是null, 染色为ZPointerMarkGoodMask | ZPointerRemembered | ZPointerRememberedMask, 否则染色为addr | ZPointerLoadGoodMask | ZPointerRememberedMask | (prev & (ZPointerMarkedMask & (~ZPointerLoadMetadataMask)))
   */
  static zaddress load_barrier_on_oop_field_preloaded(volatile zpointer* p, zpointer o);

  /**
   * 遍历数组内的元素, 执行load_barrier_on_oop_field
   */
  static void load_barrier_on_oop_array(volatile zpointer* p, size_t length);

  static zaddress keep_alive_load_barrier_on_oop_field_preloaded(volatile zpointer* p, zpointer o);

  // Load barriers on non-strong oop refs
  static zaddress load_barrier_on_weak_oop_field_preloaded(volatile zpointer* p, zpointer o);
  static zaddress load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o);

  /**
   * 如果禁用了引用复活机制, 则仅在对象属于年轻代且处于gc标记阶段时做一次标记, 此时如果对象没有被引用就返回null;
   * 如果启用了引用复活机制, 则将指针染色为load_good, 返回对象地址
   */
  static zaddress no_keep_alive_load_barrier_on_weak_oop_field_preloaded(volatile zpointer* p, zpointer o);
  static zaddress no_keep_alive_load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o);

  // Reference processor / weak cleaning barriers

  /**
   * 如果地址属于老年代且没有被强引用则返回true, 否则返回false; 如果地址属于年轻代且处于young-mark阶段则做一次标记
   */
  static bool clean_barrier_on_weak_oop_field(volatile zpointer* p);

  /**
   * 如果地址属于老年代且没有被强/final引用则返回true, 否则返回false; 如果地址属于年轻代且处于young-mark阶段则做一次标记
   */
  static bool clean_barrier_on_phantom_oop_field(volatile zpointer* p);

  /**
   * 如果地址属于老年代且仅被final引用未被强引用时返回true, 否则返回false; 如果地址属于年轻代且处于young-mark阶段则做一次标记
   */
  static bool clean_barrier_on_final_oop_field(volatile zpointer* p);

  // Mark barrier

  /**
   * 更新为store_good
   */
  static void mark_barrier_on_young_oop_field(volatile zpointer* p);

  /**
   * 如果finalizable为true则更新为finalizable_good, 否则更新为mark_good
   */
  static void mark_barrier_on_old_oop_field(volatile zpointer* p, bool finalizable);

  /**
   * 如果finalizable为true则更新为finalizable_good, 否则更新为mark_good
   */
  static void mark_barrier_on_oop_field(volatile zpointer* p, bool finalizable);

  /**
   * 对未经历过本轮标记的年轻代对象进行一次标记
   * 并将指针颜色调整为ZPointerLoadGoodMask | ZPointerMarkedYoung | ZPointerRememberedMask
   */
  static void mark_young_good_barrier_on_oop_field(volatile zpointer* p);
  static zaddress remset_barrier_on_oop_field(volatile zpointer* p);

  /**
   * 染色成ZPointerStoreGoodMask
   */
  static void promote_barrier_on_young_oop_field(volatile zpointer* p);

  /**
   * Store barrier似乎只用于jit, c1和c2各有一个引用
   */
  // Store barrier
  static void store_barrier_on_heap_oop_field(volatile zpointer* p, bool heal);
  static void store_barrier_on_native_oop_field(volatile zpointer* p, bool heal);

  static void no_keep_alive_store_barrier_on_heap_oop_field(volatile zpointer* p);
};

#endif // SHARE_GC_Z_ZBARRIER_HPP
