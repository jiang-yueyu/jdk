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
  /**
   * 总线程数, 只会被reset方法更新
   */
  uint           _nworkers;

  /**
   * 工作中的线程数
   */
  volatile uint  _nworking;

  /**
   * 正处于唤醒阶段的线程数, 执行notify_one前会将该计数+1, 线程被唤醒后会将这个计数-1
   */
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
   * 如果工作中的线程数+唤醒中的线程数的总和恰好等于总线程数(已饱和), 或者工作中的线程数为0(已完成), 则直接返回
   * 然后锁住_lock，如果上述的等式仍不成立, 则唤醒一个线程(将唤醒中的线程数+1, 并对_lock执行notify_one)
   */
  void wake_up();

  /**
   * 加锁并执行以下步骤:
   * 1. 停止当前任务(_nworking--), 如果此时所有的工作任务都已经停止(_nworking==0), 则返回true
   * 2. 如果stripes中的条纹数和used_nstripes相等且大于1, 则将全局条纹集中的条纹数减半 ?? TODO 为什么要减半, 或者说又回到之前的问题, 条纹数到底有什么含义 ??
   * 3. 进入等待状态
   * 4. 被唤醒后, 首先清理掉当前线程的唤醒状态标记(_nawakening大于0时减1)
   * - 标记条纹中插入标记栈时, 会调用wake_up()进行唤醒; leave()完全退出时也会唤醒
   * 5. 如果此时其他任务都已经停止, 则返回true
   * 6. 重新将当前线程进入工作状态(_nworking++), 并返回false
   * @param stripes 全局条纹集
   * @param used_nstripes ZMarkContext中在用的条纹数
   * @return _nworking归零时返回true
   */
  bool try_terminate(ZMarkStripeSet* stripes, size_t used_nstripes);
  void set_resurrected(bool value);
  bool resurrected() const;
};

#endif // SHARE_GC_Z_ZMARKTERMINATE_HPP
