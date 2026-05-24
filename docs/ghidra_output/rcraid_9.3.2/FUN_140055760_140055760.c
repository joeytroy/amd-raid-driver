// FUN_140055760 @ 140055760

void FUN_140055760(longlong param_1)

{
  longlong lVar1;
  undefined2 uVar2;
  int iVar3;
  int iVar4;
  uint uVar5;
  ulonglong uVar6;
  ulonglong uVar7;
  ulonglong uVar8;
  uint uVar9;
  uint *puVar10;
  short sVar11;
  uint uVar12;
  uint uVar13;
  uint uVar14;
  longlong lVar15;
  ulonglong uVar16;
  ulonglong uVar17;
  uint uVar18;
  uint uVar19;
  longlong *plVar20;
  ulonglong uVar21;
  undefined8 local_58;
  ulonglong local_50;
  ulonglong local_48;
  longlong *local_40;
  ulonglong local_38;
  
  lVar1 = *(longlong *)(param_1 + 0x234);
  iVar3 = FUN_140062bb8(lVar1);
  uVar7 = 0;
  if (iVar3 == 0) {
    return;
  }
  lVar15 = *(longlong *)(lVar1 + 0x2ec);
  local_58 = 0x271000002710;
  iVar3 = *(int *)(lVar15 + 4);
  uVar18 = *(uint *)(lVar15 + 0xb8);
  *(undefined4 *)(lVar1 + 0x274) = *(undefined4 *)(lVar1 + 0x38);
  uVar12 = (iVar3 == 0x1bfa) + 1;
  *(undefined4 *)(lVar1 + 0x278) = *(undefined4 *)(lVar1 + 0x3c);
  *(undefined4 *)(lVar1 + 0x240) = *(undefined4 *)(lVar1 + 0x20);
  *(undefined8 *)(lVar1 + 0x27c) = *(undefined8 *)(lVar1 + 0x264);
  *(undefined8 *)(lVar1 + 0x26c) = *(undefined8 *)(lVar1 + 0x30);
  if (*(int *)(lVar15 + 200) == 3) {
    local_50 = 0x200;
  }
  else {
    local_50 = 0x80;
    if (*(int *)(lVar15 + 200) == 2) {
      local_50 = 0x100;
    }
  }
  if (uVar18 != 0) {
    uVar16 = (ulonglong)DAT_1400f814c;
    plVar20 = (longlong *)(lVar1 + 0x3c8);
    uVar6 = uVar7;
    do {
      lVar15 = *plVar20;
      iVar3 = (int)uVar6;
      uVar14 = (uint)uVar7;
      if ((*(uint *)(lVar15 + 0xc) & 0x80000) == 0) {
        if (((int)uVar16 == 0) && (1 < DAT_140081628)) {
          puVar10 = &DAT_1400f3b68;
          uVar6 = (ulonglong)(DAT_140081628 - 1);
          do {
            if (*(code **)(puVar10 + 4) == FUN_140055760) {
              uVar16 = (ulonglong)*puVar10;
            }
            puVar10 = puVar10 + 0x18;
            uVar6 = uVar6 - 1;
          } while (uVar6 != 0);
          DAT_1400f814c = (uint)uVar16;
        }
        if ((((code *)(&DAT_1400f3b18)[uVar16 * 0xc] == FUN_140055760) &&
            ((&DAT_1400f3b10)[uVar16 * 0x18] != 0)) &&
           (((&DAT_1400f3b14)[uVar16 * 0x18] == (uint)*(ushort *)(lVar15 + 4) ||
            ((&DAT_1400f3b14)[uVar16 * 0x18] == 0xffff)))) {
          *(undefined4 *)(lVar15 + 0x24) = 0x19;
          (&DAT_1400f3b10)[uVar16 * 0x18] = (&DAT_1400f3b10)[uVar16 * 0x18] + -1;
        }
        if (*(int *)(lVar15 + 0x24) != 1) {
          if (*(int *)(lVar15 + 0x24) == 0x39) {
            uVar7 = (ulonglong)(uVar14 + 1000);
            *(undefined4 *)(*(longlong *)(lVar1 + 0x234) + 0x24) = 0x39;
          }
          else {
            (**(code **)(DAT_1400f6bf8 + 0xac))
                      ("FAIL at RC_UncachedRMWStripeDoneRead() dev:%d blk:%d size:%d\n",
                       *(undefined2 *)(lVar15 + 4),*(undefined4 *)(lVar15 + 0x264),
                       *(undefined4 *)(lVar15 + 0x20));
            if (*(int *)(lVar15 + 0x24) != 0x25) {
              FUN_140021a50(lVar15);
            }
            if (uVar14 < uVar12) {
              *(int *)((longlong)&local_58 + uVar7 * 4) = iVar3;
            }
            uVar16 = (ulonglong)DAT_1400f814c;
            uVar7 = (ulonglong)(uVar14 + 1);
          }
        }
      }
      else {
        if (uVar14 < uVar12) {
          *(int *)((longlong)&local_58 + uVar7 * 4) = iVar3;
        }
        uVar7 = (ulonglong)(uVar14 + 1);
      }
      uVar6 = (ulonglong)(iVar3 + 1U);
      plVar20 = plVar20 + 1;
    } while (iVar3 + 1U < uVar18);
    uVar14 = (uint)uVar7;
    if ((uVar14 != 0) && (uVar14 <= uVar12)) {
      uVar2 = (undefined2)local_58;
      iVar3 = 0;
      uVar5 = local_58._4_4_;
      uVar6 = 0;
      *(undefined2 *)(lVar1 + 0x3bc) = uVar2;
      *(undefined2 *)(lVar1 + 0x3be) = local_58._4_2_;
      *(undefined2 *)(lVar1 + 0x23e) = 0;
      uVar19 = (uint)local_58;
      do {
        uVar13 = (uint)uVar6;
        if (((uVar13 != uVar19) && (uVar13 != uVar5)) &&
           (lVar15 = *(longlong *)(lVar1 + 0x3c8 + uVar6 * 8),
           (*(uint *)(lVar15 + 0x284) & 0x100) != 0)) {
          *(code **)(lVar15 + 0x28) = FUN_140055e14;
          iVar4 = (**(code **)(*(longlong *)(lVar1 + 0x2ec) + 0x158))(lVar15);
          if (iVar4 == 0) {
            uVar9 = *(uint *)(lVar15 + 0xc) & 0xfff7ffff;
          }
          else {
            uVar9 = *(uint *)(lVar15 + 0xc) | 0x80000;
          }
          *(uint *)(lVar15 + 0xc) = uVar9;
          if ((uVar9 >> 0x13 & 1) == 0) {
            iVar3 = iVar3 + 1;
            FUN_140031f00(lVar15);
          }
          else {
            if (uVar12 <= uVar14) break;
            *(short *)(lVar1 + 0x3be) = (short)uVar6;
            *(uint *)((longlong)&local_58 + uVar7 * 4) = uVar13;
            uVar5 = local_58._4_4_;
            uVar19 = (uint)local_58;
          }
        }
        uVar6 = (ulonglong)(uVar13 + 1);
      } while (uVar13 + 1 < uVar18);
      if (iVar3 != 0) {
        *(short *)(lVar1 + 0x23c) = (short)iVar3;
        return;
      }
      if (uVar19 != 10000) {
        *(undefined4 *)(*(longlong *)(lVar1 + 0x3c8 + (ulonglong)uVar19 * 8) + 0x24) = 1;
        puVar10 = (uint *)(*(longlong *)(lVar1 + 0x3c8 + (ulonglong)uVar19 * 8) + 0xc);
        *puVar10 = *puVar10 & 0xfff7ffff;
      }
      if (uVar5 != 10000) {
        *(undefined4 *)(*(longlong *)(lVar1 + 0x3c8 + (ulonglong)uVar5 * 8) + 0x24) = 1;
        puVar10 = (uint *)(*(longlong *)(lVar1 + 0x3c8 + (ulonglong)uVar5 * 8) + 0xc);
        *puVar10 = *puVar10 & 0xfff7ffff;
      }
      *(undefined2 *)(lVar1 + 0x23c) = 1;
      FUN_140058520(lVar15);
      return;
    }
    if ((1 < uVar14) && (*(char *)(lVar1 + 0x294) == '\0')) {
      *(code **)(lVar1 + 0x28) = FUN_1400554c8;
      FUN_140051334(lVar1);
      return;
    }
  }
  sVar11 = 0;
  local_58 = *(ulonglong *)(lVar1 + 0x264);
  uVar16 = (ulonglong)*(uint *)(lVar1 + 0x20);
  uVar6 = (uVar16 + local_58) / local_50;
  uVar7 = local_58 / local_50;
  if (uVar18 - uVar12 != 0) {
    local_38 = (ulonglong)(uVar18 - uVar12);
    local_40 = (longlong *)(lVar1 + 0x3c8);
    uVar17 = local_50;
    uVar21 = local_58;
    local_48 = uVar7;
    sVar11 = 0;
    do {
      lVar15 = *local_40;
      uVar18 = *(uint *)(lVar15 + 0x284);
      *(uint *)(lVar15 + 0x284) = uVar18 | 0x10;
      uVar12 = (uint)uVar16;
      if ((uVar12 != 0) && (local_58 / uVar17 == *(ulonglong *)(lVar15 + 0x264) / uVar17)) {
        local_58 = local_58 + uVar17;
        *(uint *)(lVar15 + 0x284) = uVar18 & 0xffffffef;
        if (uVar7 == uVar6) {
          sVar11 = 1;
          *(code **)(lVar15 + 0x28) = FUN_1400554f8;
          uVar16 = 0;
          uVar21 = uVar21 + *(uint *)(lVar1 + 0x20);
          DAT_1400f83d0 = DAT_1400f83d0 + 1;
          if (DAT_1400f83d4 < DAT_1400f83d0) {
            DAT_1400f83d4 = DAT_1400f83d0;
          }
          *(undefined8 *)(lVar15 + 0x274) = 0;
          *(undefined4 *)(lVar15 + 0x240) = *(undefined4 *)(lVar1 + 0x20);
          *(undefined8 *)(lVar15 + 0x27c) = *(undefined8 *)(lVar1 + 0x264);
          FUN_14003750c(lVar15);
          FUN_140067a08(lVar15,lVar15 + 0x40,0,(*(uint *)(lVar1 + 0x264) & 0x1f) << 9,
                        *(undefined8 *)(lVar1 + 0x30),*(undefined4 *)(lVar1 + 0x38),
                        *(undefined4 *)(lVar1 + 0x3c),*(undefined4 *)(lVar1 + 0x20));
          uVar17 = local_50;
        }
        else {
          sVar11 = sVar11 + 1;
          uVar5 = (int)uVar21 - *(int *)(lVar15 + 0x264);
          iVar3 = (uVar5 & 0x1f) << 9;
          uVar14 = *(int *)(lVar15 + 0x20) - uVar5;
          uVar18 = uVar12;
          if ((uVar12 < uVar14) || (uVar18 = uVar14, uVar12 <= uVar14)) {
            uVar12 = 0;
            uVar14 = uVar18;
          }
          else {
            uVar12 = uVar12 - uVar14;
          }
          uVar16 = (ulonglong)uVar12;
          *(code **)(lVar15 + 0x28) = FUN_1400554f8;
          DAT_1400f83d0 = DAT_1400f83d0 + 1;
          if (DAT_1400f83d4 < DAT_1400f83d0) {
            DAT_1400f83d4 = DAT_1400f83d0;
          }
          *(ulonglong *)(lVar15 + 0x27c) = uVar21;
          uVar21 = uVar21 + uVar14;
          *(uint *)(lVar15 + 0x274) = uVar5 >> 5;
          *(int *)(lVar15 + 0x278) = iVar3;
          *(uint *)(lVar15 + 0x240) = uVar14;
          FUN_14003750c(lVar15);
          FUN_140067a08(lVar15,lVar15 + 0x40,uVar5 >> 5,iVar3,*(undefined8 *)(lVar1 + 0x30),
                        *(undefined4 *)(lVar1 + 0x38),*(undefined4 *)(lVar1 + 0x3c),uVar14);
          uVar18 = 0;
          uVar17 = local_50;
          uVar7 = local_48;
          if (uVar12 != 0) {
            iVar3 = *(int *)(lVar1 + 0x3c);
            uVar8 = (ulonglong)*(uint *)(lVar1 + 0x38);
            lVar15 = *(longlong *)(lVar1 + 0x30);
            uVar14 = uVar14 * 0x200;
            uVar12 = *(uint *)(lVar15 + (uVar8 + 3) * 0xc);
            uVar5 = uVar12 - iVar3;
            if (iVar3 == 0) {
              uVar5 = uVar12;
            }
            if (uVar14 < uVar5) {
              uVar19 = iVar3 + uVar14;
              uVar12 = *(uint *)(lVar1 + 0x38);
            }
            else {
              do {
                uVar13 = (uint)uVar8;
                uVar19 = uVar18;
                uVar12 = uVar13;
                if (uVar14 == 0) goto LAB_140055d40;
                uVar14 = uVar14 - uVar5;
                uVar12 = uVar13 + 1;
                if (uVar13 == 0x10) {
                  lVar15 = *(longlong *)(lVar15 + 0x14);
                  uVar12 = uVar18;
                }
                uVar8 = (ulonglong)uVar12;
                uVar5 = *(uint *)(lVar15 + (uVar8 + 3) * 0xc);
                uVar19 = uVar14;
                if (uVar14 < uVar5) goto LAB_140055d40;
              } while (uVar5 != uVar14);
              if ((uVar12 == 0x11) || (uVar12 = uVar12 + 1, uVar19 = uVar18, uVar12 == 0x11)) {
                lVar15 = *(longlong *)(lVar15 + 0x14);
                uVar19 = uVar18;
                uVar12 = 0;
              }
            }
LAB_140055d40:
            *(longlong *)(lVar1 + 0x30) = lVar15;
            *(uint *)(lVar1 + 0x38) = uVar12;
            *(uint *)(lVar1 + 0x3c) = uVar19;
          }
        }
      }
      local_40 = local_40 + 1;
      local_38 = local_38 - 1;
    } while (local_38 != 0);
  }
  *(short *)(lVar1 + 0x23c) = sVar11;
  *(undefined2 *)(lVar1 + 0x23e) = 0;
  return;
}

