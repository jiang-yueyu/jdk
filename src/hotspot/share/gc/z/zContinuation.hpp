/*
 * Copyright (c) 2022, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_Z_ZCONTINUATION_HPP
#define SHARE_GC_Z_ZCONTINUATION_HPP

#include "memory/allStatic.hpp"
#include "memory/iterator.hpp"
#include "oops/oopsHierarchy.hpp"

class OopClosure;
class ZHeap;

class ZContinuation : public AllStatic {
public:
  /**
   * 指示chunk能否需要加读屏障, 用于处理协程栈的拷贝
   */
  static bool requires_barriers(const ZHeap* heap, stackChunkOop chunk);

  static oop load_oop(stackChunkOop chunk, void* addr);

  /**
   * 将地址转换为指针
   */
  class ZColorStackOopClosure : public OopClosure {
  private:
    uintptr_t _color;

  public:
    ZColorStackOopClosure(stackChunkOop chunk);
    virtual void do_oop(oop* p) override;
    virtual void do_oop(narrowOop* p) override;
  };

  /**
   * 将指针转换为地址
   */
  class ZUncolorStackOopClosure : public OopClosure {
  public:
    virtual void do_oop(oop* p) override;
    virtual void do_oop(narrowOop* p) override;
  };
};

#endif // SHARE_GC_Z_ZCONTINUATION_HPP
