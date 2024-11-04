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

#ifndef SHARE_GC_Z_ZLIVEMAP_HPP
#define SHARE_GC_Z_ZLIVEMAP_HPP

#include "gc/z/zAddress.hpp"
#include "gc/z/zBitMap.hpp"
#include "gc/z/zGenerationId.hpp"
#include "memory/allocation.hpp"

class ObjectClosure;

class ZLiveMap {
  friend class ZLiveMapTest;

private:
  static const size_t nsegments = 64;

  /**
   * 用这个年龄和当前分代年龄比较, 可以判断出livemap的拥有者是否被标记过
   */
  volatile uint32_t _seqnum;
  volatile uint32_t _live_objects;
  volatile size_t   _live_bytes;

  /**
   * 64位的bitset, 指示一个段是否存活
   */
  BitMap::bm_word_t _segment_live_bits;

  /**
   * 64位的bitset, 用作自旋锁, 指示一个段是否已经被独占
   */
  BitMap::bm_word_t _segment_claim_bits;

  /**
   * 分为64个段, 指示相应的地址是否存活
   * 相邻两个位是一个组, finalizable仅标记其中的奇数位, 强引用会标记奇偶两个位
   */
  ZBitMap           _bitmap;

  /**
   * 仅在页表发生尺寸变更时更新
   */
  int               _segment_shift;

  /**
   * _segment_live_bits的视图
   */
  const BitMapView segment_live_bits() const;

  /**
   * _segment_claim_bits的视图
   */
  const BitMapView segment_claim_bits() const;

  /**
   * _segment_live_bits的视图
   */
  BitMapView segment_live_bits();

  /**
   * _segment_claim_bits的视图
   */
  BitMapView segment_claim_bits();

  /**
   * 单个段的尺寸
   */
  BitMap::idx_t segment_size() const;

  BitMap::idx_t segment_start(BitMap::idx_t segment) const;
  BitMap::idx_t segment_end(BitMap::idx_t segment) const;

  bool is_segment_live(BitMap::idx_t segment) const;
  bool set_segment_live(BitMap::idx_t segment);

  BitMap::idx_t first_live_segment() const;
  BitMap::idx_t next_live_segment(BitMap::idx_t segment) const;
  BitMap::idx_t index_to_segment(BitMap::idx_t index) const;

  /**
   * 尝试加自旋锁
   */
  bool claim_segment(BitMap::idx_t segment);

  /**
   * 如果当前的年龄不等于分代的最新年龄, 则在加锁以后清空计数和live_bits claim_bits两个段, 并更新到最新年龄
   */
  void reset(ZGenerationId id);

  /**
   * 给指定的段加自旋锁后, 清理掉这个段上的存活记录, 然后将这个段标记为存活
   * 仅在执行标记且段未存活时被调用
   */
  void reset_segment(BitMap::idx_t segment);

  size_t do_object(ObjectClosure* cl, zaddress addr) const;

  template <typename Function>
  void iterate_segment(BitMap::idx_t segment, Function function);

public:
  ZLiveMap(uint32_t size);
  ZLiveMap(const ZLiveMap& other) = delete;

  void reset();
  void resize(uint32_t size);

  /**
   * 比较当前年龄和分代的最新年龄, 相等代表已经被标记过
   */
  bool is_marked(ZGenerationId id) const;

  uint32_t live_objects() const;
  size_t live_bytes() const;

  /**
   * @return 页表年龄等于最新分代年龄 && 所属段已被标记 && 地址已被标记
   */
  bool get(ZGenerationId id, BitMap::idx_t index) const;

  /**
   * 首先更新自身分代年龄, 然后设置标记位
   */
  bool set(ZGenerationId id, BitMap::idx_t index, bool finalizable, bool& inc_live);

  void inc_live(uint32_t objects, size_t bytes);

  template <typename Function>
  void iterate(ZGenerationId id, Function function);

  BitMap::idx_t find_base_bit(BitMap::idx_t index);
  BitMap::idx_t find_base_bit_in_segment(BitMap::idx_t start, BitMap::idx_t index);
};

#endif // SHARE_GC_Z_ZLIVEMAP_HPP
