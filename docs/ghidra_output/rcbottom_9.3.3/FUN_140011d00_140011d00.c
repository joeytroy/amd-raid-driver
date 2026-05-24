// FUN_140011d00 @ 140011d00

longlong * FUN_140011d00(longlong *param_1,byte param_2,undefined1 *param_3)

{
  longlong *plVar1;
  longlong *plVar2;
  longlong *plVar3;
  undefined4 uVar4;
  longlong lVar5;
  undefined1 *puVar6;
  ulonglong uVar7;
  undefined1 *puVar8;
  longlong *plVar9;
  longlong extraout_XMM0_Qa;
  longlong lVar10;
  longlong extraout_XMM0_Qb;
  
  plVar1 = param_1;
  lVar5 = (ulonglong)param_2 * 0x101010101010101;
  if (param_3 < (undefined1 *)0x40) {
    if (param_3 < (undefined1 *)0x10) {
      if ((undefined1 *)0x3 < param_3) {
        uVar4 = (undefined4)lVar5;
        *(undefined4 *)param_1 = uVar4;
        uVar7 = ((ulonglong)param_3 & 8) >> 1;
        *(undefined4 *)(param_3 + -4 + (longlong)param_1) = uVar4;
        *(undefined4 *)((longlong)param_1 + uVar7) = uVar4;
        *(undefined4 *)((longlong)(param_3 + -4 + (longlong)param_1) - uVar7) = uVar4;
        return plVar1;
      }
      if (param_3 != (undefined1 *)0x0) {
        *(char *)param_1 = (char)lVar5;
        if (param_3 != (undefined1 *)0x1) {
          *(short *)((longlong)param_1 + (longlong)(param_3 + -2)) = (short)lVar5;
        }
      }
      return plVar1;
    }
  }
  else {
    if (((DAT_1400151c0 & 2) != 0) && ((undefined1 *)0x31f < param_3)) {
      lVar10 = lVar5;
      if ((DAT_1400151c0 & 1) == 0) {
        param_1 = (longlong *)FUN_140011ec0();
        lVar5 = extraout_XMM0_Qa;
        lVar10 = extraout_XMM0_Qb;
      }
      *plVar1 = lVar5;
      plVar1[1] = lVar10;
      plVar1[2] = lVar5;
      plVar1[3] = lVar10;
      plVar1[4] = lVar5;
      plVar1[5] = lVar10;
      plVar1[6] = lVar5;
      plVar1[7] = lVar10;
      puVar6 = (undefined1 *)((ulonglong)(plVar1 + 8) & 0xffffffffffffffc0);
      for (puVar8 = (undefined1 *)
                    ((longlong)plVar1 +
                    ((longlong)param_3 - (longlong)((ulonglong)(plVar1 + 8) & 0xffffffffffffffc0)));
          puVar8 != (undefined1 *)0x0; puVar8 = puVar8 + -1) {
        *puVar6 = (char)lVar5;
        puVar6 = puVar6 + 1;
      }
      return param_1;
    }
    *param_1 = lVar5;
    param_1[1] = lVar5;
    plVar2 = (longlong *)((ulonglong)(param_1 + 2) & 0xfffffffffffffff0);
    param_3 = (undefined1 *)((longlong)param_1 + ((longlong)param_3 - (longlong)plVar2));
    param_1 = plVar2;
    if ((undefined1 *)0x3f < param_3) {
      plVar9 = (longlong *)
               ((ulonglong)((longlong)plVar2 + (longlong)(param_3 + -0x30)) & 0xfffffffffffffff0);
      uVar7 = (ulonglong)param_3 >> 6;
      plVar3 = plVar2;
      do {
        *plVar3 = lVar5;
        plVar3[1] = lVar5;
        plVar3[2] = lVar5;
        plVar3[3] = lVar5;
        uVar7 = uVar7 - 1;
        plVar3[4] = lVar5;
        plVar3[5] = lVar5;
        plVar3[6] = lVar5;
        plVar3[7] = lVar5;
        plVar3 = plVar3 + 8;
      } while (uVar7 != 0);
      *plVar9 = lVar5;
      plVar9[1] = lVar5;
      plVar9[2] = lVar5;
      plVar9[3] = lVar5;
      plVar9[4] = lVar5;
      plVar9[5] = lVar5;
      *(longlong *)((longlong)plVar2 + (longlong)(param_3 + -0x10)) = lVar5;
      ((longlong *)((longlong)plVar2 + (longlong)(param_3 + -0x10)))[1] = lVar5;
      return plVar1;
    }
  }
  plVar2 = (longlong *)(param_3 + -0x10 + (longlong)param_1);
  *param_1 = lVar5;
  param_1[1] = lVar5;
  uVar7 = ((ulonglong)param_3 & 0x20) >> 1;
  *plVar2 = lVar5;
  plVar2[1] = lVar5;
  param_1 = (longlong *)((longlong)param_1 + uVar7);
  *param_1 = lVar5;
  param_1[1] = lVar5;
  plVar2 = (longlong *)((longlong)plVar2 - uVar7);
  *plVar2 = lVar5;
  plVar2[1] = lVar5;
  return plVar1;
}

