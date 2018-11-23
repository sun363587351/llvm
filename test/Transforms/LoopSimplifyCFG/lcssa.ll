; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt -S -loop-simplifycfg -verify-loop-info -verify-dom-info -verify-loop-lcssa < %s | FileCheck %s
; RUN: opt -S -passes='require<domtree>,loop(simplify-cfg)' -verify-loop-info -verify-dom-info -verify-loop-lcssa < %s | FileCheck %s
; RUN: opt -S -loop-simplifycfg -enable-mssa-loop-dependency=true -verify-memoryssa -verify-loop-info -verify-dom-info -verify-loop-lcssa < %s | FileCheck %s

target datalayout = "e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128"

define void @c() {
; CHECK-LABEL: @c(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[D:%.*]]
; CHECK:       d.loopexit:
; CHECK-NEXT:    [[DOTLCSSA:%.*]] = phi i32 [ [[TMP1:%.*]], [[FOR_COND:%.*]] ]
; CHECK-NEXT:    br label [[D]]
; CHECK:       d:
; CHECK-NEXT:    [[TMP0:%.*]] = phi i32 [ undef, [[ENTRY:%.*]] ], [ [[DOTLCSSA]], [[D_LOOPEXIT:%.*]] ]
; CHECK-NEXT:    br label [[FOR_COND]]
; CHECK:       for.cond:
; CHECK-NEXT:    [[TMP1]] = phi i32 [ [[TMP0]], [[D]] ], [ 0, [[IF_END:%.*]] ]
; CHECK-NEXT:    [[TOBOOL2:%.*]] = icmp eq i32 [[TMP1]], 0
; CHECK-NEXT:    br i1 [[TOBOOL2]], label [[IF_END]], label [[D_LOOPEXIT]]
; CHECK:       if.end:
; CHECK-NEXT:    br label [[FOR_COND]]
;
entry:
  br label %d

d.loopexit:                                       ; preds = %if.end.7, %for.body
  %.lcssa = phi i32 [ %1, %for.body ], [ 0, %if.end.7 ]
  br label %d

d:                                                ; preds = %d.loopexit, %entry
  %0 = phi i32 [ undef, %entry ], [ %.lcssa, %d.loopexit ]
  br label %for.cond

for.cond:                                         ; preds = %if.end.8, %d
  %1 = phi i32 [ %0, %d ], [ 0, %if.end.8 ]
  br label %for.body

for.body:                                         ; preds = %for.cond
  %tobool2 = icmp eq i32 %1, 0
  br i1 %tobool2, label %if.end, label %d.loopexit

if.end:                                           ; preds = %for.body
  br label %if.end.7

if.end.7:                                         ; preds = %if.end
  br i1 true, label %if.end.8, label %d.loopexit

if.end.8:                                         ; preds = %if.end.7
  br label %for.cond
}
