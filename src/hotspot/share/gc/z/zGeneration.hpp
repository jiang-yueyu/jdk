/*
 * Copyright (c) 2021, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZGENERATION_HPP
#define SHARE_GC_Z_ZGENERATION_HPP

#include "gc/z/zForwardingTable.hpp"
#include "gc/z/zGenerationId.hpp"
#include "gc/z/zMark.hpp"
#include "gc/z/zReferenceProcessor.hpp"
#include "gc/z/zRelocate.hpp"
#include "gc/z/zRelocationSet.hpp"
#include "gc/z/zRemembered.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zTracer.hpp"
#include "gc/z/zUnload.hpp"
#include "gc/z/zWeakRootsProcessor.hpp"
#include "gc/z/zWorkers.hpp"
#include "memory/allocation.hpp"

class ThreadClosure;
class ZForwardingTable;
class ZGenerationOld;
class ZGenerationYoung;
class ZPage;
class ZPageAllocator;
class ZPageTable;
class ZRelocationSetSelector;

class ZGeneration {
  friend class ZForwardingTest;
  friend class ZLiveMapTest;

protected:
  static ZGenerationYoung* _young; // 初始化后不变
  static ZGenerationOld*   _old; // 初始化后不变

  enum class Phase {
    Mark,
    MarkComplete,
    Relocate
  };

  const ZGenerationId   _id;
  ZPageAllocator* const _page_allocator;
  ZPageTable* const     _page_table;
  ZForwardingTable      _forwarding_table;
  ZWorkers              _workers;
  ZMark                 _mark;
  ZRelocate             _relocate;
  ZRelocationSet        _relocation_set;

  volatile size_t       _freed;
  volatile size_t       _promoted;
  volatile size_t       _compacted;

  Phase                 _phase;

  /**
   * gc计数, 在mark_start阶段更新
   */
  uint32_t              _seqnum;

  ZStatHeap             _stat_heap;
  ZStatCycle            _stat_cycle;
  ZStatWorkers          _stat_workers;
  ZStatMark             _stat_mark;
  ZStatRelocation       _stat_relocation;

  ConcurrentGCTimer*    _gc_timer;

  /**
   * 当selector中的空页表数量大于bulk时回收掉空页表
   */
  void free_empty_pages(ZRelocationSetSelector* selector, int bulk);

  /**
   * 如果是YGC, 对未选中的年轻代页表执行晋升
   * 如果需要执行分代晋升, 会遍历页表内的对象的每个对象字段, 更新指针颜色到ZPointerStoreGoodMask
   * 执行年龄晋升. 如果需要执行分代晋升, 则是对页表生成一份拷贝, 在拷贝上执行年龄晋升
   * 如果需要执行分代晋升, 会置换page_table的页表对象, 置换成页表拷贝, 并更新相关的计数和统计值
   * 最后遍历分代晋升的页表, 追加到转移集的已转移页表里面. ?? TODO 这一步涉及到remembered_set ??
   */
  void flip_age_pages(const ZRelocationSetSelector* selector);

  /**
   * 清理标记栈的内存空间, 更新统计数据
   */
  void mark_free();

  /**
   * 1. 首先遍历当前分代的页表, 在能够转移的页表中, 选取被标记过的页表注册到selector当中, 否则视作空页表, 然后对空页表做一次清理
   * 2. 执行页表选取:
   * - 对小中大三个selector分别执行select函数
   * - 大型页表始终不会被选取, 中小型页表会根据启动参数的内存碎片阈值, 选取若干个页表
   * 3. 计算一次分代提前晋升所需的年龄阈值
   * 4. 将页表选取结果装载到转移集
   * - 根据选中的页表构造转发表
   * - 如果转移过程伴随分代晋升, 会遍历页表对象的对象字段, 将指针颜色更新为ZPointerStoreGoodMask
   * - 然后将构造出来的转发表插入到转移集
   * 5. 如果处于YGC, 则对未选中的年轻代页表执行晋升
   * - 如果需要执行分代晋升, 会遍历页表内的对象的每个对象字段, 更新指针颜色到ZPointerStoreGoodMask
   * - 执行年龄晋升. 如果需要执行分代晋升, 则是对页表生成一份拷贝, 在拷贝上执行年龄晋升
   * - 如果需要执行分代晋升, 会置换page_table的页表对象, 置换成页表拷贝, 并更新相关的计数和统计值
   * - 最后遍历分代晋升的页表, 追加到转移集的已转移页表里面
   * 6. 将转移集里的转发表插入到转发表收集器
   * 7. 更新统计值
   */
  void select_relocation_set(ZGenerationId generation, bool promote_all);

  /**
   * 1. 遍历_relocation_set, 将所有的元素从_forwarding_table中移除
   * 2. 重置掉所有的转发表, 然后销毁掉相关的页表对象(仅销毁对象并清空容器, 但不回收页表内存)
   */
  void reset_relocation_set();

  ZGeneration(ZGenerationId id, ZPageTable* page_table, ZPageAllocator* page_allocator);

  void log_phase_switch(Phase from, Phase to);

