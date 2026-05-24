// FUN_140051334 @ 140051334

void FUN_140051334(longlong param_1)

{
  uint uVar1;
  int *piVar2;
  longlong lVar3;
  undefined8 uVar4;
  bool bVar5;
  int iVar6;
  longlong lVar7;
  undefined8 *puVar8;
  ulonglong uVar9;
  longlong *plVar10;
  ulonglong uVar11;
  ulonglong uVar12;
  uint uVar13;
  longlong lVar14;
  ulonglong uVar15;
  bool bVar16;
  
  piVar2 = *(int **)(param_1 + 0x2ec);
  uVar12 = 0;
  bVar16 = false;
  uVar1 = piVar2[0x2e];
  uVar11 = (ulonglong)uVar1;
  if (*piVar2 == 0x201) {
    bVar16 = true;
    lVar7 = FUN_14001d9b0();
    uVar9 = uVar12;
    if (piVar2[0x2d] != 0) {
      do {
        if (piVar2[0x2e] != 0) {
          lVar3 = *(longlong *)(*(longlong *)(piVar2 + 0x76) + uVar9 * 8);
          uVar4 = *(undefined8 *)(lVar7 + 8);
          uVar15 = uVar12;
          do {
            lVar14 = uVar15 * 0x44;
            if ((((short)uVar4 == *(short *)(lVar14 + 8 + lVar3)) &&
                ((short)((ulonglong)uVar4 >> 0x20) == *(short *)(lVar14 + 0xc + lVar3))) &&
               ((short)((ulonglong)uVar4 >> 0x10) == *(short *)(lVar14 + 10 + lVar3)))
            goto LAB_1400513ef;
            uVar13 = (int)uVar15 + 1;
            uVar15 = (ulonglong)uVar13;
          } while (uVar13 < (uint)piVar2[0x2e]);
        }
        uVar13 = (int)uVar9 + 1;
        uVar9 = (ulonglong)uVar13;
      } while (uVar13 < (uint)piVar2[0x2d]);
    }
LAB_1400513ef:
    if (piVar2[0x2e] != 0) {
      uVar15 = (ulonglong)(uint)piVar2[0x2e];
      plVar10 = *(longlong **)(*(longlong *)(piVar2 + 0x76) + uVar9 * 8);
      do {
        bVar5 = bVar16;
        if ((((short)plVar10[1] != 0) || (*(short *)((longlong)plVar10 + 10) != 0)) &&
           (*plVar10 != 0)) {
          bVar5 = (*(uint *)((longlong)plVar10 + 0x34) & 0x4030) != 0;
        }
        uVar13 = (uint)uVar12 + 1;
        if (!bVar5) {
          uVar13 = (uint)uVar12;
        }
        plVar10 = (longlong *)((longlong)plVar10 + 0x44);
        uVar12 = (ulonglong)uVar13;
        uVar15 = uVar15 - 1;
      } while (uVar15 != 0);
    }
    if (*(int *)(*(longlong *)(param_1 + 0x2ec) + 4) == 0x1bfa) {
      bVar16 = 1 < (uint)uVar12;
      goto LAB_140051473;
    }
    if (((uint)uVar12 != 0) && (iVar6 = FUN_1400675b8(param_1), iVar6 != 1)) goto LAB_140051473;
  }
  else {
LAB_140051473:
    if (bVar16) {
      DAT_1400f83d8 = DAT_1400f83d8 + 1;
      *(undefined1 *)(param_1 + 0x294) = 1;
      *(undefined4 *)(param_1 + 0x24) = 0x4f;
      if (*(longlong *)(param_1 + 0x234) != 0) {
        *(undefined1 *)(*(longlong *)(param_1 + 0x234) + 0x294) = 1;
      }
      goto LAB_140051559;
    }
  }
  *(undefined1 *)(param_1 + 0x294) = 0;
  if (uVar1 != 0) {
    plVar10 = (longlong *)(param_1 + 0x3c8);
    do {
      lVar7 = *plVar10;
      (**(code **)(DAT_1400f6bf8 + 0xac))
                ("%s, setup pPartialReadIoc = 0x%x\n","RC_StartInterStripeReadRebuild",lVar7);
      puVar8 = (undefined8 *)FUN_1400010e4(0x28);
      plVar10 = plVar10 + 1;
      *(undefined8 **)(lVar7 + 0x4dc) = puVar8;
      *puVar8 = *(undefined8 *)(lVar7 + 0x264);
      *(undefined4 *)(*(longlong *)(lVar7 + 0x4dc) + 8) = *(undefined4 *)(lVar7 + 0x20);
      *(undefined4 *)(*(longlong *)(lVar7 + 0x4dc) + 0xc) = *(undefined4 *)(lVar7 + 0x38);
      *(undefined4 *)(*(longlong *)(lVar7 + 0x4dc) + 0x10) = *(undefined4 *)(lVar7 + 0x3c);
      *(undefined8 *)(*(longlong *)(lVar7 + 0x4dc) + 0x20) = *(undefined8 *)(param_1 + 0x28);
      *(uint *)(lVar7 + 600) = *(uint *)(lVar7 + 600) & 0xfbffffff;
      *(undefined4 *)(lVar7 + 0x20) = 1;
      uVar11 = uVar11 - 1;
    } while (uVar11 != 0);
  }
  *(code **)(param_1 + 0x28) = FUN_14004fa2c;
LAB_140051559:
  FUN_140058520(param_1);
  return;
}

