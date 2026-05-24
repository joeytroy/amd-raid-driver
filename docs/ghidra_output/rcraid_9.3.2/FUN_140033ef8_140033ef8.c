// FUN_140033ef8 @ 140033ef8

void FUN_140033ef8(longlong param_1)

{
  uint uVar1;
  longlong lVar2;
  longlong lVar3;
  uint *puVar4;
  ulonglong uVar5;
  longlong *plVar6;
  short sVar7;
  uint uVar8;
  ulonglong uVar9;
  
  sVar7 = *(short *)(*(longlong *)(param_1 + 0x3c8) + 0x23e) + 1;
  *(short *)(*(longlong *)(param_1 + 0x3c8) + 0x23e) = sVar7;
  if ((*(uint *)(param_1 + 0xc) >> 0x13 & 1) != 0) {
    *(undefined4 *)(param_1 + 0x24) = 0x19;
  }
  uVar5 = (ulonglong)DAT_1400f43dc;
  uVar8 = 0;
  if ((DAT_1400f43dc == 0) && (1 < DAT_140081628)) {
    puVar4 = &DAT_1400f3b68;
    uVar9 = (ulonglong)(DAT_140081628 - 1);
    do {
      if (*(code **)(puVar4 + 4) == FUN_140033ef8) {
        uVar5 = (ulonglong)*puVar4;
      }
      puVar4 = puVar4 + 0x18;
      uVar9 = uVar9 - 1;
    } while (uVar9 != 0);
    DAT_1400f43dc = (uint)uVar5;
  }
  if ((((code *)(&DAT_1400f3b18)[uVar5 * 0xc] == FUN_140033ef8) &&
      ((&DAT_1400f3b10)[uVar5 * 0x18] != 0)) &&
     (((&DAT_1400f3b14)[uVar5 * 0x18] == (uint)*(ushort *)(param_1 + 4) ||
      ((&DAT_1400f3b14)[uVar5 * 0x18] == 0xffff)))) {
    *(undefined4 *)(param_1 + 0x24) = 0x19;
    (&DAT_1400f3b10)[uVar5 * 0x18] = (&DAT_1400f3b10)[uVar5 * 0x18] + -1;
  }
  if (*(int *)(param_1 + 0x24) != 1) {
    if (*(int *)(param_1 + 0x24) == 0x39) {
      *(undefined4 *)(*(longlong *)(param_1 + 0x234) + 0x24) = 0x39;
    }
    else if ((*(uint *)(param_1 + 0xc) >> 0x13 & 1) == 0) {
      (**(code **)(DAT_1400f6bf8 + 0xac))
                ("FAIL at RC_GenericIoFinishIoBufferedWrite() dev:%d blk:%d size:%d\n",
                 *(undefined2 *)(param_1 + 4),*(undefined4 *)(param_1 + 0x264),
                 *(undefined4 *)(param_1 + 0x20));
      FUN_140021a50(param_1);
    }
  }
  lVar2 = *(longlong *)(param_1 + 0x3c8);
  if (sVar7 == *(short *)(lVar2 + 0x23c)) {
    FUN_140015278(lVar2);
    FUN_1400672dc(lVar2,0,0,0,0,1);
    FUN_140015bd4(lVar2);
    lVar3 = *(longlong *)(lVar2 + 0x234);
    *(short *)(lVar3 + 0x23e) = *(short *)(lVar3 + 0x23e) + 1;
    uVar1 = *(uint *)(lVar2 + 0x3c4);
    if (uVar1 != 0) {
      plVar6 = (longlong *)(lVar2 + 0x3c8);
      do {
        if (*(int *)(*plVar6 + 0x24) == 1) goto LAB_140034095;
        uVar8 = uVar8 + 1;
        plVar6 = plVar6 + 1;
      } while (uVar8 < uVar1);
    }
    if ((uVar8 == uVar1) && (*(int *)(lVar3 + 0x24) != 0x39)) {
      *(undefined4 *)(lVar3 + 0x24) = 0x19;
    }
LAB_140034095:
    if (*(short *)(lVar3 + 0x23e) == *(short *)(lVar3 + 0x23c)) {
      FUN_140058520();
    }
    FUN_1400336b4(lVar2);
  }
  return;
}

