// FUN_1400341a4 @ 1400341a4

void FUN_1400341a4(longlong param_1)

{
  short *psVar1;
  uint uVar2;
  longlong lVar3;
  longlong lVar4;
  longlong lVar5;
  ushort uVar6;
  int iVar7;
  ulonglong uVar8;
  uint *puVar9;
  uint uVar10;
  uint uVar11;
  ulonglong uVar12;
  ulonglong uVar13;
  uint uVar14;
  uint uVar15;
  uint uVar16;
  
  uVar12 = 0;
  uVar11 = 0;
  FUN_140057658(*(undefined8 *)(*(longlong *)(param_1 + 0x3c8) + 0x2d4));
  uVar16 = 1;
  if ((*(int *)(param_1 + 0x24) == 0x25) || (uVar14 = uVar11, *(int *)(param_1 + 0x24) == 0x50)) {
    uVar14 = 1;
  }
  if ((*(uint *)(param_1 + 0xc) & 0x80000) != 0) {
    *(undefined4 *)(param_1 + 0x24) = 0x19;
  }
  uVar8 = (ulonglong)DAT_1400f43e0;
  if ((DAT_1400f43e0 == 0) && (1 < DAT_140081628)) {
    puVar9 = &DAT_1400f3b68;
    uVar13 = (ulonglong)(DAT_140081628 - 1);
    do {
      if (*(code **)(puVar9 + 4) == FUN_1400341a4) {
        uVar8 = (ulonglong)*puVar9;
      }
      puVar9 = puVar9 + 0x18;
      uVar13 = uVar13 - 1;
    } while (uVar13 != 0);
    DAT_1400f43e0 = (uint)uVar8;
  }
  if ((((code *)(&DAT_1400f3b18)[uVar8 * 0xc] == FUN_1400341a4) &&
      ((&DAT_1400f3b10)[uVar8 * 0x18] != 0)) &&
     (((&DAT_1400f3b14)[uVar8 * 0x18] == (uint)*(ushort *)(param_1 + 4) ||
      ((&DAT_1400f3b14)[uVar8 * 0x18] == 0xffff)))) {
    *(undefined4 *)(param_1 + 0x24) = 0x19;
    (&DAT_1400f3b10)[uVar8 * 0x18] = (&DAT_1400f3b10)[uVar8 * 0x18] + -1;
  }
  if (*(int *)(param_1 + 0x24) == 1) goto LAB_1400346a1;
  lVar3 = *(longlong *)(param_1 + 0x2ec);
  if (((*(uint *)(param_1 + 0xc) & 0x80000) == 0) && (*(char *)(param_1 + 0x294) == '\0')) {
    (**(code **)(DAT_1400f6bf8 + 0xac))
              ("FAIL at RC_GenericIoFinishIoUnbuffered() dev:%d blk:%d size:%d\n",
               *(undefined2 *)(param_1 + 4),*(undefined4 *)(param_1 + 0x264),
               *(undefined4 *)(param_1 + 0x20));
    if (uVar14 == 1) {
      if (*(int *)(lVar3 + 0x13c) - 0x2bf0U < 2) goto LAB_1400343dd;
      if ((*(uint *)(param_1 + 0xc) & 1) != 0) {
        uVar6 = *(ushort *)(param_1 + 0x25c) & 0x7fff;
        if (DAT_1400f6bf8 == 0) {
          FUN_14004c838(0x420,uVar6,0,0,0,0,0,0,0);
        }
        else {
          FUN_14004c4c0(*(undefined4 *)(DAT_1400f6bf8 + 0x15c),0x420,uVar6,0,0,0,0,0,0,0);
        }
        uVar8 = *(ulonglong *)(param_1 + 0x264) >> 0x20;
        if (DAT_1400f6bf8 == 0) {
          FUN_14004c838(0x41c,*(undefined4 *)(param_1 + 0x264),uVar8,0,0,0,0,0,0);
        }
        else {
          FUN_14004c4c0(*(undefined4 *)(DAT_1400f6bf8 + 0x15c),0x41c,
                        *(undefined4 *)(param_1 + 0x264),uVar8,0,0,0,0,0,0);
        }
      }
    }
    FUN_140021a50(param_1);
  }
LAB_1400343dd:
  if ((*(uint *)(param_1 + 0xc) & 0x80001) == 1) {
    uVar15 = *(uint *)(param_1 + 600) & 0xff;
    if (*(int *)(param_1 + 0x24) == 0x50) {
      uVar6 = *(ushort *)(param_1 + 0x3bc);
      if (uVar15 < uVar6) {
        uVar10 = uVar6 - uVar15;
      }
      else {
        uVar10 = (uint)uVar6 + (*(int *)(lVar3 + 0xb8) - uVar15);
      }
    }
    else {
      *(short *)(param_1 + 0x3bc) = (short)uVar15;
      uVar10 = *(uint *)(lVar3 + 0xb8);
    }
    if (*(int *)(lVar3 + 0x13c) == 0x2bf1) {
      if (1 < uVar10) {
        do {
          uVar2 = *(uint *)(param_1 + 0x18);
          uVar15 = (uVar15 + 1) % *(uint *)(lVar3 + 0xb8);
          *(undefined1 *)(param_1 + 600) = 0;
          *(uint *)(param_1 + 600) = *(uint *)(param_1 + 600) | uVar15;
          iVar7 = (**(code **)(lVar3 + 0x158))(param_1);
          uVar8 = (*(ulonglong *)(param_1 + 0x18) & 0xffffffffffffffe0) + (ulonglong)(uVar2 & 0x1f);
          *(ulonglong *)(param_1 + 0x18) = uVar8;
          if (iVar7 == 0) {
            if (((*(uint *)(param_1 + 0xc) & 1) != 0) && (uVar14 == 1)) {
              (**(code **)(DAT_1400f6bf8 + 0xac))
                        ("RC_GenericIoTransferBufferedReadData(READ) REMAPPED media error dev:%d blk:%d size:%d\n"
                         ,*(undefined2 *)(param_1 + 4),uVar8 & 0xffffffff,
                         *(undefined4 *)(param_1 + 0x20));
              *(uint *)(param_1 + 0xc) = *(uint *)(param_1 + 0xc) | 0x1000;
              *(undefined8 *)(param_1 + 0x2fc) = *(undefined8 *)(param_1 + 0x28);
              *(code **)(param_1 + 0x28) = FUN_140033718;
            }
            FUN_140031f00(param_1);
            return;
          }
          if (uVar16 == uVar10 - 1) goto LAB_140034503;
          uVar16 = uVar16 + 1;
        } while (uVar16 < uVar10);
      }
      if (((*(uint *)(param_1 + 0xc) & 1) != 0) && (uVar14 == 1)) {
        uVar6 = *(ushort *)(param_1 + 0x25c) & 0x7fff;
        if (DAT_1400f6bf8 == 0) {
          FUN_14004c838(0x420,uVar6,0,0,0,0,0,0,0);
        }
        else {
          FUN_14004c4c0(*(undefined4 *)(DAT_1400f6bf8 + 0x15c),0x420,uVar6,0,0,0,0,0,0,0);
        }
        uVar8 = *(ulonglong *)(param_1 + 0x264) >> 0x20;
        if (DAT_1400f6bf8 == 0) {
          FUN_14004c838(0x41c,*(undefined4 *)(param_1 + 0x264),uVar8,0,0,0,0,0,0);
        }
        else {
          FUN_14004c4c0(*(undefined4 *)(DAT_1400f6bf8 + 0x15c),0x41c,
                        *(undefined4 *)(param_1 + 0x264),uVar8,0,0,0,0,0,0);
        }
      }
    }
  }
  lVar3 = *(longlong *)(param_1 + 0x2ec);
  if (*(int *)(lVar3 + 0x13c) == 0x2bf0) {
    *(code **)(param_1 + 0x28) = FUN_140034d20;
    *(undefined4 *)(param_1 + 0x20) = *(undefined4 *)(param_1 + 0x240);
    *(undefined8 *)(param_1 + 0x30) = *(undefined8 *)(param_1 + 0x26c);
    *(undefined4 *)(param_1 + 0x38) = *(undefined4 *)(param_1 + 0x274);
    *(undefined4 *)(param_1 + 0x3c) = *(undefined4 *)(param_1 + 0x278);
    *(uint *)(param_1 + 0x284) = *(uint *)(param_1 + 0x284) & 0xfffffffd | 1;
    (**(code **)(lVar3 + 0x158))(param_1);
    if (uVar14 != 1) {
      *(uint *)(param_1 + 0xc) = *(uint *)(param_1 + 0xc) | 0x80000;
    }
    FUN_140058520(param_1);
    return;
  }
  if ((((*(int *)(lVar3 + 0x13c) != 0x2bf1) && (*(int *)(param_1 + 0x24) != 0x50)) &&
      (*(longlong *)(lVar3 + 0x198) != 0)) && (*(int *)(*(longlong *)(lVar3 + 0x198) + 0x1798) == 1)
     ) {
LAB_140034503:
    *(undefined8 *)(param_1 + 0x2fc) = *(undefined8 *)(param_1 + 0x28);
    FUN_1400376d0(param_1);
    return;
  }
LAB_1400346a1:
  lVar3 = *(longlong *)(param_1 + 0x3c8);
  psVar1 = (short *)(lVar3 + 0x23e);
  *psVar1 = *psVar1 + 1;
  lVar4 = *(longlong *)(param_1 + 0x3c8);
  if (*(short *)(lVar3 + 0x23e) == *(short *)(lVar4 + 0x23c)) {
    lVar3 = *(longlong *)(lVar4 + 0x2d4);
    lVar5 = *(longlong *)(lVar4 + 0x234);
    if (((*(longlong *)(lVar3 + 0x40) == 0) && (*(longlong *)(lVar3 + 0x30) == lVar4)) &&
       (*(longlong *)(*(longlong *)(lVar3 + 0x30) + 0x2c4) == 0)) {
      *(undefined8 *)(lVar3 + 0x30) = 0;
      *(undefined8 *)(lVar4 + 0x2c4) = 0;
      if ((DAT_1400eee90 == 0) && ((*(uint *)(lVar4 + 600) & 0xd8a00) == 0)) {
        FUN_1400139dc(lVar3);
      }
      else {
        *(undefined8 *)(lVar4 + 0x2d4) = 0;
        FUN_140057658(lVar3);
        FUN_1400147ac(lVar3);
        FUN_1400126bc(lVar3);
      }
    }
    else {
      FUN_140012584(lVar4);
    }
    FUN_140015bd4(lVar4);
    *(short *)(lVar5 + 0x23e) = *(short *)(lVar5 + 0x23e) + 1;
    uVar16 = *(uint *)(lVar4 + 0x3c4);
    if (uVar16 != 0) {
      do {
        if (*(int *)(*(longlong *)(lVar4 + 0x3c8 + uVar12 * 8) + 0x24) == 1) goto LAB_140034794;
        uVar11 = (int)uVar12 + 1;
        uVar12 = (ulonglong)uVar11;
      } while (uVar11 < uVar16);
    }
    if (uVar11 == uVar16) {
      *(uint *)(lVar5 + 0x24) = (-(uint)((*(byte *)(lVar4 + 0xc) & 1) != 0) & 0xc) + 0x19;
    }
LAB_140034794:
    if (*(short *)(lVar5 + 0x23e) == *(short *)(lVar5 + 0x23c)) {
      FUN_140058520(lVar5);
    }
    FUN_1400336b4(lVar4);
  }
  return;
}

