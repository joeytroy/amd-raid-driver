// FUN_140052404 @ 140052404

void FUN_140052404(longlong param_1)

{
  uint uVar1;
  longlong lVar2;
  longlong lVar3;
  int iVar4;
  int iVar5;
  uint *puVar6;
  longlong *plVar7;
  int iVar8;
  ulonglong uVar9;
  ulonglong uVar10;
  ulonglong uVar11;
  uint uVar12;
  uint uVar13;
  uint uVar14;
  int local_40;
  undefined4 local_3c;
  ulonglong local_38;
  
  lVar2 = *(longlong *)(param_1 + 0x234);
  uVar11 = (ulonglong)DAT_1400f8178;
  uVar9 = 0;
  iVar8 = *(int *)(*(longlong *)(lVar2 + 0x2ec) + 4);
  uVar13 = (iVar8 == 0x1bfa) + 1;
  if ((DAT_1400f8178 == 0) && (1 < DAT_140081628)) {
    puVar6 = &DAT_1400f3b68;
    uVar10 = (ulonglong)(DAT_140081628 - 1);
    do {
      if (*(code **)(puVar6 + 4) == FUN_140052404) {
        uVar11 = (ulonglong)*puVar6;
      }
      puVar6 = puVar6 + 0x18;
      uVar10 = uVar10 - 1;
    } while (uVar10 != 0);
    DAT_1400f8178 = (uint)uVar11;
  }
  if ((((code *)(&DAT_1400f3b18)[uVar11 * 0xc] == FUN_140052404) &&
      ((&DAT_1400f3b10)[uVar11 * 0x18] != 0)) &&
     (((&DAT_1400f3b14)[uVar11 * 0x18] == (uint)*(ushort *)(param_1 + 4) ||
      ((&DAT_1400f3b14)[uVar11 * 0x18] == 0xffff)))) {
    *(undefined4 *)(param_1 + 0x24) = 0x19;
    (&DAT_1400f3b10)[uVar11 * 0x18] = (&DAT_1400f3b10)[uVar11 * 0x18] + -1;
  }
  iVar4 = FUN_140062bb8(lVar2);
  if (iVar4 != 0) {
    uVar11 = 0x200;
    iVar4 = *(int *)(*(longlong *)(lVar2 + 0x2ec) + 200);
    if (iVar4 == 3) {
      uVar14 = 0x200;
    }
    else {
      uVar14 = 0x80;
      if (iVar4 == 2) {
        uVar14 = 0x100;
      }
    }
    uVar1 = *(uint *)(*(longlong *)(lVar2 + 0x2ec) + 0xb8);
    uVar10 = (*(ulonglong *)(lVar2 + 0x264) / (ulonglong)uVar14) % (ulonglong)(uVar1 - uVar13);
    if ((iVar4 != 3) && (uVar11 = 0x80, iVar4 == 2)) {
      uVar11 = 0x100;
    }
    uVar12 = 0;
    *(undefined2 *)(lVar2 + 0x3c0) = 0;
    uVar11 = (ulonglong)
             ((int)((ulonglong)*(uint *)(lVar2 + 0x264) % uVar11) + *(int *)(lVar2 + 0x20) + -1 +
             uVar14) / (ulonglong)uVar14;
    local_40 = 10000;
    local_3c = 10000;
    uVar14 = (uint)uVar11;
    local_38 = uVar10;
    if (uVar14 != 0) {
      do {
        iVar4 = (int)uVar10;
        lVar3 = *(longlong *)(lVar2 + 0x3c8 + uVar10 * 8);
        uVar12 = (uint)uVar9;
        if ((*(uint *)(lVar3 + 0xc) & 0x80000) == 0) {
          iVar5 = *(int *)(lVar3 + 0x24);
          if (iVar5 != 1) {
            if (uVar12 < 2) {
              (&local_40)[uVar9] = iVar4;
            }
            uVar9 = (ulonglong)(uVar12 + 1);
            if (iVar5 == 0x25) {
              (**(code **)(DAT_1400f6bf8 + 0xac))
                        ("Medium Error at RC_StripeReadDoneRead() dev:%d blk:%d size:%d\n",
                         *(undefined2 *)(lVar3 + 4),*(undefined4 *)(lVar3 + 0x264),
                         *(undefined4 *)(lVar3 + 0x20));
              *(undefined2 *)(lVar2 + 0x3c0) = 1;
            }
            else {
              (**(code **)(DAT_1400f6bf8 + 0xac))
                        ("FAIL at RC_StripeReadDoneRead() dev:%d blk:%d size:%d\n");
              FUN_140021a50(lVar3);
            }
          }
        }
        else if ((*(uint *)(lVar3 + 0x284) & 4) == 0) {
          if (uVar12 < 2) {
            (&local_40)[uVar9] = iVar4;
          }
          uVar9 = (ulonglong)(uVar12 + 1);
        }
        uVar12 = (uint)uVar9;
        uVar10 = (ulonglong)(iVar4 + 1);
        uVar11 = uVar11 - 1;
      } while (uVar11 != 0);
    }
    if ((uVar1 != 0) && (uVar12 <= uVar13)) {
      plVar7 = (longlong *)(lVar2 + 0x3c8);
      uVar11 = (ulonglong)uVar1;
      do {
        lVar3 = *plVar7;
        plVar7 = plVar7 + 1;
        *(undefined4 *)(lVar3 + 0x24) = 1;
        uVar11 = uVar11 - 1;
      } while (uVar11 != 0);
    }
    iVar4 = 0;
    if (uVar12 == 0) {
      uVar13 = 0;
      uVar11 = local_38;
      iVar8 = 0;
      if (uVar14 != 0) {
        do {
          lVar3 = *(longlong *)(lVar2 + 0x3c8 + (ulonglong)(uVar13 + (int)uVar11) * 8);
          iVar4 = iVar8;
          if (((*(uint *)(lVar3 + 0x284) & 4) == 0) &&
             (iVar5 = FUN_140013ea0(*(undefined8 *)(lVar3 + 0x2d4)), uVar11 = local_38, iVar5 != 0))
          {
            iVar4 = iVar8 + 1;
            *(code **)(lVar3 + 0x28) = FUN_140052190;
            FUN_140014128(lVar3);
            uVar11 = local_38;
          }
          uVar13 = uVar13 + 1;
          iVar8 = iVar4;
        } while (uVar13 < uVar14);
      }
      *(undefined2 *)(lVar2 + 0x23e) = 0;
      if (iVar4 == 0) {
        *(undefined2 *)(lVar2 + 0x23c) = 1;
        *(code **)(*(longlong *)(lVar2 + 0x3c8 + (uVar11 & 0xffffffff) * 8) + 0x28) = FUN_140052190;
        FUN_140058520();
      }
      else {
        *(ushort *)(lVar2 + 0x23c) = (ushort)iVar4 & 0xff;
      }
    }
    else {
      uVar13 = 0;
      if (uVar14 != 0) {
        do {
          lVar3 = *(longlong *)(lVar2 + 0x3c8 + (ulonglong)(uVar13 + (int)local_38) * 8);
          if (((*(uint *)(lVar3 + 0x284) & 4) == 0) && (uVar11 = 0, *(char *)(lVar3 + 0x50) != '\0')
             ) {
            do {
              uVar12 = (int)uVar11 + 1;
              *(undefined8 *)(DAT_1400eeed0 + (ulonglong)DAT_1400eeec8 * 8) =
                   *(undefined8 *)(lVar3 + 0x5c + uVar11 * 0xc);
              DAT_1400eeec8 = DAT_1400eeec8 + 1;
              uVar11 = (ulonglong)uVar12;
            } while (uVar12 < *(byte *)(lVar3 + 0x50));
          }
          uVar13 = uVar13 + 1;
        } while (uVar13 < uVar14);
      }
      *(undefined2 *)(lVar2 + 0x3bc) = (undefined2)local_40;
      if (iVar8 == 0x1bfa) {
        *(undefined2 *)(lVar2 + 0x3be) = (undefined2)local_3c;
      }
      iVar8 = *(int *)(*(longlong *)(lVar2 + 0x2ec) + 200);
      if (iVar8 == 3) {
        iVar4 = 0x10;
      }
      else {
        iVar4 = 4;
        if (iVar8 == 2) {
          iVar4 = 8;
        }
      }
      FUN_140057864(lVar2,lVar2 + 0x40,iVar4 * uVar1,0,FUN_14005281c);
    }
  }
  return;
}

