// FUN_140035364 @ 140035364

void FUN_140035364(longlong param_1)

{
  longlong lVar1;
  bool bVar2;
  ushort uVar3;
  undefined2 uVar4;
  int iVar5;
  uint *puVar6;
  uint uVar7;
  uint uVar8;
  ulonglong uVar9;
  uint uVar10;
  
  bVar2 = false;
  if ((DAT_1400f43ec == 0) && (1 < DAT_140081628)) {
    puVar6 = &DAT_1400f3b68;
    uVar9 = (ulonglong)(DAT_140081628 - 1);
    do {
      if (*(code **)(puVar6 + 4) == FUN_140035364) {
        DAT_1400f43ec = *puVar6;
      }
      puVar6 = puVar6 + 0x18;
      uVar9 = uVar9 - 1;
    } while (uVar9 != 0);
  }
  uVar9 = (ulonglong)DAT_1400f43ec;
  if ((((code *)(&DAT_1400f3b18)[uVar9 * 0xc] == FUN_140035364) &&
      ((&DAT_1400f3b10)[uVar9 * 0x18] != 0)) &&
     (((&DAT_1400f3b14)[uVar9 * 0x18] == (uint)*(ushort *)(param_1 + 4) ||
      ((&DAT_1400f3b14)[uVar9 * 0x18] == 0xffff)))) {
    *(undefined4 *)(param_1 + 0x24) = 0x19;
    (&DAT_1400f3b10)[uVar9 * 0x18] = (&DAT_1400f3b10)[uVar9 * 0x18] + -1;
  }
  iVar5 = *(int *)(param_1 + 0x24);
  if ((iVar5 == 0x25) || (iVar5 == 0x50)) {
    bVar2 = true;
  }
  if ((iVar5 != 1) && (*(char *)(param_1 + 0x294) == '\0')) {
    lVar1 = *(longlong *)(param_1 + 0x2ec);
    if ((*(uint *)(param_1 + 0xc) & 0x80000) == 0) {
      (**(code **)(DAT_1400f6bf8 + 0xac))
                ("FAIL at RC_GenericIoMergeDirtyData(ioc:0x%08x) dev:%d blk:%d size:%d\n",param_1,
                 *(undefined2 *)(param_1 + 4),*(undefined4 *)(param_1 + 0x264),
                 *(undefined4 *)(param_1 + 0x20));
      if (((bVar2) && (*(int *)(lVar1 + 0x13c) - 0x2bf0U < 2)) || (FUN_140021a50(param_1), bVar2)) {
        (**(code **)(DAT_1400f6bf8 + 0xac))
                  ("RC_GenericIoMergeDirtyData(READ) media error dev:%d blk:%d size:%d\n",
                   *(undefined2 *)(param_1 + 4),*(undefined4 *)(param_1 + 0x18),
                   *(undefined4 *)(param_1 + 0x20));
      }
      if (*(int *)(lVar1 + 0x13c) == 0x2bf1) {
        uVar7 = *(uint *)(param_1 + 600) & 0xff;
        if (*(int *)(param_1 + 0x24) == 0x50) {
          uVar3 = *(ushort *)(param_1 + 0x3bc);
          if (uVar7 < uVar3) {
            uVar8 = uVar3 - uVar7;
          }
          else {
            uVar8 = (uint)uVar3 + (*(int *)(lVar1 + 0xb8) - uVar7);
          }
        }
        else {
          *(short *)(param_1 + 0x3bc) = (short)uVar7;
          uVar8 = *(uint *)(lVar1 + 0xb8);
        }
        uVar10 = 1;
        if (1 < uVar8) {
          while( true ) {
            uVar7 = (uVar7 + 1) % *(uint *)(lVar1 + 0xb8);
            *(undefined1 *)(param_1 + 600) = 0;
            *(uint *)(param_1 + 600) = *(uint *)(param_1 + 600) | uVar7;
            iVar5 = (**(code **)(lVar1 + 0x158))(param_1);
            *(ulonglong *)(param_1 + 0x18) = *(ulonglong *)(param_1 + 0x18) & 0xffffffffffffffe0;
            if (iVar5 != 0) break;
            uVar10 = uVar10 + 1;
            if (uVar8 <= uVar10) {
              if (bVar2) {
                (**(code **)(DAT_1400f6bf8 + 0xac))
                          ("RC_GenericIoMergeDirtyData(READ) REMAPPED media error dev:%d blk:%d size:%d\n"
                           ,*(undefined2 *)(param_1 + 4),*(undefined4 *)(param_1 + 0x18),
                           *(undefined4 *)(param_1 + 0x20));
                *(uint *)(param_1 + 0xc) = *(uint *)(param_1 + 0xc) | 0x1000;
                *(undefined8 *)(param_1 + 0x2fc) = *(undefined8 *)(param_1 + 0x28);
                *(code **)(param_1 + 0x28) = FUN_140033718;
              }
              FUN_140031f00(param_1);
              return;
            }
          }
          if (uVar10 == uVar8 - 1) goto LAB_140035598;
        }
        if (bVar2) {
          uVar3 = *(ushort *)(param_1 + 0x25c) & 0x7fff;
          if (DAT_1400f6bf8 == 0) {
            FUN_14004c838(0x420,uVar3,0,0,0,0,0,0,0);
          }
          else {
            FUN_14004c4c0(*(undefined4 *)(DAT_1400f6bf8 + 0x15c),0x420,uVar3,0,0,0,0,0,0,0);
          }
          uVar9 = *(ulonglong *)(param_1 + 0x264) >> 0x20;
          if (DAT_1400f6bf8 == 0) {
            FUN_14004c838(0x41c,*(undefined4 *)(param_1 + 0x264),uVar9,0,0,0,0,0,0);
          }
          else {
            FUN_14004c4c0(*(undefined4 *)(DAT_1400f6bf8 + 0x15c),0x41c,
                          *(undefined4 *)(param_1 + 0x264),uVar9,0,0,0,0,0,0);
          }
        }
      }
    }
    if (*(char *)(param_1 + 0x294) == '\0') {
      iVar5 = *(int *)(*(longlong *)(param_1 + 0x2ec) + 0x13c);
      if (iVar5 == 0x2bf0) {
        (**(code **)(DAT_1400f6bf8 + 0xac))
                  ("RC_GenericIoMergeDirtyData(READ) REMAPPED media error dev:%d blk:%d size:%d\n",
                   *(undefined2 *)(param_1 + 4),*(undefined4 *)(param_1 + 0x18),
                   *(undefined4 *)(param_1 + 0x20));
        if (*(int *)(*(longlong *)(param_1 + 0x2ec) + 4) == 0x1bfa) {
          uVar4 = FUN_1400145dc();
        }
        else {
          uVar4 = FUN_140014584(param_1);
        }
        *(undefined2 *)(param_1 + 0x3bc) = uVar4;
        FUN_140051584(param_1);
        return;
      }
      if (((iVar5 != 0x2bf1) && (*(int *)(param_1 + 0x24) != 0x50)) &&
         ((lVar1 = *(longlong *)(*(longlong *)(param_1 + 0x2ec) + 0x198), lVar1 != 0 &&
          (*(int *)(lVar1 + 0x1798) == 1)))) {
LAB_140035598:
        *(undefined8 *)(param_1 + 0x2fc) = *(undefined8 *)(param_1 + 0x28);
        FUN_1400376d0(param_1);
        return;
      }
    }
    *(undefined1 *)(param_1 + 0x294) = 1;
  }
  *(code **)(param_1 + 0x28) = FUN_140035e60;
  FUN_140014128(param_1);
  return;
}