public:
  bool is_initialized() const;

  // GC phases
  void set_phase(Phase new_phase);
  bool is_phase_relocate() const;
  bool is_phase_mark() const;
  bool is_phase_mark_complete() const;
  const char* phase_to_string() const;

  uint32_t seqnum() const;

  ZGenerationId id() const;
  ZGenerationIdOptional id_optional() const;
  bool is_young() const;
  bool is_old() const;

  static ZGenerationYoung* young();
  static ZGenerationOld* old();
  static ZGeneration* generation(ZGenerationId id);

  // Statistics
  void reset_statistics();
  virtual bool should_record_stats() = 0;
  size_t freed() const;

  /**
   * 修改被回收的内存尺寸
   */
  void increase_freed(size_t size);
  size_t promoted() const;
  void increase_promoted(size_t size);
  size_t compacted() const;
  void increase_compacted(size_t size);

  ConcurrentGCTimer* gc_timer() const;
  void set_gc_timer(ConcurrentGCTimer* gc_timer);
  void clear_gc_timer();

  ZStatHeap* stat_heap();
  ZStatCycle* stat_cycle();
  ZStatWorkers* stat_workers();
  ZStatMark* stat_mark();
  ZStatRelocation* stat_relocation();

  void at_collection_start(ConcurrentGCTimer* gc_timer);
  void at_collection_end();

  // Workers
  ZWorkers* workers();
  uint active_workers() const;
  void set_active_workers(uint nworkers);

  // Worker resizing
  bool should_worker_resize();

  ZPageTable* page_table() const;
  const ZForwardingTable* forwarding_table() const;
  ZForwarding* forwarding(zaddress_unsafe addr) const;

  ZRelocationSetParallelIterator relocation_set_parallel_iterator();

  // Marking
  
  /**
   * 可见地址标记的时候follow都是true
   */
  template <bool resurrect, bool gc_thread, bool follow, bool finalizable>
  void mark_object(zaddress addr);
  template <bool resurrect, bool gc_thread, bool follow, bool finalizable>
  void mark_object_if_active(zaddress addr);

  /**
   * 将线程独享的标记栈转移到全局标记器条纹_stripes中
   */
  void mark_flush_and_free(Thread* thread);

  // Relocation
  void synchronize_relocation();
  void desynchronize_relocation();
  bool is_relocate_queue_active() const;

  /**
   * 1. 如果地址没有对应的转发器, 则立即返回原始地址
   * 2. 在转发器上执行一次地址查找, 如果能查到值代表对象已经被转移, 直接返回转移后的地址
   * 3. 如果转发器仍然有效, 且目标页表能够分配出相同尺寸的对象, 则直接把对象数据拷贝到新的对象地址上, 然后把新地址插入到转发器, 此时插入失败代表其他线程抢先完成了转移任务, 此时回滚内存分配, 并返回其他线程的转移结果
   * 4. 走到这一步代表转发表已经失效, 或者目标页表内存不足, 此时会插入到任务队列中, concurrent_relocate阶段会处理这部分任务
   */
  zaddress relocate_or_remap_object(zaddress_unsafe addr);
  zaddress remap_object(zaddress_unsafe addr);

  // Threads
  void threads_do(ThreadClosure* tc) const;
};

enum class ZYoungType {
  minor,
  major_full_preclean,
  major_full_roots,
  major_partial_roots,
  none
};

class ZYoungTypeSetter {
public:
  ZYoungTypeSetter(ZYoungType type);
  ~ZYoungTypeSetter();
};

class ZGenerationYoung : public ZGeneration {
  friend class VM_ZMarkEndYoung;
  friend class VM_ZMarkStartYoung;
  friend class VM_ZMarkStartYoungAndOld;
  friend class VM_ZRelocateStartYoung;
  friend class ZYoungTypeSetter;

private:
  ZYoungType   _active_type;

  /**
   * 提前晋升的年龄阈值
   */
  uint         _tenuring_threshold;
  ZRemembered  _remembered;
  ZYoungTracer _jfr_tracer;

  void flip_mark_start();

