// FUN_140035e60 @ 140035e60

void FUN_140035e60(longlong param_1)

{
  ulonglong *puVar1;
  bool bVar2;
  ushort uVar3;
  undefined2 uVar4;
  int iVar5;
  ulonglong uVar6;
  uint uVar7;
  uint uVar8;
  uint uVar9;
  uint uVar10;
  uint uVar11;
  undefined8 in_stack_ffffffffffffffa8;
  longlong lVar12;
  
  iVar5 = *(int *)(param_1 + 0x24);
  uVar10 = 1;
  if ((iVar5 == 0x25) || (bVar2 = false, iVar5 == 0x50)) {
    bVar2 = true;
  }
  uVar7 = 0;
  if (iVar5 != 1) {
    lVar12 = *(longlong *)(param_1 + 0x2ec);
    if (((*(uint *)(param_1 + 0xc) & 0x80000) == 0) && (*(char *)(param_1 + 0x294) == '\0')) {
      (**(code **)(DAT_1400f6bf8 + 0xac))
                ("FAIL at RC_GenericIoTransferBufferedReadData() dev:%d blk:%d size:%d\n",
                 *(undefined2 *)(param_1 + 4),*(undefined4 *)(param_1 + 0x264),
                 *(undefined4 *)(param_1 + 0x20));
      if (((bVar2) && (*(int *)(lVar12 + 0x13c) == 0x2bf0 || *(int *)(lVar12 + 0x13c) == 0x2bf1)) ||
         (FUN_140021a50(param_1), bVar2)) {
        (**(code **)(DAT_1400f6bf8 + 0xac))
                  ("RC_GenericIoTransferBufferedReadData(READ) media error dev:%d blk:%d size:%d\n",
                   *(undefined2 *)(param_1 + 4),*(undefined4 *)(param_1 + 0x18),
                   *(undefined4 *)(param_1 + 0x20));
      }
      uVar7 = (uint)((ulonglong)in_stack_ffffffffffffffa8 >> 0x20);
      if (*(int *)(lVar12 + 0x13c) == 0x2bf1) {
        uVar9 = *(uint *)(param_1 + 600) & 0xff;
        if (*(int *)(param_1 + 0x24) == 0x50) {
          uVar3 = *(ushort *)(param_1 + 0x3bc);
          if (uVar9 < uVar3) {
            uVar8 = uVar3 - uVar9;
          }
          else {
            uVar8 = (uint)uVar3 + (*(int *)(lVar12 + 0xb8) - uVar9);
          }
        }
        else {
          *(short *)(param_1 + 0x3bc) = (short)uVar9;
          uVar8 = *(uint *)(lVar12 + 0xb8);
        }
        uVar11 = uVar10;
        if (1 < uVar8) {
          while( true ) {
            uVar9 = (uVar9 + 1) % *(uint *)(lVar12 + 0xb8);
            *(undefined1 *)(param_1 + 600) = 0;
            *(uint *)(param_1 + 600) = *(uint *)(param_1 + 600) | uVar9;
            iVar5 = (**(code **)(lVar12 + 0x158))(param_1);
            uVar7 = (uint)((ulonglong)in_stack_ffffffffffffffa8 >> 0x20);
            if (iVar5 != 0) break;
            uVar11 = uVar11 + 1;
            if (uVar8 <= uVar11) {
              if (bVar2) {
                *(uint *)(param_1 + 0xc) = *(uint *)(param_1 + 0xc) | 0x1000;
                *(undefined8 *)(param_1 + 0x2fc) = *(undefined8 *)(param_1 + 0x28);
                *(code **)(param_1 + 0x28) = FUN_140033718;
              }
              iVar5 = FUN_140013ea0(*(undefined8 *)(param_1 + 0x2d4));
              if (iVar5 != 0) {
                *(code **)(param_1 + 0x28) = FUN_140035364;
              }
              FUN_140031f00(param_1);
              return;
            }
          }
          if (uVar11 == uVar8 - 1) goto LAB_140036008;
          *(ulonglong *)(param_1 + 0x18) = *(ulonglong *)(param_1 + 0x18) & 0xffffffffffffffe0;
        }
        if (bVar2) {
          uVar3 = *(ushort *)(param_1 + 0x25c) & 0x7fff;
          if (DAT_1400f6bf8 == 0) {
            lVar12 = (ulonglong)uVar7 << 0x20;
            FUN_14004c838(0x420,uVar3,0,0,lVar12,0,0,0,0);
            uVar7 = (uint)((ulonglong)lVar12 >> 0x20);
          }
          else {
            lVar12 = (ulonglong)uVar7 << 0x20;
            FUN_14004c4c0(*(undefined4 *)(DAT_1400f6bf8 + 0x15c),0x420,uVar3,0,lVar12,0,0,0,0,0);
            uVar7 = (uint)((ulonglong)lVar12 >> 0x20);
          }
          uVar6 = *(ulonglong *)(param_1 + 0x264) >> 0x20;
          if (DAT_1400f6bf8 == 0) {
            FUN_14004c838(0x41c,*(undefined4 *)(param_1 + 0x264),uVar6,0,(ulonglong)uVar7 << 0x20,0,
                          0,0,0);
          }
          else {
            FUN_14004c4c0(*(undefined4 *)(DAT_1400f6bf8 + 0x15c),0x41c,
                          *(undefined4 *)(param_1 + 0x264),uVar6,(ulonglong)uVar7 << 0x20,0,0,0,0,0)
            ;
          }
        }
      }
    }
    uVar9 = 1;
    uVar7 = uVar9;
    if (*(char *)(param_1 + 0x294) == '\0') {
      lVar12 = *(longlong *)(param_1 + 0x2ec);
      if (*(int *)(lVar12 + 0x13c) == 0x2bf0) {
        if (*(int *)(lVar12 + 4) == 0x1bfa) {
          uVar4 = FUN_1400145dc();
        }
        else {
          uVar4 = FUN_140014584(param_1);
        }
        *(undefined2 *)(param_1 + 0x3bc) = uVar4;
        if (bVar2) {
          *(uint *)(param_1 + 0xc) = *(uint *)(param_1 + 0xc) | 0x1000;
        }
        FUN_140051584(param_1);
        return;
      }
      uVar7 = uVar10;
      if ((((*(int *)(lVar12 + 0x13c) != 0x2bf1) && (*(int *)(param_1 + 0x24) != 0x50)) &&
          (*(longlong *)(lVar12 + 0x198) != 0)) &&
         (uVar7 = uVar9, *(int *)(*(longlong *)(lVar12 + 0x198) + 0x1798) == 1)) {
LAB_140036008:
        *(undefined8 *)(param_1 + 0x2fc) = *(undefined8 *)(param_1 + 0x28);
        FUN_1400376d0(param_1);
        return;
      }
    }
    else if (*(int *)(param_1 + 0x24) == 0x50) {
      *(undefined4 *)(param_1 + 0x24) = 0x25;
    }
  }
  *(code **)(param_1 + 0x28) = FUN_140033e34;
  *(undefined8 *)(param_1 + 0x30) = *(undefined8 *)(param_1 + 0x26c);
  *(undefined4 *)(param_1 + 0x38) = *(undefined4 *)(param_1 + 0x274);
  *(undefined4 *)(param_1 + 0x3c) = *(undefined4 *)(param_1 + 0x278);
  if (uVar7 == 0) {
LAB_140036254:
    DAT_1400f83d0 = DAT_1400f83d0 + 1;
    if (DAT_1400f83d4 < DAT_1400f83d0) {
      DAT_1400f83d4 = DAT_1400f83d0;
    }
    if ((*(uint *)(param_1 + 0xc) & 0x40000) == 0) {
      iVar5 = *(int *)(param_1 + 0x20);
      *(undefined4 *)(param_1 + 0x20) = *(undefined4 *)(param_1 + 0x240);
      FUN_14003750c(param_1);
      uVar10 = *(uint *)(param_1 + 0x264);
      *(uint *)(param_1 + 0x20) = ((uVar10 & 0xffffffe0) - uVar10) + iVar5;
      FUN_140067a08(param_1,*(undefined8 *)(param_1 + 700),*(undefined4 *)(param_1 + 0x38),
                    *(undefined4 *)(param_1 + 0x3c),param_1 + 0x40,0,(uVar10 & 0x1f) << 9,
                    *(undefined4 *)(param_1 + 0x240));
      return;
    }
  }
  else {
    bVar2 = false;
    if (*(int *)(*(longlong *)(param_1 + 0x2ec) + 0x13c) == 0x2bf0) {
      puVar1 = *(ulonglong **)(*(longlong *)(param_1 + 0x2ec) + 0x198);
      if ((puVar1 != (ulonglong *)0x0) && ((int)puVar1[0x2f3] == 1)) {
        if ((*puVar1 <= *(ulonglong *)(param_1 + 0x264)) &&
           (bVar2 = false, *(ulonglong *)(param_1 + 0x264) < (ulonglong)(uint)puVar1[1] + *puVar1))
        {
          bVar2 = true;
        }
      }
      *(undefined4 *)(*(longlong *)(param_1 + 0x234) + 0x24) = *(undefined4 *)(param_1 + 0x24);
      if (bVar2) goto LAB_140036254;
    }
    DAT_1400f83d0 = DAT_1400f83d0 + 1;
    if (DAT_1400f83d4 < DAT_1400f83d0) {
      DAT_1400f83d4 = DAT_1400f83d0;
    }
  }
  FUN_140033e34(param_1);
  return;
}

