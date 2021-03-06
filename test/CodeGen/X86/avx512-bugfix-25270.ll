; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=x86_64-apple-darwin -mcpu=knl | FileCheck %s

declare void @Print__512(<16 x i32>) #0

define void @bar__512(<16 x i32>* %var) #0 {
; CHECK-LABEL: bar__512:
; CHECK:       ## BB#0: ## %allocas
; CHECK-NEXT:    pushq %rbx
; CHECK-NEXT:    subq $112, %rsp
; CHECK-NEXT:    movq %rdi, %rbx
; CHECK-NEXT:    vmovdqu32 (%rbx), %zmm0
; CHECK-NEXT:    vmovdqu64 %zmm0, (%rsp) ## 64-byte Spill
; CHECK-NEXT:    vpbroadcastd {{.*}}(%rip), %zmm1
; CHECK-NEXT:    vmovdqa32 %zmm1, (%rbx)
; CHECK-NEXT:    callq _Print__512
; CHECK-NEXT:    vmovups (%rsp), %zmm0 ## 64-byte Reload
; CHECK-NEXT:    callq _Print__512
; CHECK-NEXT:    vpbroadcastd {{.*}}(%rip), %zmm0
; CHECK-NEXT:    vmovdqa32 %zmm0, (%rbx)
; CHECK-NEXT:    addq $112, %rsp
; CHECK-NEXT:    popq %rbx
; CHECK-NEXT:    retq
allocas:
  %var_load_load = load <16 x i32>, <16 x i32>* %var, align 1
  store <16 x i32> <i32 4, i32 4, i32 4, i32 4, i32 4, i32 4, i32 4, i32 4, i32 4, i32 4, i32 4, i32 4, i32 4, i32 4, i32 4, i32 4>, <16 x i32>* %var, align 64
  call void @Print__512(<16 x i32> %var_load_load)
 ; %var_load_load value should be reloaded
  call void @Print__512(<16 x i32> %var_load_load)
  store <16 x i32> <i32 7, i32 7, i32 7, i32 7, i32 7, i32 7, i32 7, i32 7, i32 7, i32 7, i32 7, i32 7, i32 7, i32 7, i32 7, i32 7>, <16 x i32>* %var, align 64
  ret void
}


attributes #0 = { nounwind }