  /**
   * ZPointerRemappedYoungMask相位反转, 然后更新指针颜色
   */
  void flip_relocate_start();

  /**
   * 1. 更新指针颜色
   * 2. 情况各个内存分配器的共享页表
   * 3. 更新统计值
   * 4. 更新分代年龄
   * 5. 状态流转到Mark
   * 6. 启动标记任务
   * - 将计数器清零
   * - 设置工作线程数
   * - 根据工作线程数计算条纹数nstripes
   * - 更新统计值
   * 7. 切换remembered_set存储器的current和previous
   * 8. 更新统计值
   */
  void mark_start();

  /**
   *
   * 扫描strong+weak所有的oop-storage中的对象, classloader及其class module 常量等对象, 函数调用栈中的对象, 对扫描到的gcroot对象执行ZMarkYoungOopClosure
   * 此处的标记会将指针颜色调整为ZPointerLoadGoodMask | ZPointerMarkedYoung | ZPointerRememberedMask, 并将对象推入到标记栈中
   */
  void mark_roots();
  void mark_follow();

  /**
   * 1. 如果终止器被重新激活, 或者非java线程中仍存在标记任务, 则返回false
   * 2. 更新相位至Phase::MarkComplete, 更新统计值
   * 3. JvmtiTagMap ?? TODO 具体做了什么 ??
   * 4. 返回true
   */
  bool mark_end();

  /**
   * ZPointerRemappedYoungMask相位反转, 然后更新指针颜色
   * 将状态流转到Relocate
   * 更新统计值
   * 启用转移集的任务队列
   */
  void relocate_start();

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
  void relocate();

  void pause_mark_start();
  void concurrent_mark();

  /**
   * 1. 如果终止器被重新激活, 或者非java线程中仍存在标记任务, 则返回false
   * 2. 更新相位至Phase::MarkComplete, 更新统计值
   * 3. JvmtiTagMap ?? TODO 具体做了什么 ??
   * 4. 返回true
   */
  bool pause_mark_end();

  /**
   * 执行mark_follow
   */
  void concurrent_mark_continue();

  /**
   * 清理标记栈的内存空间, 更新统计数据
   */
  void concurrent_mark_free();

  /**
   * 1. 遍历_relocation_set, 将所有的元素从_forwarding_table中移除
   * 2. 重置掉所有的转发表, 然后销毁掉相关的页表对象(仅销毁对象并清空容器, 但不回收页表内存)
   */
  void concurrent_reset_relocation_set();

  /**
   * 1. 首先遍历当前分代的页表, 在能够转移的页表中, 选取被标记过的页表注册到selector当中, 否则视作空页表, 然后对空页表做一次清理
   * 2. 执行页表选取:
   * - 对小中大三个selector分别执行select函数
   * - 大型页表始终不会被选取, 中小型页表会根据启动参数的内存碎片阈值, 选取若干个页表
   * 3. 计算一次分代提前晋升所需的年龄阈值
   * 4. 将页表选取结果装载到转移集
   * - 根据选中的页表构造转发表
   * - 如果转移过程伴随分代晋升, 会遍历页表对象的对象字段, 将指针颜色更新为ZPointerStoreGoodMask
   * - 然后将构造出来的转发表插入到转移集
   * 5. 如果处于YGC, 则对未选中的年轻代页表执行晋升
   * - 如果需要执行分代晋升, 会遍历页表内的对象的每个对象字段, 更新指针颜色到ZPointerStoreGoodMask
   * - 执行年龄晋升. 如果需要执行分代晋升, 则是对页表生成一份拷贝, 在拷贝上执行年龄晋升
   * - 如果需要执行分代晋升, 会置换page_table的页表对象, 置换成页表拷贝, 并更新相关的计数和统计值
   * - 最后遍历分代晋升的页表, 追加到转移集的已转移页表里面
   * 6. 将转移集里的转发表插入到转发表收集器
   * 7. 更新统计值
   */
  void concurrent_select_relocation_set();
  void pause_relocate_start();
  void concurrent_relocate();

public:
  ZGenerationYoung(ZPageTable* page_table,
                   const ZForwardingTable* old_forwarding_table,
                   ZPageAllocator* page_allocator);

  ZYoungType type() const;

  void collect(ZYoungType type, ConcurrentGCTimer* timer);

  // Statistics
  bool should_record_stats();

  // Support for promoting object to the old generation
  /**
   * 首先在page_table里置换页表对象, 然后更新相应的计数和统计值
   */
  void flip_promote(ZPage* from_page, ZPage* to_page);

  /**
   * 置换页表并调整两个分代的尺寸
   */
  void in_place_relocate_promote(ZPage* from_page, ZPage* to_page);

