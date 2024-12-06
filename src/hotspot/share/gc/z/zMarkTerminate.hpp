/*
 * Copyright (c) 2017, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZMARKTERMINATE_HPP
#define SHARE_GC_Z_ZMARKTERMINATE_HPP

#include "gc/z/zLock.hpp"
#include "utilities/globalDefinitions.hpp"

class ZMarkStripeSet;

class ZMarkTerminate {
private:
  uint           _nworkers;
  volatile uint  _nworking;
  volatile uint  _nawakening;
  volatile bool  _resurrected;
  ZConditionLock _lock;

  /**
   * 如果stripes中的条纹数和used_nstripes相等且大于1, 则将stripes中的条纹数减半
   * ?? TODO 这是为了干嘛 ??
   */
  void maybe_reduce_stripes(ZMarkStripeSet* stripes, size_t used_nstripes);

public:
  ZMarkTerminate();

  /**
   * _nworkers和_nworking设置为nworkers, _nawakening归零
   */
  void reset(uint nworkers);

  /**
   * 加锁后将_nworking-1, 如果_nworking归零, 则执行notify_all
   */
  void leave();

  /**
   * 如果工作中的线程数+唤醒中的线程数的总和恰好等于总线程数, 判定为已饱和
   * @return nworking + nawakening == _nworkers
   */
  bool saturated() const;

  /**
   * 如果工作中的线程数+唤醒中的线程数的总和恰好等于总线程数(已饱和), 或者总线程数为0, 则直接返回
   * 然后锁住_lock，如果上述的等式仍不成立, 将唤醒中的线程数+1, 并对_lock执行notify_one
   */
  void wake_up();

  /**
   * 锁住_lock, _nworking--
   * 如果此时_nworking归零, 则执行notify_all并返回true
   * 如果stripes中的条纹数和used_nstripes相等且大于1, 则将stripes中的条纹数减半 ?? TODO 为什么要减半, 或者说又回到之前的问题, 条纹数到底有什么含义 ??
   * 进入等待状态(标记条纹中插入标记栈时, 会调用wake_up()进行唤醒; leave()完全退出时也会唤醒)
   * 结束后如果_nawakening仍大于0则减去1, 此时如果_nworking为0则返回true
   * 最后将_nworking复位并返回false
   * @return _nworking归零时返回true
   */
  bool try_terminate(ZMarkStripeSet* stripes, size_t used_nstripes);
  void set_resurrected(bool value);
  bool resurrected() const;
};

#endif // SHARE_GC_Z_ZMARKTERMINATE_HPP