  /**
   * 执行转移集的register_flip_promoted方法. ?? TODO 目的是什么 ??
   */
  void register_flip_promoted(const ZArray<ZPage*>& pages);

  /**
   * 将原地转移并晋升的旧页表注册到待回收列表中
   */
  void register_in_place_relocate_promoted(ZPage* page);

  /**
   * 提前晋升的年龄阈值
   * 在并发转移阶段, 判断分代晋升时使用
   */
  uint tenuring_threshold();

  /**
   * 设置提前执行分代晋升的年龄阈值
   * 如果promote_all==true, 设置为0, 代表立即晋升
   * 如果设置过ZTenuringThreshold, 设置为该值
   * 否则根据统计结果计算
   */
  void select_tenuring_threshold(ZRelocationSetSelectorStats stats, bool promote_all);
  uint compute_tenuring_threshold(ZRelocationSetSelectorStats stats);

  // Add remembered set entries
  void remember(volatile zpointer* p);

  // Scan a remembered set entry
  void scan_remembered_field(volatile zpointer* p);

  // Register old pages with remembered set
  void register_with_remset(ZPage* page);

  // Serviceability
  ZGenerationTracer* jfr_tracer();

  // Verification
  bool is_remembered(volatile zpointer* p) const;
};

class ZGenerationOld : public ZGeneration {
  friend class VM_ZMarkEndOld;
  friend class VM_ZMarkStartYoungAndOld;
  friend class VM_ZRelocateStartOld;

private:
  ZReferenceProcessor _reference_processor;
  ZWeakRootsProcessor _weak_roots_processor;
  ZUnload             _unload;
  uint                _total_collections_at_start;

  /**
   * relocate_start发生时的年轻代年龄
   */
  uint32_t            _young_seqnum_at_reloc_start;
  ZOldTracer          _jfr_tracer;

  void flip_mark_start();
  void flip_relocate_start();

  void mark_start();

  /**
   * 遍历oop-storage-set & java线程 & 本地方法区的强根, 将根节点标记为mark_good
   * 完成后将工作线程独享的标记栈转移到全局条纹中
   */
  void mark_roots();

  /**
   * 完成标记任务并尝试终止标记器的运行, 结束后将young old两个代的线程独享标记栈转移到全局条纹中
   */
  void mark_follow();

  /**
   * 1. 如果终止器被重新激活, 或者非java线程中仍存在标记任务, 则返回false
   * 2. 更新相位至Phase::MarkComplete, 更新统计值
   * 3. 禁用引用复活
   * 4. 准备类卸载 ?? TODO 具体做了什么 ??
   * 5. JvmtiTagMap ?? TODO 具体做了什么 ??
   * 6. CodeCache ?? TODO 具体做了什么 ??
   * 7. 返回true
   */
  bool mark_end();

  /**
   * 1. 将引用处理器中已发现的待清理引用添加到pending列表中
   * 2. 遍历weak类型的oop-storage-set, 如果处于young-mark阶段, 对其中的年轻代对象做一次标记
   * 3. 执行类卸载
   * 4. ?? TODO 让所有java线程进入到特定状态 ??
   * 5. ?? TODO 让所有vm线程进入到特定状态 ??
   * 6. 恢复引用复活机制
   * 7. 清理被卸载的类加载器对应的本地方法区
   * 8. 将ReferenceProcessor的待清理列表转移到全局列表上, 等待ReferenceHandler线程做最终处理
   * 9. 给ClassLoader设置标记 ?? TODO ??
   */
  void process_non_strong_references();

  /**
   * 1. 调整元空间的尺寸
   * 2. 切换染色
   * 3. 相位更新至Phase::Relocate
   * 4. 更新统计值
   * 5. 记录此时的年轻代年龄
   * 6. 启用转移队列
   */
  void relocate_start();

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
  void relocate();

  /**
   * 遍历strong weak两个oop-storage-set和ClassLoaderDataGraph, 对每个gcroot执行一次转移并染色为load_good
   * 然后?? TODO remembered深坑 ??
   * 会同时利用young old两个代的工作线程执行任务 ?? TODO 待确认 ??
   */
  void remap_young_roots();

  /**
   * 1. 遍历oop-storage-set & java线程 & 本地方法区的强根, 将根节点标记为mark_good, 完成后将工作线程独享的标记栈转移到全局条纹中
   * 2. 完成标记任务并尝试终止标记器的运行, 结束后将young old两个代的线程独享标记栈转移到全局条纹中
   */
  void concurrent_mark();

 /**
  * 1. 如果终止器被重新激活, 或者非java线程中仍存在标记任务, 则返回false
  * 2. 更新相位至Phase::MarkComplete, 更新统计值
  * 3. 禁用引用复活
  * 4. 准备类卸载 ?? TODO 具体做了什么 ??
  * 5. JvmtiTagMap ?? TODO 具体做了什么 ??
  * 6. CodeCache ?? TODO 具体做了什么 ??
  * 7. 返回true
  */
  bool pause_mark_end();

  /**
   * 完成标记任务并尝试终止标记器的运行, 结束后将young old两个代的线程独享标记栈转移到全局条纹中
   */
  void concurrent_mark_continue();
  
  /**
   * 清理标记栈的内存空间, 更新统计数据
   */
  void concurrent_mark_free();

  /**
   * 1. 将引用处理器中已发现的待清理引用添加到pending列表中
   * 2. 遍历weak类型的oop-storage-set, 如果处于young-mark阶段, 对其中的年轻代对象做一次标记
   * 3. 执行类卸载
   * 4. ?? TODO 让所有java线程进入到特定状态 ??
   * 5. ?? TODO 让所有vm线程进入到特定状态 ??
   * 6. 恢复引用复活机制
   * 7. 清理被卸载的类加载器对应的本地方法区
   * 8. 将ReferenceProcessor的待清理列表转移到全局列表上, 等待ReferenceHandler线程做最终处理
   * 9. 给ClassLoader设置标记 ?? TODO ??
   */
  void concurrent_process_non_strong_references();

  /**
   * 1. 遍历_relocation_set, 将所有的元素从_forwarding_table中移除
   * 2. 重置掉所有的转发表, 然后销毁掉相关的页表对象(仅销毁对象并清空容器, 但不回收页表内存)
   */
  void concurrent_reset_relocation_set();
  void pause_verify();

  /**
   * 1. 首先遍历当前分代的页表, 在能够转移的页表中, 选取被标记过的页表注册到selector当中, 否则视作空页表, 然后对空页表做一次清理
   * 2. 执行页表选取:
   * - 对小中大三个selector分别执行select函数
   * - 大型页表始终不会被选取, 中小型页表会根据启动参数的内存碎片阈值, 选取若干个页表
   * 3. 计算一次分代提前晋升所需的年龄阈值
   * 4. 将页表选取结果装载到转移集
   * - 根据选中的页表构造转发表
   * - 如果转移过程伴随分代晋升, 会遍历页表对象的对象字段, 将指针颜色更新为ZPointerStoreGoodMask
   * - 然后将构造出来的转发表插入到转移集
   * 5. 如果处于YGC, 则对未选中的年轻代页表执行晋升
   * - 如果需要执行分代晋升, 会遍历页表内的对象的每个对象字段, 更新指针颜色到ZPointerStoreGoodMask
   * - 执行年龄晋升. 如果需要执行分代晋升, 则是对页表生成一份拷贝, 在拷贝上执行年龄晋升
   * - 如果需要执行分代晋升, 会置换page_table的页表对象, 置换成页表拷贝, 并更新相关的计数和统计值
   * - 最后遍历分代晋升的页表, 追加到转移集的已转移页表里面
   * 6. 将转移集里的转发表插入到转发表收集器
   * 7. 更新统计值
   */
  void concurrent_select_relocation_set();

  /**
   * 1. 调整元空间的尺寸
   * 2. 切换染色
   * 3. 相位更新至Phase::Relocate
   * 4. 更新统计值
   * 5. 记录此时的年轻代年龄
   * 6. 启用转移队列
   */
  void pause_relocate_start();

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
  void concurrent_relocate();

  /**
   * 遍历strong weak两个oop-storage-set和ClassLoaderDataGraph, 对每个gcroot执行一次转移并染色为load_good
   * 然后?? TODO remembered深坑 ??
   * 会同时利用young old两个代的工作线程执行任务 ?? TODO 待确认 ??
   */
  void concurrent_remap_young_roots();

public:
  ZGenerationOld(ZPageTable* page_table, ZPageAllocator* page_allocator);

  void collect(ConcurrentGCTimer* timer);

  // Statistics
  bool should_record_stats();

  // Reference processing
  ReferenceDiscoverer* reference_discoverer();
  void set_soft_reference_policy(bool clear);
  bool uses_clear_all_soft_reference_policy() const;

  uint total_collections_at_start() const;

  bool active_remset_is_current() const;

  ZRelocateQueue* relocate_queue();

  // Serviceability
  ZGenerationTracer* jfr_tracer();
};

#endif // SHARE_GC_Z_ZGENERATION_HPP
