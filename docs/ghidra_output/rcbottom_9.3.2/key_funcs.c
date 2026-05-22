// ===== FUN_140007d40 @ 140007d40 =====

/* WARNING: Function: __security_check_cookie replaced with injection: security_check_cookie */

void FUN_140007d40(wchar_t *param_1,longlong param_2,char param_3)

{
  ushort uVar1;
  int iVar2;
  short sVar3;
  uint uVar4;
  short sVar5;
  int iVar6;
  int iVar7;
  uint uVar8;
  undefined1 auStack_128 [32];
  undefined4 local_108;
  undefined2 local_100;
  undefined8 local_f8;
  ushort local_e8 [2];
  ushort local_e4 [2];
  undefined2 local_e0 [2];
  undefined2 local_dc [2];
  undefined2 local_d8 [2];
  undefined2 local_d4 [2];
  uint local_d0 [2];
  undefined1 local_c8 [16];
  undefined1 local_b8 [16];
  undefined1 local_a8 [16];
  undefined1 local_98 [16];
  wchar_t local_88 [4];
  undefined2 local_80;
  wchar_t local_78 [4];
  undefined2 local_70;
  wchar_t local_68 [4];
  undefined2 local_60;
  wchar_t local_58 [4];
  undefined2 local_50;
  ulonglong local_48;
  
  local_48 = DAT_140014180 ^ (ulonglong)auStack_128;
  wcsncpy(local_88,param_1 + 8,4);
  iVar6 = 0;
  sVar5 = 0;
  local_80 = 0;
  wcsncpy(local_78,param_1 + 0x11,4);
  local_70 = 0;
  wcsncpy(local_58,param_1 + 0x1d,4);
  local_50 = 0;
  wcsncpy(local_68,param_1 + 0x21,4);
  local_60 = 0;
  RtlInitUnicodeString(local_c8,local_88);
  RtlInitUnicodeString(local_b8,local_78);
  RtlInitUnicodeString(local_a8,local_68);
  RtlInitUnicodeString(local_98,local_58);
  RtlUnicodeStringToInteger(local_c8,0x10,local_dc);
  RtlUnicodeStringToInteger(local_b8,0x10,local_e0);
  RtlUnicodeStringToInteger(local_a8,0x10,local_d4);
  RtlUnicodeStringToInteger(local_98,0x10,local_d8);
  *(undefined2 *)(param_2 + 0x16058) = local_e0[0];
  *(undefined2 *)(param_2 + 0x16056) = local_dc[0];
  *(undefined2 *)(param_2 + 0x1605c) = local_d8[0];
  *(undefined2 *)(param_2 + 0x1605a) = local_d4[0];
  iVar2 = wcsncmp(param_1,L"PCI\\VEN_1022&DEV_7905",0x15);
  iVar7 = 2;
  if (iVar2 == 0) {
    sVar5 = 0x82;
    sVar3 = 0xc6;
  }
  else {
    iVar2 = wcsncmp(param_1,L"PCI\\VEN_1022&DEV_7916",0x15);
    if ((iVar2 == 0) || (iVar2 = wcsncmp(param_1,L"PCI\\VEN_1022&DEV_7917",0x15), iVar2 == 0)) {
      sVar5 = 0x108;
      sVar3 = 0x10a;
    }
    else {
      iVar2 = wcsncmp(param_1,L"PCI\\VEN_1022&DEV_43BD",0x15);
      sVar3 = sVar5;
      if (iVar2 != 0) {
        *(undefined1 *)(param_2 + 0x1606c) = 0;
        if (param_3 == '\0') {
          iVar7 = 99;
          *(undefined4 *)(param_2 + 0x16068) = 99;
        }
        else {
          *(undefined4 *)(param_2 + 0x16068) = 2;
        }
        goto LAB_140007f3d;
      }
    }
  }
  *(undefined4 *)(param_2 + 0x16068) = 1;
  *(undefined1 *)(param_2 + 0x1606c) = 1;
  iVar7 = 1;
LAB_140007f3d:
  if (iVar7 == 1) {
    *(code **)(param_2 + 0x16100) = FUN_140004090;
    *(code **)(param_2 + 0x16108) = FUN_140001438;
    *(code **)(param_2 + 0x16110) = FUN_1400021d4;
    *(code **)(param_2 + 0x16118) = FUN_140003f7c;
    *(code **)(param_2 + 0x16158) = FUN_14000303c;
    *(code **)(param_2 + 0x16120) = FUN_140003048;
    *(code **)(param_2 + 0x16128) = FUN_1400027a8;
    *(code **)(param_2 + 0x16130) = FUN_1400028f8;
    *(code **)(param_2 + 0x16138) = FUN_140001ba4;
    *(code **)(param_2 + 0x16140) = FUN_140001bbc;
    *(code **)(param_2 + 0x16148) = FUN_140003838;
    *(undefined1 **)(param_2 + 0x16150) = &LAB_140001778;
    *(code **)(param_2 + 0x16160) = FUN_140003598;
    *(undefined1 **)(param_2 + 0x16168) = &LAB_140002808;
    if ((sVar5 != 0) && (sVar3 != 0)) {
      local_f8 = 0;
      local_100 = 1;
      local_108 = CONCAT22(local_108._2_2_,0x40);
      local_e4[0] = 0;
      local_e8[0] = 0;
      (**(code **)(DAT_140014958 + 0x418))
                (DAT_140014980,*(undefined8 *)(param_2 + 0x20),&DAT_140012258,param_2 + 0x1c298);
      local_108 = 2;
      (**(code **)(param_2 + 0x1c2d0))(*(undefined8 *)(param_2 + 0x1c2a0),0,local_e4,sVar5);
      local_108 = 2;
      (**(code **)(param_2 + 0x1c2d0))(*(undefined8 *)(param_2 + 0x1c2a0),0,local_e8,sVar3);
      iVar6 = (uint)local_e8[0] * 0x10000 + (uint)local_e4[0];
    }
    *(int *)(param_2 + 0x1c2d8) = iVar6;
  }
  else if (iVar7 == 2) {
    *(code **)(param_2 + 0x16100) = FUN_14000fafc;
    local_f8 = 0;
    *(code **)(param_2 + 0x16108) = FUN_14000c0bc;
    local_100 = 1;
    *(code **)(param_2 + 0x16110) = FUN_14000dd44;
    *(code **)(param_2 + 0x16118) = FUN_14000303c;
    *(code **)(param_2 + 0x16158) = FUN_14000e59c;
    *(code **)(param_2 + 0x16120) = FUN_14000e800;
    *(undefined1 **)(param_2 + 0x16128) = &LAB_14000918c;
    *(code **)(param_2 + 0x16130) = FUN_14000e494;
    *(code **)(param_2 + 0x16138) = FUN_14000c814;
    *(code **)(param_2 + 0x16140) = FUN_14000c82c;
    *(code **)(param_2 + 0x16148) = FUN_14000d06c;
    *(undefined1 **)(param_2 + 0x16150) = &LAB_140002808;
    *(code **)(param_2 + 0x16160) = FUN_14001023c;
    *(code **)(param_2 + 0x16168) = FUN_1400100c0;
    local_108 = CONCAT22(local_108._2_2_,0x40);
    (**(code **)(DAT_140014958 + 0x418))
              (DAT_140014980,*(undefined8 *)(param_2 + 0x20),&DAT_140012258,param_2 + 0x1c298);
    local_108 = 1;
    (**(code **)(param_2 + 0x1c2d0))(*(undefined8 *)(param_2 + 0x1c2a0),0,local_e8,0x34);
    uVar1 = local_e8[0] & 0xff;
    uVar4 = 1;
    uVar8 = 0;
    do {
      uVar8 = uVar8 + 1;
      local_108 = 2;
      (**(code **)(param_2 + 0x1c2d0))(*(undefined8 *)(param_2 + 0x1c2a0),0,local_e4,uVar1);
      if ((char)local_e4[0] == '\x10') {
        local_108 = 2;
        (**(code **)(param_2 + 0x1c2d0))
                  (*(undefined8 *)(param_2 + 0x1c2a0),0,local_d0,(byte)local_e8[0] + 0x12);
        *(uint *)(param_2 + 0x1c7ac) = local_d0[0] & 0xf;
        *(uint *)(param_2 + 0x1c7a8) = local_d0[0] >> 4 & 0x3f;
        uVar4 = 0;
      }
      else if (10 < uVar8) {
        uVar4 = 0;
      }
      uVar1 = local_e4[0] >> 8;
      local_e8[0] = CONCAT11(local_e8[0]._1_1_,(char)(local_e4[0] >> 8));
    } while ((char)uVar4 != '\0');
  }
  else {
    *(code **)(param_2 + 0x16100) = FUN_1400102d8;
    *(code **)(param_2 + 0x16108) = FUN_1400102d8;
    *(undefined1 **)(param_2 + 0x16110) = &LAB_140010318;
    *(code **)(param_2 + 0x16118) = FUN_14000303c;
    *(code **)(param_2 + 0x16158) = FUN_14000303c;
    *(undefined1 **)(param_2 + 0x16120) = &LAB_140010328;
    *(undefined1 **)(param_2 + 0x16128) = &LAB_14000918c;
    *(code **)(param_2 + 0x16130) = FUN_14000303c;
    *(code **)(param_2 + 0x16138) = FUN_1400102f4;
    *(undefined1 **)(param_2 + 0x16140) = &LAB_140010304;
    *(code **)(param_2 + 0x16148) = FUN_1400102d8;
    *(undefined1 **)(param_2 + 0x16150) = &LAB_140002808;
    *(code **)(param_2 + 0x16160) = FUN_14000303c;
    *(undefined1 **)(param_2 + 0x16168) = &LAB_140002808;
  }
  return;
}



// ===== FUN_140008a48 @ 140008a48 =====

undefined8 FUN_140008a48(undefined8 param_1)

{
  int iVar1;
  longlong lVar2;
  uint uVar3;
  
  lVar2 = (**(code **)(DAT_140014958 + 0x650))(DAT_140014980,param_1,PTR_DAT_140014090);
  (**(code **)(DAT_140014958 + 0x9d8))(DAT_140014980,0,lVar2 + 0x16288);
  (**(code **)(DAT_140014958 + 0x9d8))(DAT_140014980,0,lVar2 + 0x1c2e8);
  uVar3 = 0;
  if (*(int *)(lVar2 + 0xb0) != 0) {
    do {
      KeInitializeSpinLock(lVar2 + ((ulonglong)*(uint *)(lVar2 + 0xb0) + 0x38e5) * 8);
      uVar3 = uVar3 + 1;
    } while (uVar3 < *(uint *)(lVar2 + 0xb0));
  }
  FUN_140007978();
  (**(code **)(lVar2 + 0x16110))(lVar2,0);
  (**(code **)(DAT_140014958 + 0x9f8))
            (DAT_140014980,*(undefined8 *)(lVar2 + 0x16010),0xffffffffffe17b80);
  *(undefined1 *)(lVar2 + 0x16054) = 0;
  iVar1 = FUN_1400093c4(lVar2);
  if (iVar1 == 1) {
    FUN_14000273c(lVar2);
  }
  return 0;
}



// ===== FUN_140007978 @ 140007978 =====

void FUN_140007978(void)

{
  DAT_140014284 = 0;
                    /* WARNING: Could not recover jumptable at 0x000140007996. Too many branches */
                    /* WARNING: Treating indirect jump as call */
  (**(code **)(DAT_140014958 + 0x9d8))(DAT_140014980,0,&DAT_140216880);
  return;
}



// ===== FUN_1400021d4 @ 1400021d4 =====

void FUN_1400021d4(longlong param_1,char param_2)

{
  char cVar1;
  undefined4 *puVar2;
  uint uVar3;
  undefined1 uVar4;
  byte bVar5;
  int iVar6;
  undefined8 uVar7;
  ulonglong uVar8;
  ulonglong *puVar9;
  int iVar10;
  ulonglong uVar11;
  longlong lVar12;
  undefined4 *puVar13;
  uint uVar14;
  uint uVar15;
  longlong lVar16;
  ulonglong *puVar17;
  int iVar18;
  ulonglong uVar19;
  undefined4 *puVar20;
  ulonglong uVar21;
  longlong local_48;
  longlong local_40;
  
  uVar15 = 0;
  cVar1 = *(char *)(param_1 + 0x15920);
  *(char *)(param_1 + 0x1c7b0) = param_2;
  *(undefined2 *)(param_1 + 0x15920) = 0;
  puVar2 = *(undefined4 **)(param_1 + 0x10);
  *(longlong *)(param_1 + 0x15938) = param_1;
  *(undefined8 *)(param_1 + 0x15908) = 0;
  *(undefined4 **)(param_1 + 0x158b8) = puVar2;
  if (puVar2 != (undefined4 *)0x0) {
    if (cVar1 == '\0') {
      (**(code **)(DAT_140014958 + 0xdb0))
                (DAT_140014980,*(undefined8 *)(param_1 + 0x20),0,0,0x5c2,
                 "Y:\\RC-932\\RC_932_00255\\fulcrum\\rc\\platforms\\rcbottom\\rcbottom\\ahci.c");
    }
    if ((param_2 == '\0') && (cVar1 == '\0')) {
      uVar7 = (**(code **)(DAT_140014958 + 0x100))(DAT_140014980,*(undefined8 *)(param_1 + 0x20));
      FUN_14000b730(uVar7);
      if (*(char *)(param_1 + 0x15930) == '\0') {
        lVar16 = param_1 + 0x650;
        uVar14 = uVar15;
        do {
          FUN_14000be7c(param_1,uVar14,lVar16);
          uVar14 = uVar14 + 1;
          lVar16 = lVar16 + 0x728;
        } while (uVar14 < 8);
        *(undefined1 *)(param_1 + 0x15930) = 1;
      }
    }
    do {
      KeStallExecutionProcessor(1000);
      if (puVar2[1] == 0xffffffff) {
        if (param_2 == '\0') {
          FUN_14000273c(param_1);
        }
        else {
          *(undefined4 *)(param_1 + 0x1c848) = 1;
        }
        *(undefined2 *)(param_1 + 0x15920) = 0x101;
        return;
      }
    } while (((puVar2[1] & 1) != 0) && (uVar15 = uVar15 + 1, uVar15 < 1000));
    puVar2[1] = 0x80000000;
    *(ulonglong *)(param_1 + 0x158e0) = (ulonglong)(uint)puVar2[3];
    *(undefined4 *)(param_1 + 0x158f8) = *puVar2;
    uVar15 = *(uint *)(param_1 + 0x158f8);
    if (uVar15 != 0xffffffff) {
      uVar4 = FUN_1400021a8(*(undefined8 *)(param_1 + 0x158e0));
      *(undefined1 *)(param_1 + 0x158fc) = uVar4;
      *(uint *)(param_1 + 0x16070) = (uVar15 & 0x1f) + 1;
      bVar5 = ((byte)(*(uint *)(param_1 + 0x158f8) >> 8) & 0x1f) + 1;
      iVar18 = (uint)bVar5 * 0x2080;
      iVar6 = (uint)bVar5 << 5;
      *(byte *)(param_1 + 0x158fe) = (byte)(*(uint *)(param_1 + 0x158f8) >> 0x14) & 0xf;
      iVar10 = 0x1000;
      *(byte *)(param_1 + 0x158fd) = bVar5;
      *(int *)(param_1 + 0x158e8) = iVar6;
      *(int *)(param_1 + 0x158ec) = iVar18;
      *(undefined4 *)(param_1 + 0x158f0) = 0x1000;
      if (param_2 == '\0') {
        (**(code **)(DAT_140014958 + 0x9d8))(DAT_140014980,0,param_1 + 0x15928);
        iVar10 = *(int *)(param_1 + 0x158f0);
        iVar18 = *(int *)(param_1 + 0x158ec);
        iVar6 = *(int *)(param_1 + 0x158e8);
      }
      iVar10 = (iVar6 + iVar18 + iVar10) * 8;
      lVar16 = local_40;
      if (cVar1 == '\0') {
        (**(code **)(DAT_140014958 + 0xa8))
                  (DAT_140014980,*(undefined8 *)(param_1 + 0x16008),iVar10 + 0x4fef,0,&local_40);
        lVar16 = (**(code **)(DAT_140014958 + 0xb8))(DAT_140014980,local_40);
        local_40 = (**(code **)(DAT_140014958 + 0xb0))(DAT_140014980,local_40);
        *(int *)(param_1 + 0x1c320) = iVar10 + 0x3ff0;
        *(longlong *)(param_1 + 0x1c318) = local_40;
      }
      puVar17 = (ulonglong *)(param_1 + 0x150);
      uVar21 = 0;
      local_48 = local_40;
      do {
        uVar15 = 0;
        iVar10 = (int)uVar21;
        *(undefined1 *)(puVar17 + 0x62) = 0;
        puVar20 = (undefined4 *)((ulonglong)(uint)((iVar10 + 2) * 0x80) + (longlong)puVar2);
        puVar17[-3] = (ulonglong)puVar20;
        if (cVar1 == '\0') {
          uVar8 = lVar16 + 0x3ffU & 0xfffffffffffffc00;
          uVar19 = local_40 + 0x3ffU & 0xfffffffffffffc00;
          *(int *)(puVar17 + 0x52) = (int)uVar8;
          *(int *)((longlong)puVar17 + 0x294) = (int)(lVar16 + 0x3ffU >> 0x20);
          puVar17[-1] = uVar19;
          uVar14 = *(uint *)(param_1 + 0x158e8);
          uVar8 = (ulonglong)uVar14 + 0x3ff + uVar8;
          uVar11 = uVar8 & 0xfffffffffffffc00;
          *(int *)(puVar17 + 0x53) = (int)uVar11;
          uVar19 = (ulonglong)uVar14 + 0x3ff + uVar19 & 0xfffffffffffffc00;
          *(int *)((longlong)puVar17 + 0x29c) = (int)(uVar8 >> 0x20);
          *puVar17 = uVar19;
          uVar11 = (ulonglong)*(uint *)(param_1 + 0x158ec) + 0x3ff + uVar11;
          uVar8 = uVar11 & 0xfffffffffffffc00;
          uVar19 = (ulonglong)*(uint *)(param_1 + 0x158ec) + 0x3ff + uVar19 & 0xfffffffffffffc00;
          puVar17[1] = uVar19;
          *(int *)((longlong)puVar17 + 0x2a4) = (int)(uVar11 >> 0x20);
          *(int *)(puVar17 + 0x54) = (int)uVar8;
          local_40 = uVar19 + *(uint *)(param_1 + 0x158f0);
          lVar16 = uVar8 + *(uint *)(param_1 + 0x158f0);
          local_48 = local_40;
        }
        if ((*(ulonglong *)(param_1 + 0x158e0) >> (uVar21 & 0x3f) & 1) != 0) {
          if (*(char *)(param_1 + 0x158fd) != '\0') {
            puVar13 = (undefined4 *)(puVar17[-1] + 0xc);
            uVar14 = uVar15;
            do {
              uVar15 = uVar15 + 1;
              iVar18 = (int)puVar17[0x53] + uVar14;
              uVar14 = uVar14 + 0x2080;
              puVar13[-1] = iVar18;
              *puVar13 = *(undefined4 *)((longlong)puVar17 + 0x29c);
              puVar13 = puVar13 + 8;
            } while (uVar15 < *(byte *)(param_1 + 0x158fd));
          }
          *puVar20 = (int)puVar17[0x52];
          puVar20[1] = *(undefined4 *)((longlong)puVar17 + 0x294);
          puVar20[2] = (int)puVar17[0x54];
          puVar20[3] = *(undefined4 *)((longlong)puVar17 + 0x2a4);
          uVar15 = 1 << ((byte)uVar21 & 0x1f);
          if (((*(uint *)(param_1 + 0x16170) & uVar15) == uVar15) ||
             ((*(uint *)(param_1 + 0x16174) & uVar15) == uVar15)) {
            uVar15 = puVar20[0xb];
            uVar14 = *(uint *)(param_1 + 0x158f8);
            if ((uVar14 >> 0xd & 1) == 0) {
              uVar15 = uVar15 | 0x100;
            }
            uVar3 = uVar15 | 0x200;
            if ((uVar14 & 0x4000) != 0) {
              uVar3 = uVar15;
            }
            uVar15 = uVar3 & 0xfffffcff;
            if ((uVar14 & 0x6000) != 0x6000) {
              uVar15 = uVar3;
            }
            puVar20[0xb] = uVar15;
          }
          *(int *)(puVar17 + -2) = iVar10;
          *(undefined4 *)((longlong)puVar17 + -0xc) = *(undefined4 *)(param_1 + 0x16050);
          if (param_2 == '\0') {
            (**(code **)(DAT_140014958 + 0x9d8))(DAT_140014980,0,puVar17 + 0xde);
            (**(code **)(DAT_140014958 + 0x9d8))(DAT_140014980,0,puVar17 + 0xdd);
          }
          *(undefined8 *)((longlong)puVar17 + 0x2ac) = 0;
          puVar9 = puVar17 + 0x22;
          *(undefined8 *)((longlong)puVar17 + 0x2b4) = 0;
          lVar12 = 0x20;
          puVar17[0xdf] = 0;
          puVar17[0xe0] = 0;
          puVar17[99] = 0;
          do {
            puVar9[-0x20] = 0;
            *puVar9 = 0;
            puVar9 = puVar9 + 1;
            lVar12 = lVar12 + -1;
          } while (lVar12 != 0);
          *(undefined2 *)((longlong)puVar17 + 0x2e9) = 0xf0f;
          FUN_140001c68(param_1 + 0x138,uVar21);
          FUN_140002ef0(param_1,uVar21);
          local_40 = local_48;
        }
        puVar17 = puVar17 + 0xe5;
        uVar21 = (ulonglong)(iVar10 + 1U);
      } while (iVar10 + 1U < 8);
      FUN_14000204c(param_1 + 0x138,0xffffffff);
    }
  }
  return;
}



// ===== FUN_14000924c @ 14000924c =====

void FUN_14000924c(undefined8 param_1)

{
  longlong lVar1;
  
  lVar1 = (**(code **)(DAT_140014958 + 0x650))(DAT_140014980,param_1,PTR_DAT_140014090);
  *(undefined1 *)(lVar1 + 0x16054) = 1;
  KeStallExecutionProcessor(5000);
  if (*(int *)(lVar1 + 0x16068) == 1) {
    FUN_140001ed8(lVar1);
  }
  KeStallExecutionProcessor(25000);
  *(undefined1 *)(lVar1 + 0x1c2dc) = 1;
  if (*(char *)(lVar1 + 0x1607c) == '\0') {
    FUN_14000a564(param_1);
    (**(code **)(DAT_140014958 + 0x188))(DAT_140014980,*(undefined8 *)(lVar1 + 0x16020),1);
    (**(code **)(DAT_140014958 + 0x188))(DAT_140014980,*(undefined8 *)(lVar1 + 0x16020),2);
    (**(code **)(DAT_140014958 + 0x188))(DAT_140014980,*(undefined8 *)(lVar1 + 0x16020),3);
    (**(code **)(DAT_140014958 + 0x188))(DAT_140014980,*(undefined8 *)(lVar1 + 0x16020),4);
  }
  return;
}



// ===== FUN_140005ff4 @ 140005ff4 =====

/* WARNING: Function: __security_check_cookie replaced with injection: security_check_cookie */

undefined8 FUN_140005ff4(undefined8 param_1)

{
  ushort uVar1;
  char cVar2;
  int iVar3;
  int iVar4;
  undefined4 *puVar5;
  undefined8 uVar6;
  longlong lVar7;
  char *pcVar8;
  longlong lVar9;
  uint uVar10;
  ulonglong uVar11;
  ushort *puVar12;
  ulonglong uVar13;
  ulonglong uVar14;
  uint uVar15;
  undefined1 uVar16;
  undefined1 uVar17;
  undefined1 auStack_db8 [32];
  wchar_t *local_d98;
  int *local_d90;
  undefined8 local_d88;
  undefined8 local_d78;
  undefined1 local_d70;
  char local_d6f;
  int local_d6c;
  undefined8 local_d68;
  uint local_d60;
  undefined8 local_d58;
  code *pcStack_d50;
  undefined8 local_d48;
  undefined8 uStack_d40;
  undefined8 local_d38;
  undefined8 local_d30;
  undefined8 uStack_d28;
  undefined8 local_d20;
  undefined8 uStack_d18;
  undefined8 local_d10;
  undefined8 uStack_d08;
  undefined *local_d00;
  undefined8 local_cf8;
  undefined8 uStack_cf0;
  undefined8 local_ce8;
  undefined8 uStack_ce0;
  undefined8 local_cd8;
  undefined8 uStack_cd0;
  undefined8 local_cc8;
  undefined4 local_cb8;
  undefined4 local_cb4;
  undefined8 local_cb0;
  undefined4 local_c68;
  undefined8 local_c64;
  undefined4 local_c5c;
  undefined4 local_c58;
  undefined4 local_c54;
  undefined8 local_c50;
  undefined4 local_c48;
  undefined4 local_c44;
  undefined4 local_c40;
  undefined4 local_c3c;
  wchar_t local_c38 [512];
  undefined1 local_838 [2048];
  ulonglong local_38;
  
  local_38 = DAT_140014180 ^ (ulonglong)auStack_db8;
  uVar14 = 0;
  iVar4 = 0;
  local_d78 = 0;
  uVar15 = 0;
  local_d60 = 0;
  local_d68 = param_1;
  FUN_140011400(local_c38,0,0x400);
  FUN_140011400(local_838,0,0x800);
  uVar16 = 0;
  if (DAT_140014290 == 0x12) {
    local_d78 = 0;
  }
  else {
    cVar2 = FUN_14000a3d0();
    local_d70 = 0x16;
    local_d6f = cVar2;
    if (cVar2 != '\0') {
      local_d98 = (wchar_t *)&local_d70;
      local_d90 = (int *)CONCAT44(local_d90._4_4_,1);
      (**(code **)(DAT_140014958 + 0x248))(DAT_140014980,local_d68,FUN_140006c34,0x1b);
    }
    local_d90 = (int *)((ulonglong)local_d90 & 0xffffffff00000000);
    local_d98 = (wchar_t *)0x0;
    (**(code **)(DAT_140014958 + 0x248))(DAT_140014980,local_d68,FUN_140006da0,0x16);
    local_d00 = PTR_DAT_140014090;
    local_d90 = &local_d6c;
    local_d98 = local_c38;
    local_d20 = 0;
    uStack_d28 = 0;
    local_d30 = 0x38;
    local_d10 = 0;
    uStack_d08 = 0;
    uStack_d18 = 0x100000001;
    (**(code **)(DAT_140014958 + 0x3f0))(DAT_140014980,local_d68,1,0x400);
    local_d90 = &local_d6c;
    local_d98 = (wchar_t *)local_838;
    (**(code **)(DAT_140014958 + 0x3f0))(DAT_140014980,local_d68,2,0x800);
    uVar11 = uVar14;
    uVar17 = 0;
    if (local_d6c != 9) {
      do {
        iVar3 = wcsncmp(local_c38 + uVar11,L"CC_010802",9);
        if (iVar3 == 0) {
          uVar16 = 1;
        }
        uVar10 = (int)uVar11 + 1;
        uVar11 = (ulonglong)uVar10;
      } while (uVar10 < local_d6c - 9U);
      uVar11 = (ulonglong)local_d60;
      cVar2 = local_d6f;
      uVar17 = uVar16;
    }
    uVar10 = (uint)uVar11;
    local_d90 = &local_d6c;
    puVar12 = &DAT_1400142a0;
    local_d98 = &DAT_1400142a0;
    iVar3 = (**(code **)(DAT_140014958 + 0x3f0))(DAT_140014980,local_d68,10,0x400);
    if (-1 < iVar3) {
      for (; ((ushort)(*puVar12 - 1) < 0x30 || (uVar13 = uVar14, 0x38 < *puVar12));
          puVar12 = puVar12 + 1) {
      }
      do {
        uVar1 = *puVar12;
        if ((9 < (ushort)(uVar1 - 0x30)) || (uVar1 == 0)) break;
        uVar13 = (ulonglong)((uint)uVar1 + ((int)uVar13 * 5 + -0x18) * 2);
        puVar12 = puVar12 + 1;
      } while (puVar12 != (ushort *)0x0);
      while( true ) {
        iVar4 = (int)uVar13;
        if ((0x2e < (ushort)(*puVar12 - 1)) && (*puVar12 < 0x3a)) break;
        puVar12 = puVar12 + 1;
      }
      do {
        uVar1 = *puVar12;
        if ((9 < (ushort)(uVar1 - 0x30)) || (uVar1 == 0)) break;
        uVar14 = (ulonglong)((uint)uVar1 + ((int)uVar14 * 5 + -0x18) * 2);
        puVar12 = puVar12 + 1;
      } while (puVar12 != (ushort *)0x0);
      while( true ) {
        uVar15 = (uint)uVar14;
        if ((0x2e < (ushort)(*puVar12 - 1)) && (*puVar12 < 0x3a)) break;
        puVar12 = puVar12 + 1;
      }
      do {
        uVar10 = (uint)uVar11;
        uVar1 = *puVar12;
        if ((9 < (ushort)(uVar1 - 0x30)) || (uVar1 == 0)) break;
        uVar10 = (uint)uVar1 + (uVar10 * 5 + -0x18) * 2;
        uVar11 = (ulonglong)uVar10;
        puVar12 = puVar12 + 1;
      } while (puVar12 != (ushort *)0x0);
    }
    iVar3 = (**(code **)(DAT_140014958 + 600))(DAT_140014980,&local_d68,&local_d30,&local_d78);
    if (-1 < iVar3) {
      puVar5 = (undefined4 *)
               (**(code **)(DAT_140014958 + 0x650))(DAT_140014980,local_d78,PTR_DAT_140014090);
      LOCK();
      UNLOCK();
      iVar3 = DAT_140014294 + 1;
      if (DAT_140014294 + 1 < 0x20) {
        lVar9 = (longlong)DAT_140014294;
        DAT_140014294 = DAT_140014294 + 1;
        *(undefined4 **)(&DAT_140216780 + lVar9 * 8) = puVar5;
        iVar3 = DAT_140014294;
      }
      DAT_140014294 = iVar3;
      *puVar5 = 0;
      *(undefined8 *)(puVar5 + 8) = local_d78;
      uVar6 = (**(code **)(DAT_140014958 + 0x108))(DAT_140014980,local_d78);
      *(undefined8 *)(puVar5 + 10) = uVar6;
      pcVar8 = &DAT_140216505;
      puVar5[0x5818] = (iVar4 << 8 | uVar15) << 8 | uVar10;
      do {
        if (((pcVar8[-1] == (char)iVar4) && (*pcVar8 == (char)uVar15)) &&
           (pcVar8[1] == (char)uVar10)) {
          *(undefined2 *)(puVar5 + 0x5819) = *(undefined2 *)(pcVar8 + -5);
          *(undefined2 *)((longlong)puVar5 + 0x16066) = *(undefined2 *)(pcVar8 + -3);
        }
        pcVar8 = pcVar8 + 8;
      } while ((longlong)pcVar8 < 0x140216705);
      FUN_140007d40(local_c38,puVar5,uVar17);
      (**(code **)(DAT_140014958 + 0x1a8))(DAT_140014980,local_d78,0xfff);
      FUN_140011400(&local_cb8,0,0x50);
      local_cb8 = 0x50;
      local_cb4 = 4;
      local_cb0 = 0x100000;
      local_d98 = (wchar_t *)(puVar5 + 0x5802);
      (**(code **)(DAT_140014958 + 0x2f0))(DAT_140014980,local_d78,&local_cb8,0);
      uVar6 = *(undefined8 *)(puVar5 + 0x5802);
      FUN_140011400(puVar5 + 0x4e,0,0x15808);
      local_d38 = 0;
      pcStack_d50 = FUN_140007b2c;
      local_d58 = 0x28;
      local_cc8 = 0;
      local_ce8 = 0;
      uStack_cd0 = 0;
      local_cd8 = local_d78;
      uStack_cf0 = 0;
      uStack_d40 = 0;
      local_d48 = 0x1000000c8;
      local_cf8 = 0x38;
      uStack_ce0 = 0x100000001;
      iVar4 = (**(code **)(DAT_140014958 + 0x9f0))
                        (DAT_140014980,&local_d58,&local_cf8,puVar5 + 0x5804);
      local_d88 = 0;
      local_d90 = (int *)CONCAT62(local_d90._2_6_,1);
      local_d98 = (wchar_t *)CONCAT62(local_d98._2_6_,0x58);
      (**(code **)(DAT_140014958 + 0x418))(DAT_140014980,local_d78,&DAT_140012208,puVar5 + 0x5824);
      *(undefined1 *)((longlong)puVar5 + 0x160f9) = 0;
      local_c64 = 2;
      local_c58 = 2;
      local_c54 = 2;
      local_c50 = 2;
      local_c48 = 2;
      local_c44 = 2;
      local_c40 = 0xffffffff;
      local_c68 = 0x30;
      local_c3c = 0xffffffff;
      local_c5c = 0;
      (**(code **)(DAT_140014958 + 0x298))(DAT_140014980,local_d78,&local_c68);
      if (DAT_140014730 == '\0') {
        *(undefined1 *)(puVar5 + 0x581f) = 0;
        *(char *)((longlong)puVar5 + 0x1c7b1) = cVar2;
        *(undefined1 *)((longlong)puVar5 + 0x1c7b2) = 0;
        if (DAT_140014288 == 0) {
          local_d98 = (wchar_t *)(puVar5 + 0x580a);
          (**(code **)(DAT_140014958 + 0xa8))(DAT_140014980,uVar6,0x4400000,0);
          local_d98 = (wchar_t *)(puVar5 + 0x580c);
          (**(code **)(DAT_140014958 + 0xa8))(DAT_140014980,uVar6,0x20000,0);
          FUN_1400067fc(local_d78);
        }
        else {
          uVar6 = (**(code **)(DAT_140014958 + 0x6e8))(DAT_140014980);
          lVar9 = (**(code **)(DAT_140014958 + 0x650))(DAT_140014980,uVar6,PTR_DAT_140014090);
          FUN_1400067fc(local_d78);
          *(undefined8 *)(puVar5 + 0x580a) = *(undefined8 *)(lVar9 + 0x16028);
          *(undefined8 *)(puVar5 + 0x580c) = *(undefined8 *)(lVar9 + 0x16030);
          *(longlong *)(puVar5 + 0x5806) = DAT_140014288;
          lVar9 = (**(code **)(DAT_140014958 + 0x650))
                            (DAT_140014980,DAT_140014288,PTR_DAT_140014040);
          uVar14 = 1;
          puVar5[0x5814] = *(undefined4 *)(lVar9 + 0x10);
          *(undefined4 **)(lVar9 + 0x18 + (ulonglong)*(uint *)(lVar9 + 0x10) * 8) = puVar5;
          *(int *)(lVar9 + 0x10) = *(int *)(lVar9 + 0x10) + 1;
          *(undefined1 *)(puVar5 + 0x5815) = 1;
          if (1 < *(uint *)(lVar9 + 0x10)) {
            do {
              lVar7 = (**(code **)(DAT_140014958 + 0x650))
                                (DAT_140014980,
                                 *(undefined8 *)(*(longlong *)(lVar9 + 0x18 + uVar14 * 8) + 0x16020)
                                 ,PTR_DAT_140014040);
              uVar15 = 0;
              uVar11 = 0;
              if (*(int *)(lVar9 + 0x10) != 0) {
                do {
                  uVar10 = (int)uVar11 + 1;
                  *(undefined8 *)(lVar7 + 0x18 + uVar11 * 8) =
                       *(undefined8 *)(lVar9 + 0x18 + uVar11 * 8);
                  uVar15 = *(uint *)(lVar9 + 0x10);
                  uVar11 = (ulonglong)uVar10;
                } while (uVar10 < uVar15);
              }
              uVar10 = (int)uVar14 + 1;
              uVar14 = (ulonglong)uVar10;
              *(uint *)(lVar7 + 0x10) = uVar15;
            } while (uVar10 < *(uint *)(lVar9 + 0x10));
          }
        }
        if (*(char *)((longlong)puVar5 + 0x1c7b1) != '\0') {
          uVar6 = (**(code **)(DAT_140014958 + 0xf8))(DAT_140014980,local_d78);
          *(undefined8 *)(puVar5 + 0x71ee) = uVar6;
          lVar9 = (**(code **)(DAT_140014958 + 0x100))(DAT_140014980,local_d78);
          *(longlong *)(puVar5 + 0x71f0) = lVar9;
          if (((*(longlong *)(puVar5 + 0x71ee) != 0) && (lVar9 != 0)) &&
             (iVar3 = FUN_140008434(local_d78), iVar3 == 0)) {
            *(undefined1 *)((longlong)puVar5 + 0x1c7b2) = 1;
          }
        }
        if (-1 < iVar4) {
          FUN_1405d9448(local_d78);
        }
      }
      else {
        *(undefined1 *)(puVar5 + 0x581f) = 1;
      }
    }
  }
  return local_d78;
}



// ===== FUN_1400067fc @ 1400067fc =====

/* WARNING: Function: __security_check_cookie replaced with injection: security_check_cookie */
/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

void FUN_1400067fc(undefined8 param_1)

{
  longlong *plVar1;
  longlong lVar2;
  longlong lVar3;
  int iVar4;
  undefined1 auStack_1d8 [32];
  undefined8 local_1b8;
  undefined4 local_1b0 [2];
  undefined8 *local_1a8;
  undefined4 local_1a0 [2];
  undefined1 *local_198;
  undefined8 local_190;
  undefined8 uStack_188;
  undefined8 local_180;
  undefined8 uStack_178;
  undefined8 local_170;
  undefined8 uStack_168;
  undefined *local_160;
  undefined4 local_158;
  undefined4 local_154;
  undefined4 local_150;
  undefined4 local_14c;
  undefined4 local_148;
  undefined4 local_144;
  undefined4 local_140;
  undefined4 uStack_13c;
  undefined4 uStack_138;
  undefined8 uStack_134;
  undefined4 local_12c;
  undefined4 uStack_128;
  undefined4 local_124;
  undefined4 local_120;
  undefined4 local_11c;
  undefined4 local_118;
  undefined4 local_114;
  undefined4 local_110;
  undefined4 local_10c;
  undefined4 local_108;
  undefined4 local_104;
  undefined4 local_100;
  undefined4 local_fc;
  undefined4 local_f8;
  undefined4 local_f4;
  undefined4 local_f0;
  undefined4 local_e8;
  undefined8 local_e4;
  undefined4 local_dc;
  undefined4 local_d8;
  undefined4 local_d4;
  undefined8 local_d0;
  undefined4 local_c8;
  undefined4 local_c4;
  int local_c0;
  int local_bc;
  undefined8 local_b8;
  undefined8 uStack_b0;
  undefined8 local_a8;
  undefined8 uStack_a0;
  undefined8 local_98;
  undefined8 uStack_90;
  undefined8 local_88;
  undefined8 uStack_80;
  undefined8 local_78;
  undefined8 uStack_70;
  undefined4 local_68;
  undefined4 uStack_64;
  undefined4 uStack_60;
  undefined4 uStack_5c;
  undefined2 local_58;
  undefined1 local_48 [24];
  ulonglong local_30;
  
  local_30 = DAT_140014180 ^ (ulonglong)auStack_1d8;
  local_58 = DAT_140011700;
  local_1a8 = &local_b8;
  local_198 = local_48;
  local_b8 = _DAT_1400116a0;
  uStack_b0 = _UNK_1400116a8;
  local_a8 = _DAT_1400116b0;
  uStack_a0 = _UNK_1400116b8;
  local_98 = _DAT_1400116c0;
  uStack_90 = _UNK_1400116c8;
  local_88 = _DAT_1400116d0;
  uStack_80 = _UNK_1400116d8;
  local_78 = _DAT_1400116e0;
  uStack_70 = _UNK_1400116e8;
  iVar4 = DAT_140014290 * 0x10000 + 0x1ffff;
  local_68 = _DAT_1400116f0;
  uStack_64 = _UNK_1400116f4;
  uStack_60 = _UNK_1400116f8;
  uStack_5c = _UNK_1400116fc;
  local_1b0[0] = 0x620060;
  local_1a0[0] = 0x140000;
  lVar2 = (**(code **)(DAT_140014958 + 0x650))(DAT_140014980,param_1,PTR_DAT_140014090);
  FUN_14000a3d0();
  local_1b8 = (**(code **)(DAT_140014958 + 0x690))(DAT_140014980,param_1);
  (**(code **)(DAT_140014958 + 0x210))(DAT_140014980,local_1b8,0x2a);
  (**(code **)(DAT_140014958 + 0x6a0))(DAT_140014980,local_1b8,local_1b0);
  (**(code **)(DAT_140014958 + 0x6b0))(DAT_140014980,local_1b8,local_1b0);
  (**(code **)(DAT_140014958 + 0x6b8))(DAT_140014980,local_1b8,local_1b0);
  RtlIntegerToUnicodeString(iVar4,10,local_1a0);
  (**(code **)(DAT_140014958 + 0x6a8))(DAT_140014980,local_1b8,local_1a0);
  local_180 = 0;
  local_160 = PTR_DAT_140014040;
  uStack_188 = 0;
  local_190 = 0x38;
  local_170 = 0;
  uStack_168 = 0;
  uStack_178 = 0x100000001;
  (**(code **)(DAT_140014958 + 0x1f8))(DAT_140014980,local_1b8);
  plVar1 = (longlong *)(lVar2 + 0x16018);
  (**(code **)(DAT_140014958 + 600))(DAT_140014980,&local_1b8,&local_190,plVar1);
  *(longlong *)(lVar2 + 0x16020) = *plVar1;
  if (DAT_140014288 == 0) {
    DAT_140014288 = *plVar1;
  }
  DAT_140014290 = DAT_140014290 + 1;
  lVar3 = (**(code **)(DAT_140014958 + 0x650))(DAT_140014980,*plVar1,PTR_DAT_140014040);
  *(undefined4 *)(lVar3 + 0x10) = 0;
  *(int *)(lVar3 + 4) = iVar4;
  *(undefined4 *)(lVar2 + 0x16050) = 0;
  *(longlong *)(lVar3 + 0x18 + (ulonglong)*(uint *)(lVar3 + 0x10) * 8) = lVar2;
  *(int *)(lVar3 + 0x10) = *(int *)(lVar3 + 0x10) + 1;
  *(undefined8 *)(lVar3 + 0xa8) = param_1;
  local_e8 = 0x30;
  local_e4 = 2;
  local_d8 = 2;
  local_d4 = 2;
  local_d0 = 2;
  local_c8 = 2;
  local_c4 = 2;
  local_dc = 0;
  local_c0 = iVar4;
  local_bc = iVar4;
  (**(code **)(DAT_140014958 + 0x298))(DAT_140014980,*plVar1,&local_e8);
  local_118 = 0xffffffff;
  local_114 = 0xffffffff;
  local_110 = 0xffffffff;
  local_12c = (undefined4)_DAT_140012270;
  _local_12c = CONCAT44(4,local_12c);
  local_124 = 4;
  local_140 = 2;
  uStack_134 = _UNK_140012278;
  local_120 = 5;
  local_10c = 5;
  local_158 = 0x50;
  local_154 = 2;
  local_150 = 2;
  local_14c = 2;
  local_148 = 2;
  local_144 = 2;
  local_11c = 7;
  uStack_138 = 1;
  uStack_13c = local_12c;
  (**(code **)(DAT_140014958 + 0x2a0))(DAT_140014980,*plVar1,&local_158);
  FUN_14000a430(*plVar1,lVar3);
  (**(code **)(DAT_140014958 + 0x428))(DAT_140014980,param_1,*plVar1);
  local_108 = 0x1c;
  local_104 = 2;
  local_100 = 2;
  local_fc = 2;
  local_f4 = 2;
  local_f0 = 2;
  local_f8 = 1;
  (**(code **)(DAT_140014958 + 0xe8))(DAT_140014980,*plVar1,&local_108);
  (**(code **)(DAT_140014958 + 0x188))(DAT_140014980,*plVar1,1,1);
  (**(code **)(DAT_140014958 + 0x188))(DAT_140014980,*plVar1,2,1);
  (**(code **)(DAT_140014958 + 0x188))(DAT_140014980,*plVar1,3,1);
  return;
}



// ===== FUN_140008f34 @ 140008f34 =====

undefined8 FUN_140008f34(undefined8 param_1,undefined8 param_2,undefined8 param_3)

{
  undefined4 uVar1;
  undefined1 uVar2;
  ushort uVar3;
  int iVar4;
  uint uVar5;
  longlong lVar6;
  char *pcVar7;
  undefined8 uVar8;
  uint *puVar9;
  uint uVar10;
  byte local_38 [4];
  ushort local_34 [2];
  uint local_30;
  uint local_2c;
  
  lVar6 = (**(code **)(DAT_140014958 + 0x650))(DAT_140014980,param_1,PTR_DAT_140014090);
  if (*(int *)(lVar6 + 0x16068) == 2) {
    (**(code **)(lVar6 + 0x1c2d0))(*(undefined8 *)(lVar6 + 0x1c2a0),0,local_38,0x34,1);
    uVar3 = (ushort)local_38[0];
    local_30 = 0;
    local_2c = 0;
    uVar5 = 1;
    uVar10 = 0;
    do {
      uVar10 = uVar10 + 1;
      (**(code **)(lVar6 + 0x1c2d0))(*(undefined8 *)(lVar6 + 0x1c2a0),0,local_34,uVar3,2);
      if ((char)local_34[0] == '\x11') {
        puVar9 = &local_30;
LAB_140008fe9:
        (**(code **)(lVar6 + 0x1c2d0))(*(undefined8 *)(lVar6 + 0x1c2a0),0,puVar9,local_38[0] + 2,2);
      }
      else {
        if ((char)local_34[0] == '\x05') {
          puVar9 = &local_2c;
          goto LAB_140008fe9;
        }
        if (10 < uVar10) {
          uVar5 = 0;
        }
      }
      uVar3 = local_34[0] >> 8;
      local_38[0] = (byte)(local_34[0] >> 8);
    } while ((char)uVar5 != '\0');
    if (((local_30 & 0x8000) != 0) && (uVar2 = 1, (local_2c & 1) == 0)) goto LAB_14000903e;
  }
  uVar2 = 0;
LAB_14000903e:
  uVar10 = 0;
  *(undefined1 *)(lVar6 + 0xb5) = uVar2;
  iVar4 = (**(code **)(DAT_140014958 + 0x980))(DAT_140014980,param_3);
  if (iVar4 != 0) {
    do {
      pcVar7 = (char *)(**(code **)(DAT_140014958 + 0x988))(DAT_140014980,param_3,uVar10);
      uVar8 = (**(code **)(DAT_140014958 + 0x988))(DAT_140014980,param_2,uVar10);
      if (*pcVar7 == '\x02') {
        FUN_1400079a4(lVar6,uVar8,pcVar7);
      }
      else if ((*pcVar7 == '\x03') && (*(longlong *)(lVar6 + 0x10) == 0)) {
        uVar8 = *(undefined8 *)(pcVar7 + 4);
        *(undefined8 *)(lVar6 + 8) = uVar8;
        uVar1 = *(undefined4 *)(pcVar7 + 0xc);
        *(undefined4 *)(lVar6 + 0x18) = uVar1;
        uVar8 = MmMapIoSpace(uVar8,uVar1,0);
        *(undefined8 *)(lVar6 + 0x10) = uVar8;
      }
      uVar10 = uVar10 + 1;
      uVar5 = (**(code **)(DAT_140014958 + 0x980))(DAT_140014980,param_3);
    } while (uVar10 < uVar5);
  }
  return 0;
}



// ===== FUN_140001008 @ 140001008 =====

void FUN_140001008(undefined8 param_1,longlong param_2,char *param_3,char *param_4,char param_5,
                  char param_6)

{
  byte bVar1;
  longlong lVar2;
  undefined4 uVar3;
  undefined4 uVar4;
  undefined4 uVar5;
  undefined2 uVar6;
  
  if ((param_3 == (char *)0x0) || (*param_3 != -0x5f)) {
    if (*param_4 == '4') {
      uVar3 = *(undefined4 *)(param_4 + 4);
      uVar4 = *(undefined4 *)(param_4 + 8);
      uVar5 = *(undefined4 *)(param_4 + 0xc);
      *(undefined4 *)(param_2 + 0x114) = *(undefined4 *)param_4;
      *(undefined4 *)(param_2 + 0x118) = uVar3;
      *(undefined4 *)(param_2 + 0x11c) = uVar4;
      *(undefined4 *)(param_2 + 0x120) = uVar5;
      *(undefined4 *)(param_2 + 0x124) = *(undefined4 *)(param_4 + 0x10);
      *(undefined1 *)(param_2 + 0x112) = 0x14;
      *(char *)(param_2 + 0x111) = param_4[2];
      *(char *)(param_2 + 0x113) = param_4[3];
      if (((((param_4[2] & 1U) != 0) && ((*(uint *)(param_2 + 0x154) & 0x200000) == 0)) &&
          (lVar2 = *(longlong *)(param_2 + 0x160), (*(uint *)(lVar2 + 0x2c0) & 0x400) == 0)) &&
         (*(int *)(lVar2 + 0x724) = *(int *)(lVar2 + 0x724) + 1, 1 < *(uint *)(lVar2 + 0x724))) {
        FUN_140001df0(param_1,*(undefined4 *)(lVar2 + 8));
      }
    }
    else {
      *(undefined2 *)(param_2 + 0x111) = 0;
      *(undefined1 *)(param_2 + 0x113) = 0;
    }
  }
  else {
    *(undefined1 *)(param_2 + 0x112) = 8;
    *(undefined8 *)(param_2 + 0x114) = *(undefined8 *)param_3;
    *(char *)(param_2 + 0x111) = param_3[2];
    *(char *)(param_2 + 0x113) = param_3[3];
  }
  if ((param_5 != '\0') && (param_6 == '\0')) {
    *(uint *)(param_2 + 0x154) = *(uint *)(param_2 + 0x154) | 0x20000;
  }
  if ((*(byte *)(param_2 + 0x111) & 0x20) == 0) {
    if ((*(byte *)(param_2 + 0x111) & 1) == 0) {
      if (param_5 == '\0') {
        *(undefined1 *)(param_2 + 0x110) = 0;
        return;
      }
      *(undefined1 *)(param_2 + 0x110) = 2;
LAB_140001160:
      *(undefined2 *)(param_2 + 0x10e) = 0;
      return;
    }
    bVar1 = *(byte *)(param_2 + 0x113);
    *(undefined1 *)(param_2 + 0x110) = 2;
    if ((bVar1 & 0x40) == 0) {
      if ((bVar1 & 0x10) == 0) {
        if (((bVar1 & 4) != 0) || (-1 < (char)bVar1)) goto LAB_140001160;
        uVar6 = 0x4703;
      }
      else {
        uVar6 = 0x1400;
      }
    }
    else {
      uVar6 = 0x1100;
    }
  }
  else {
    *(undefined1 *)(param_2 + 0x110) = 2;
    uVar6 = 0x4400;
  }
  *(undefined2 *)(param_2 + 0x10e) = uVar6;
  return;
}



// ===== FUN_140008638 @ 140008638 =====

/* WARNING: Function: __security_check_cookie replaced with injection: security_check_cookie */

undefined8 FUN_140008638(undefined8 param_1,undefined8 param_2)

{
  byte bVar1;
  char cVar2;
  int iVar3;
  longlong lVar4;
  uint uVar5;
  ulonglong uVar6;
  undefined1 auStack_948 [32];
  wchar_t *local_928;
  int *local_920;
  int local_918 [2];
  undefined4 local_910;
  undefined4 local_90c;
  code *local_908;
  code *local_900;
  undefined1 *local_8f8;
  undefined4 local_8e8 [2];
  code *local_8e0;
  code *local_8d8;
  code *local_8d0;
  code *local_8c8;
  code *local_8c0;
  code *local_8b8;
  code *local_888;
  code *local_880;
  code *local_878;
  undefined1 *local_870;
  undefined4 local_858;
  undefined4 local_854;
  undefined4 local_850;
  undefined4 local_84c;
  undefined4 local_848;
  undefined4 local_844;
  undefined4 local_840;
  wchar_t local_838 [512];
  undefined1 local_438 [1024];
  ulonglong local_38;
  
  local_38 = DAT_140014180 ^ (ulonglong)auStack_948;
  FUN_140011400(local_838,0,0x400);
  FUN_140011400(local_438,0,0x400);
  local_920 = local_918;
  local_928 = local_838;
  (**(code **)(DAT_140014958 + 0x3f0))(DAT_140014980,param_2,1,0x400);
  local_920 = local_918;
  local_928 = (wchar_t *)local_438;
  (**(code **)(DAT_140014958 + 0x3f0))(DAT_140014980,param_2,2,0x400);
  uVar6 = 0;
  if (local_918[0] != 9) {
    do {
      wcsncmp(local_838 + uVar6,L"CC_010802",9);
      uVar5 = (int)uVar6 + 1;
      uVar6 = (ulonglong)uVar5;
    } while (uVar5 < local_918[0] - 9U);
  }
  iVar3 = wcsncmp(local_838,L"PCI\\VEN_1022&DEV_7916",0x15);
  if (((iVar3 == 0) || (iVar3 = wcsncmp(local_838,L"PCI\\VEN_1022&DEV_7917",0x15), iVar3 == 0)) ||
     (iVar3 = wcsncmp(local_838,L"PCI\\VEN_1022&DEV_7905",0x15), iVar3 == 0)) {
    bVar1 = (byte)DAT_1400146b0 & 2;
  }
  else {
    iVar3 = wcsncmp(local_838,L"PCI\\VEN_1022&DEV_43BD",0x15);
    if (iVar3 != 0) goto LAB_1400087b9;
    bVar1 = (byte)DAT_1400146b0 & 1;
  }
  if (bVar1 == 0) {
    return 0;
  }
LAB_1400087b9:
  if ((((undefined **)PTR_LOOP_1400140a0 != &PTR_LOOP_1400140a0) &&
      ((*(uint *)(PTR_LOOP_1400140a0 + 0x2c) & 2) != 0)) && (3 < (byte)PTR_LOOP_1400140a0[0x29])) {
    FUN_140008368(*(undefined8 *)(PTR_LOOP_1400140a0 + 0x18),0xc,&DAT_1400122e8);
  }
  FUN_140011400(local_8e8,0,0x90);
  local_8c0 = FUN_140008f34;
  local_8e8[0] = 0x90;
  local_8b8 = FUN_14000911c;
  local_8e0 = FUN_140008a48;
  local_8d0 = FUN_140008bc0;
  local_8d8 = FUN_140008b44;
  local_8c8 = FUN_140008d88;
  local_880 = FUN_140009210;
  local_878 = FUN_140009210;
  local_888 = FUN_14000924c;
  local_870 = &LAB_14000918c;
  (**(code **)(DAT_140014958 + 0x1b8))(DAT_140014980,param_2,local_8e8);
  local_90c = 0;
  local_908 = FUN_140008dd8;
  local_910 = 0x20;
  local_900 = FUN_140008dec;
  local_8f8 = &LAB_140009174;
  (**(code **)(DAT_140014958 + 0x400))(DAT_140014980,param_2,&local_910);
  (**(code **)(DAT_140014958 + 0x1f8))(DAT_140014980,param_2);
  lVar4 = FUN_140005ff4(param_2);
  if (lVar4 != 0) {
    local_858 = 0x1c;
    local_854 = 2;
    local_850 = 2;
    local_84c = 2;
    local_844 = 2;
    local_840 = 2;
    local_848 = 1;
    (**(code **)(DAT_140014958 + 0xe8))(DAT_140014980,lVar4,&local_858);
    (**(code **)(DAT_140014958 + 0x188))(DAT_140014980,lVar4,1,1);
    (**(code **)(DAT_140014958 + 0x188))(DAT_140014980,lVar4,4,1);
    (**(code **)(DAT_140014958 + 0x188))(DAT_140014980,lVar4,2,1);
    (**(code **)(DAT_140014958 + 0x188))(DAT_140014980,lVar4,3,1);
    FUN_140007ba0(lVar4,param_1);
    cVar2 = FUN_14000a3d0();
    if (cVar2 != '\0') {
      FUN_140008540(lVar4);
    }
    if ((((undefined **)PTR_LOOP_1400140a0 != &PTR_LOOP_1400140a0) &&
        ((*(uint *)(PTR_LOOP_1400140a0 + 0x2c) & 2) != 0)) && (3 < (byte)PTR_LOOP_1400140a0[0x29]))
    {
      FUN_140008368(*(undefined8 *)(PTR_LOOP_1400140a0 + 0x18),0xd,&DAT_1400122e8);
    }
    return 0;
  }
  return 0xc000009a;
}



// ===== FUN_140003048 @ 140003048 =====

void FUN_140003048(longlong param_1,uint param_2)

{
  longlong lVar1;
  byte bVar2;
  uint uVar3;
  uint uVar4;
  uint uVar5;
  longlong lVar6;
  longlong lVar7;
  bool bVar8;
  uint uVar9;
  longlong *plVar10;
  uint uVar11;
  ulonglong uVar12;
  uint uVar13;
  uint uVar14;
  
  bVar8 = false;
  uVar12 = 0;
  lVar1 = param_1 + 0x138;
  lVar6 = *(longlong *)(param_1 + 0x158b8);
  bVar2 = *(byte *)(param_1 + 0x158fc);
  if (*(char *)(param_1 + 0x15921) == '\0') {
    uVar13 = (uint)bVar2;
    if (param_2 != 0xffffffff) {
      uVar12 = (ulonglong)param_2;
      uVar13 = param_2 + 1;
      if ((*(uint *)(param_1 + 0xb0) < (uint)bVar2) && (*(uint *)(param_1 + 0xb0) - 1 <= param_2)) {
        uVar13 = (uint)bVar2;
      }
    }
    plVar10 = (longlong *)(uVar12 * 0x728 + lVar1);
    for (; (uint)uVar12 < uVar13; uVar12 = (ulonglong)((uint)uVar12 + 1)) {
      if ((*(ulonglong *)(param_1 + 0x158e0) >> (uVar12 & 0x3f) & 1) != 0) {
        lVar7 = *plVar10;
        uVar11 = *(uint *)(lVar7 + 0x10);
        if (uVar11 == 0) {
          FUN_14000330c(param_1,plVar10);
          *(int *)(lVar6 + 8) = 1 << ((byte)uVar12 & 0x1f);
        }
        else {
          *(uint *)(lVar7 + 0x10) = uVar11;
          uVar14 = 1 << ((byte)uVar12 & 0x1f);
          *(uint *)(lVar6 + 8) = uVar14;
          if (((*(uint *)(plVar10 + 0x58) & 0x400) != 0) && ((uVar11 >> 0x1e & 1) != 0)) {
            uVar11 = uVar11 & 0xbfffffff;
            FUN_14000438c(lVar1,uVar12);
            FUN_14000403c(lVar1,uVar12);
            *(uint *)(plVar10 + 0x5a) =
                 *(uint *)(plVar10 + 0x5a) |
                 *(uint *)((longlong)plVar10 + 0x2c4) | *(uint *)(plVar10 + 0x59);
            *(uint *)((longlong)plVar10 + 0x2d4) =
                 *(uint *)((longlong)plVar10 + 0x2d4) |
                 *(uint *)((longlong)plVar10 + 0x2c4) | *(uint *)(plVar10 + 0x59);
          }
          if (((uVar11 & 0x79800000) != 0) &&
             ((*(char *)((longlong)plVar10 + 0x302) != '\x0f' || ((uVar11 & 0x4800000) == 0)))) {
            FUN_140001318(param_1,uVar12);
          }
          FUN_140001868(param_1,plVar10,0,0);
          FUN_14000330c(param_1,plVar10);
          if ((uVar11 & 0xfffffff4) != 0) {
            uVar3 = *(uint *)(lVar7 + 0x30);
            uVar4 = *(uint *)(lVar7 + 0x28);
            if ((uVar3 & 3) != 0) {
              *(int *)(plVar10 + 0x5e) = (int)plVar10[0x5e] + 1;
              *(undefined4 *)(lVar7 + 0x30) = 3;
            }
            uVar5 = *(uint *)(plVar10 + 0x5b);
            uVar9 = uVar5 & 0xff;
            if ((*(uint *)(plVar10 + 0x58) & 0x1000) == 0) {
              if (((*(uint *)(param_1 + 0x16174) & uVar14) == uVar14) ||
                 ((*(uint *)(param_1 + 0x16170) & uVar14) == uVar14)) {
                bVar8 = true;
              }
              else {
                bVar8 = false;
              }
            }
            if (-1 < (int)uVar5) {
              if (bVar8) {
                if (((((uVar11 >> 0x1b & 1) != 0) && (uVar9 - 10 < 2)) ||
                    (((uVar11 & 0x40) != 0 && (uVar9 == 0xb)))) ||
                   (((uVar9 < 0x13 && ((0x58400U >> (uVar5 & 0x1f) & 1) != 0)) &&
                    ((uVar4 & 0xf) == 0)))) goto LAB_140003284;
              }
              else if ((((uVar11 & 0x8400000) != 0) && (uVar9 < 0x13)) &&
                      ((0x58c00U >> (uVar5 & 0x1f) & 1) != 0)) {
LAB_140003284:
                if ((*(byte *)(plVar10 + 0x65) & 0x10) == 0) {
                  if ((*(uint *)(param_1 + 0x158f8) & 0x8000000) != 0) {
                    *(uint *)(lVar7 + 0x18) = *(uint *)(lVar7 + 0x18) & 0xfffffffd;
                  }
                  if ((*(int *)((longlong)plVar10 + 0x2c4) == 0) && ((int)plVar10[0x59] == 0)) {
                    FUN_140002ef0(param_1,uVar12);
                  }
                  else {
                    FUN_140001318(param_1,uVar12);
                  }
                }
              }
            }
            *(uint *)(lVar7 + 0x30) = uVar3;
            *(uint *)(lVar7 + 0x10) = uVar11 & 0xfffffff4;
          }
        }
      }
      plVar10 = plVar10 + 0xe5;
    }
  }
  return;
}



// ===== FUN_1400028f8 @ 1400028f8 =====

void FUN_1400028f8(longlong param_1)

{
  longlong lVar1;
  int iVar2;
  int iVar3;
  int iVar4;
  char cVar5;
  byte bVar6;
  uint uVar7;
  longlong lVar8;
  int iVar9;
  longlong *plVar10;
  undefined8 *puVar11;
  uint uVar12;
  ulonglong uVar13;
  int iVar14;
  uint uVar15;
  
  lVar1 = param_1 + 0x138;
  if ((*(char *)(param_1 + 0x1c7b0) == '\0') && (*(char *)(param_1 + 0x15923) != '\0')) {
    *(undefined1 *)(param_1 + 0x15923) = 0;
    FUN_14000273c();
  }
  iVar2 = *(int *)(param_1 + 0x15900);
  puVar11 = (undefined8 *)(param_1 + 0x838);
  iVar9 = 0;
  iVar14 = 0;
  uVar13 = 0;
  uVar15 = iVar2 + 1;
  *(uint *)(param_1 + 0x15900) = uVar15;
  do {
    uVar12 = (uint)uVar13;
    if ((*(ulonglong *)(param_1 + 0x158e0) >> (uVar13 & 0x3f) & 1) != 0) {
      plVar10 = puVar11 + -0xe0;
      lVar8 = *plVar10;
      iVar9 = iVar9 + 1;
      uVar7 = *(uint *)(puVar11 + -0x85) & 0xff;
      if (uVar7 < 8) {
        if (uVar7 == 7) {
          if (*(uint *)((longlong)puVar11 + -0x424) <= uVar15) {
            if (DAT_140014731 == '\0') {
              (**(code **)(DAT_140014958 + 0x9e0))(DAT_140014980,*puVar11);
            }
            lVar8 = (ulonglong)*(byte *)((longlong)puVar11 + -0x3ff) * 0x728;
            if (*(char *)(lVar8 + 0x324 + lVar1) == '\0') {
              *(undefined1 *)(lVar8 + 0x324 + lVar1) = 1;
              if (DAT_140014731 == '\0') {
                (**(code **)(DAT_140014958 + 0x9e8))(DAT_140014980,*puVar11);
              }
              FUN_1400058dc(param_1,uVar13 & 0xff);
            }
            else {
LAB_140002a55:
              if (DAT_140014731 == '\0') {
                (**(code **)(DAT_140014958 + 0x9e8))(DAT_140014980,*puVar11);
              }
            }
          }
        }
        else if ((char)*(uint *)(puVar11 + -0x85) == '\0') {
LAB_140002c1f:
          if (((uVar12 < 8) &&
              (uVar7 = 1 << ((byte)uVar13 & 0x1f), (*(uint *)(param_1 + 0x16170) & uVar7) == uVar7))
             && (bVar6 = *(byte *)(puVar11 + -0x7b), (bVar6 & 2) == 0)) {
            if ((*(uint *)(param_1 + 0x158f8) & 0x4000000) != 0) {
              *(uint *)(lVar8 + 0x18) = *(uint *)(lVar8 + 0x18) | 0xc000000;
              bVar6 = *(byte *)(puVar11 + -0x7b);
            }
            *(byte *)(puVar11 + -0x7b) = bVar6 | 2;
          }
LAB_140002c6c:
          iVar14 = iVar14 + 1;
        }
        else if (uVar7 == 1) {
          FUN_140002ef0(param_1,uVar13);
        }
        else if (uVar7 == 2) {
          *(uint *)((longlong)puVar11 + -0x414) = uVar12;
          if (*(uint *)((longlong)puVar11 + -0x424) <= uVar15) {
            iVar3 = *(int *)(param_1 + 0x15900);
            lVar8 = uVar13 * 0x728;
            *(undefined4 *)(*(longlong *)(lVar8 + lVar1) + 0x14) = 0x70000000;
            *(int *)(lVar8 + 0x2dc + lVar1) = iVar3 + 3;
            *(undefined1 *)(lVar8 + 0x2d8 + lVar1) = 3;
          }
        }
        else if (uVar7 == 3) {
          FUN_1400014dc(param_1,uVar13);
        }
        else if (uVar7 == 4) {
          if (*(uint *)((longlong)puVar11 + -0x424) <= uVar15) {
            FUN_14000484c(plVar10,uVar13,param_1);
          }
        }
        else if (uVar7 == 5) {
          FUN_140004578(plVar10,uVar13,lVar1);
          *(int *)((longlong)puVar11 + -0x424) = iVar2 + 2;
        }
        else if (uVar7 == 6) {
          if (DAT_140014731 == '\0') {
            (**(code **)(DAT_140014958 + 0x9e0))(DAT_140014980,*puVar11);
          }
          lVar8 = (ulonglong)*(byte *)((longlong)puVar11 + -0x3ff) * 0x728;
          if (*(char *)(lVar8 + 0x324 + lVar1) != '\0') goto LAB_140002a55;
          *(undefined1 *)(lVar8 + 0x324 + lVar1) = 1;
          if (DAT_140014731 == '\0') {
            (**(code **)(DAT_140014958 + 0x9e8))(DAT_140014980,*puVar11);
          }
          FUN_140004c10(param_1,uVar13 & 0xff);
        }
      }
      else if (uVar7 == 8) {
        if (*(uint *)((longlong)puVar11 + -0x424) <= uVar15) {
          if ((*(uint *)(param_1 + 0x158f8) & 0x8000000) != 0) {
            *(uint *)(lVar8 + 0x18) = *(uint *)(lVar8 + 0x18) | 2;
          }
          *(int *)((longlong)puVar11 + -0x424) = iVar2 + 0x97;
          *(undefined4 *)(lVar8 + 0x14) = 0x70000000;
          *(undefined1 *)(puVar11 + -0x85) = 9;
        }
      }
      else if (uVar7 == 9) {
        *(byte *)(puVar11 + -0x7b) = *(byte *)(puVar11 + -0x7b) & 0xfd;
        if (*(uint *)((longlong)puVar11 + -0x424) <= uVar15) {
          *(int *)(puVar11 + -0x83) = *(int *)(puVar11 + -0x83) + 1;
          *(uint *)(puVar11 + -0x85) = *(uint *)(puVar11 + -0x85) | 0x40000000;
          FUN_140002ef0(param_1,uVar13);
          goto LAB_140002c6c;
        }
        if ((*(uint *)(lVar8 + 0x20) & 0x89) == 0) {
          *(undefined4 *)((longlong)puVar11 + -0x414) = *(undefined4 *)(puVar11 + -0xdf);
          *(undefined1 *)(puVar11 + -0x85) = 10;
          *(undefined4 *)(lVar8 + 0x14) = 0x7d40005b;
          if ((*(uint *)(lVar8 + 0x24) & 0xffff0000) == 0xeb140000) {
            *(uint *)(puVar11 + -0x88) = *(uint *)(puVar11 + -0x88) & 0xfffefcff | 0x40400;
          }
          *(uint *)(puVar11 + -0x85) = *(uint *)(puVar11 + -0x85) & 0xbfffffff;
          FUN_1400016b0(param_1,uVar13);
          FUN_1400036e0(param_1,plVar10);
        }
      }
      else if (uVar7 == 10) {
        if (DAT_140014731 == '\0') {
          (**(code **)(DAT_140014958 + 0x9e0))(DAT_140014980,*puVar11);
        }
        iVar3 = *(int *)(lVar8 + 0x38);
        iVar4 = *(int *)(lVar8 + 0x34);
        if (DAT_140014731 == '\0') {
          (**(code **)(DAT_140014958 + 0x9e8))(DAT_140014980,*puVar11);
        }
        if (((*(int *)((longlong)puVar11 + -0x43c) == 0) &&
            (cVar5 = FUN_1400027dc(puVar11 + -0xe0), cVar5 != '\0')) && (puVar11[2] != 0)) {
          FUN_1400036e0(param_1,puVar11 + -0xe0);
        }
        if (uVar12 < 8) {
          if ((*(uint *)(puVar11 + -0x88) & 0x2000) == 0) {
            uVar7 = 1 << ((byte)uVar13 & 0x1f);
            if (((*(uint *)(param_1 + 0x16170) & uVar7) == uVar7) &&
               (bVar6 = *(byte *)(puVar11 + -0x7b), (bVar6 & 6) == 4)) {
              if ((*(uint *)(param_1 + 0x158f8) & 0x4000000) != 0) {
                *(uint *)(lVar8 + 0x18) = *(uint *)(lVar8 + 0x18) | 0xc000000;
                bVar6 = *(byte *)(puVar11 + -0x7b);
              }
              *(byte *)(puVar11 + -0x7b) = bVar6 | 2;
            }
          }
          else if (*(uint *)((longlong)puVar11 + -0x424) <= uVar15) {
            *(int *)((longlong)puVar11 + -0x424) = iVar2 + 0x33;
          }
        }
        if (((*(int *)(puVar11 + -0x87) != iVar4) || (iVar3 != *(int *)((longlong)puVar11 + -0x43c))
            ) && (*(char *)(param_1 + 0x15922) != '\0')) {
          (**(code **)(param_1 + 0x16120))(param_1,0xffffffff);
        }
        iVar14 = iVar14 + 1;
      }
      else {
        if (uVar7 == 0xb) goto LAB_140002c1f;
        if (uVar7 == 0xc) {
          if (*(char *)((longlong)puVar11 + -0x3fe) != '\x0f') {
            plVar10 = (longlong *)((ulonglong)*(byte *)((longlong)puVar11 + -0x3ff) * 0x728 + lVar1)
            ;
          }
          if (((int)plVar10[0x59] != *(int *)(lVar8 + 0x34)) ||
             (*(int *)(lVar8 + 0x38) != *(int *)((longlong)plVar10 + 0x2c4))) {
            FUN_140001868(param_1);
          }
        }
        else if (uVar7 == 0xe) goto LAB_140002c6c;
      }
      if (((*(byte *)(puVar11 + -0x7b) & 8) != 0) && (*(uint *)(puVar11 + -0x84) <= uVar15)) {
        *(byte *)(puVar11 + -0x7b) = *(byte *)(puVar11 + -0x7b) & 0xf7 | 0x10;
        FUN_14000b500(param_1,puVar11 + -0xe0);
      }
    }
    puVar11 = puVar11 + 0xe5;
    uVar13 = (ulonglong)(uVar12 + 1);
    if (0x2f < uVar12 + 1) {
      if ((*(char *)(param_1 + 0x15920) == '\0') && (iVar14 == iVar9)) {
        *(undefined1 *)(param_1 + 0x15920) = 1;
      }
      return;
    }
  } while( true );
}



// ===== FUN_140010488 @ 140010488 =====

undefined4 FUN_140010488(longlong *param_1,undefined8 param_2,longlong *param_3)

{
  longlong lVar1;
  uint uVar2;
  int iVar3;
  longlong lVar4;
  byte *pbVar5;
  uint uVar6;
  undefined4 uVar7;
  uint uVar8;
  undefined4 uVar9;
  int iVar10;
  undefined4 uVar11;
  
  if (param_1 == (longlong *)0x0) {
    return 0xffffffff;
  }
  lVar1 = *param_1;
  if (lVar1 == 0) {
    return 0xffffffff;
  }
  if (param_3 == (longlong *)0x0) {
    return 0xffffffff;
  }
  iVar3 = *(int *)((longlong)param_3 + 0xc);
  if (iVar3 == 0) {
    return 0xffffffff;
  }
  if (*param_3 == 1) {
    if ((*(uint *)(param_3 + 1) & 1) != 0) {
      *(int *)(param_1 + 2) = iVar3;
      lVar4 = param_3[2];
      *(undefined4 *)(param_1 + 4) = 0;
      param_1[3] = lVar4;
      lVar4 = 0;
      iVar10 = 0;
      goto LAB_140010535;
    }
    *(undefined4 *)(param_1 + 2) = 0;
    param_1[3] = 0;
    iVar3 = 0;
  }
  else {
    if (*param_3 != 2) {
      return 0xffffffff;
    }
    if ((*(uint *)(param_3 + 3) & 1) == 0) {
      return 0xffffffff;
    }
    iVar3 = *(int *)((longlong)param_3 + 0x1c);
    *(int *)(param_1 + 2) = iVar3;
    param_1[3] = param_3[4];
  }
  iVar10 = *(int *)((longlong)param_3 + 0xc);
  *(int *)(param_1 + 4) = iVar10;
  lVar4 = param_3[2];
LAB_140010535:
  param_1[5] = lVar4;
  *(int *)(param_1 + 6) = iVar3 + iVar10;
  iVar3 = FUN_140010850(lVar1);
  if (iVar3 != 0) {
    return 0xffffffff;
  }
  lVar4 = *param_1;
  uVar7 = 0xffffffff;
  uVar9 = 0xffffffff;
  uVar11 = 0xffffffff;
  if (lVar4 != 0) {
    iVar3 = 0x14;
    while (uVar11 = uVar7, (*(uint *)(lVar4 + 0x70) & 0x20) != 0) {
      if (iVar3 == 0) goto LAB_1400106f0;
      iVar3 = iVar3 + -1;
      KeStallExecutionProcessor(1000);
    }
    *(undefined4 *)(lVar4 + 0x30) = 0;
    *(undefined4 *)(lVar4 + 0x6c) = 1;
    iVar3 = FUN_14001075c(lVar4);
    if (iVar3 == 0) {
      uVar11 = uVar9;
      if ((int)param_1[6] != 0) {
        while (uVar8 = *(uint *)(param_1 + 4), uVar11 = uVar9, uVar8 != 0) {
          uVar2 = *(uint *)(param_1 + 7);
          if (uVar8 <= *(uint *)(param_1 + 7)) {
            uVar2 = uVar8;
          }
          uVar8 = 0;
          if (uVar2 != 0) {
            iVar3 = (int)param_1[6];
            do {
              if ((iVar3 == 1) && ((int)param_1[2] == 0)) {
                pbVar5 = (byte *)param_1[5];
                uVar6 = *pbVar5 | 0x200;
              }
              else {
                pbVar5 = (byte *)param_1[5];
                uVar6 = (uint)*pbVar5;
              }
              uVar8 = uVar8 + 1;
              param_1[5] = (longlong)(pbVar5 + 1);
              *(uint *)(lVar4 + 0x10) = uVar6;
              *(int *)(param_1 + 6) = (int)param_1[6] + -1;
              *(int *)(param_1 + 4) = (int)param_1[4] + -1;
              iVar3 = (int)param_1[6];
            } while (uVar8 < uVar2);
          }
          iVar3 = 100;
          while ((*(uint *)(lVar4 + 0x70) & 4) == 0) {
            if (iVar3 == 0) goto LAB_140010632;
            iVar3 = iVar3 + -1;
            KeStallExecutionProcessor(0x19);
          }
          uVar9 = 0;
        }
      }
      uVar8 = *(uint *)(param_1 + 2);
      if (uVar8 != 0) {
        iVar3 = 10;
        do {
          uVar2 = *(uint *)((longlong)param_1 + 0x34);
          if (uVar8 <= *(uint *)((longlong)param_1 + 0x34)) {
            uVar2 = uVar8;
          }
          uVar6 = 0;
          if (uVar2 != 0) {
            do {
              uVar11 = 0x100;
              if (uVar8 == 1) {
                uVar11 = 0x300;
              }
              uVar6 = uVar6 + 1;
              *(undefined4 *)(lVar4 + 0x10) = uVar11;
              *(int *)(param_1 + 2) = (int)param_1[2] + -1;
              *(int *)(param_1 + 6) = (int)param_1[6] + -1;
              uVar8 = *(uint *)(param_1 + 2);
            } while (uVar6 < uVar2);
          }
          iVar10 = 10;
          while (*(uint *)(lVar4 + 0x78) < uVar2) {
            if (iVar10 == 0) goto LAB_140010632;
            iVar10 = iVar10 + -1;
            KeStallExecutionProcessor(0x19);
          }
          uVar11 = 0;
          uVar8 = 0;
          if (uVar2 != 0) {
            do {
              uVar6 = *(uint *)(lVar4 + 0x34);
              if ((uVar6 & 0x40) != 0) {
                uVar6 = *(uint *)(lVar4 + 0x80);
              }
              if ((uVar6 & 0x43) != 0) {
                *(undefined4 *)(lVar4 + 0x6c) = 0;
                FUN_14001075c(lVar4,1);
                while ((iVar3 != 0 &&
                       (KeStallExecutionProcessor(0x19), (*(uint *)(lVar4 + 0x9c) & 1) != 0))) {
                  iVar3 = iVar3 + -1;
                }
LAB_140010632:
                *(undefined4 *)(lVar4 + 0x10) = 0x200;
                uVar11 = uVar7;
                goto LAB_1400106f0;
              }
              uVar8 = uVar8 + 1;
              *(char *)param_1[3] = (char)*(undefined4 *)(lVar4 + 0x10);
              param_1[3] = param_1[3] + 1;
            } while (uVar8 < uVar2);
          }
          uVar8 = *(uint *)(param_1 + 2);
        } while (uVar8 != 0);
      }
    }
  }
LAB_1400106f0:
  *(undefined4 *)(lVar1 + 0x6c) = 0;
  FUN_14001075c(lVar1,1);
  return uVar11;
}



// ===== FUN_14000fafc @ 14000fafc =====

undefined1 FUN_14000fafc(longlong param_1,longlong param_2,undefined8 param_3,undefined8 param_4)

{
  byte bVar1;
  longlong lVar2;
  longlong lVar3;
  undefined1 uVar4;
  
  *(undefined8 *)(param_2 + 0x58) = 0;
  *(undefined8 *)(param_2 + 0x48) = 0;
  bVar1 = *(byte *)(param_2 + 0x14c);
  lVar2 = *(longlong *)(param_2 + 0x178);
  if ((bVar1 == 1) || (bVar1 == 4)) {
    lVar3 = *(longlong *)(param_1 + 0x15948 + (ulonglong)*(uint *)(param_1 + 0x15968) * 8);
    if ((*(ushort *)(lVar3 + 6) + 1) % (uint)*(ushort *)(lVar3 + 10) == (uint)*(ushort *)(lVar3 + 8)
       ) goto LAB_14000fc55;
    if (lVar2 == 0) {
      uVar4 = FUN_14000ed2c(param_1,param_2,param_3,param_4);
      return uVar4;
    }
  }
  else {
    if (bVar1 == 6) {
      uVar4 = FUN_14001026c(param_1,param_2,param_3,param_4);
      return uVar4;
    }
    if (bVar1 < 10) {
LAB_14000fc01:
      FUN_14000c900(param_2,param_4,2,0x2000);
      return 1;
    }
    if (bVar1 < 0xc) {
      lVar3 = *(longlong *)(param_1 + 0x15948 + (ulonglong)*(uint *)(param_1 + 0x15968) * 8);
      if ((*(ushort *)(lVar3 + 6) + 1) % (uint)*(ushort *)(lVar3 + 10) ==
          (uint)*(ushort *)(lVar3 + 8)) {
LAB_14000fc55:
        FUN_14000e960(param_1,param_2,param_3,param_4);
        return 0;
      }
      if (lVar2 == 0) {
        uVar4 = FUN_14000ec64(param_1,param_2,param_3,param_4);
        return uVar4;
      }
    }
    else {
      if (bVar1 != 0xc) goto LAB_14000fc01;
      lVar3 = *(longlong *)(param_1 + 0x15948 + (ulonglong)*(uint *)(param_1 + 0x15968) * 8);
      if ((*(ushort *)(lVar3 + 6) + 1) % (uint)*(ushort *)(lVar3 + 10) ==
          (uint)*(ushort *)(lVar3 + 8)) goto LAB_14000fc55;
      if (lVar2 == 0) {
        uVar4 = FUN_14000e2c8(param_1,param_2,param_3,param_4);
        return uVar4;
      }
    }
  }
  FUN_14000f178(param_1,param_2,param_4);
  return 1;
}



// ===== FUN_14000c0bc @ 14000c0bc =====

undefined1 FUN_14000c0bc(longlong param_1,undefined8 param_2,longlong param_3,undefined8 param_4)

{
  ushort uVar1;
  uint uVar2;
  int iVar3;
  ushort *puVar4;
  longlong lVar5;
  longlong lVar6;
  undefined8 uVar7;
  longlong lVar8;
  longlong lVar9;
  undefined8 *puVar10;
  undefined1 uVar11;
  
  puVar10 = (undefined8 *)(param_1 + 0x15940);
  uVar11 = 0;
  lVar8 = FUN_14000c2fc(*puVar10,FUN_14000c060);
  if (lVar8 != 0) {
    uVar11 = 1;
    if (param_3 == 0) {
      *(uint *)(param_1 + 0x15cd8) = *(uint *)(param_1 + 0x15cd8) & 0xffffffef;
      FUN_14000ce5c(puVar10,FUN_14000cd50);
    }
    else {
      *(int *)(lVar8 + 0x18) = (int)param_3;
      *(int *)(lVar8 + 0x1c) = (int)((ulonglong)param_3 >> 0x20);
      *(undefined8 *)(lVar8 + 0x70) = param_4;
      uVar1 = *(ushort *)(lVar8 + 0x20);
      *(undefined4 *)(param_3 + 0xc4) = 0;
      *(uint *)(param_3 + 0xc0) = (uint)uVar1;
      *(undefined1 *)(lVar8 + 0x30) = 8;
      uVar2 = *(uint *)(param_3 + 0xc4);
      iVar3 = *(int *)(param_3 + 0xc0);
      *(undefined2 *)(lVar8 + 0x32) = *(undefined2 *)(lVar8 + 0x20);
      *(uint *)(lVar8 + 0x58) = iVar3 << 0x10 | uVar2 & 0xffff;
      puVar4 = (ushort *)*puVar10;
      uVar7 = *(undefined8 *)(lVar8 + 0x38);
      lVar5 = *(longlong *)(param_1 + 0x159a8);
      lVar6 = *(longlong *)(puVar4 + 0xc);
      lVar9 = (ulonglong)*(ushort *)(lVar8 + 0x22) * 0x40;
      *(undefined8 *)(lVar9 + lVar6) = *(undefined8 *)(lVar8 + 0x30);
      ((undefined8 *)(lVar9 + lVar6))[1] = uVar7;
      uVar7 = *(undefined8 *)(lVar8 + 0x48);
      puVar10 = (undefined8 *)(lVar9 + 0x10 + lVar6);
      *puVar10 = *(undefined8 *)(lVar8 + 0x40);
      puVar10[1] = uVar7;
      uVar7 = *(undefined8 *)(lVar8 + 0x58);
      puVar10 = (undefined8 *)(lVar9 + 0x20 + lVar6);
      *puVar10 = *(undefined8 *)(lVar8 + 0x50);
      puVar10[1] = uVar7;
      uVar7 = *(undefined8 *)(lVar8 + 0x68);
      puVar10 = (undefined8 *)(lVar9 + 0x30 + lVar6);
      *puVar10 = *(undefined8 *)(lVar8 + 0x60);
      puVar10[1] = uVar7;
      *(uint *)(lVar5 + 0x1000 + (ulonglong)*puVar4 * 8) =
           (*(ushort *)(lVar8 + 0x22) + 1) % (uint)puVar4[5];
    }
  }
  return uVar11;
}



// ===== FUN_14000dd44 @ 14000dd44 =====

void FUN_14000dd44(longlong param_1,char param_2)

{
  ulonglong *puVar1;
  undefined2 *puVar2;
  undefined8 uVar3;
  longlong lVar4;
  ushort uVar5;
  ulonglong *puVar6;
  ulonglong *puVar7;
  ulonglong uVar8;
  int *piVar9;
  ushort uVar10;
  int iVar11;
  uint uVar12;
  undefined4 uVar13;
  ulonglong uVar14;
  longlong lVar15;
  longlong *plVar16;
  ulonglong uVar17;
  int iVar18;
  char cVar19;
  ulonglong *local_60;
  undefined8 local_58;
  undefined8 local_50;
  longlong local_48;
  ulonglong *local_40;
  
  cVar19 = *(char *)(param_1 + 0x15d00);
  uVar14 = 0;
  local_50 = *(undefined8 *)(param_1 + 0x16008);
  iVar18 = 0xffff;
  *(char *)(param_1 + 0x1c7b0) = param_2;
  *(undefined4 *)(param_1 + 0x16070) = 1;
  local_48 = param_1;
  if (cVar19 == '\0') {
    LOCK();
    UNLOCK();
    iVar18 = DAT_140014744 + 1;
    DAT_140014744 = DAT_140014744 + 1;
    (**(code **)(DAT_140014958 + 0xdb0))
              (DAT_140014980,*(undefined8 *)(param_1 + 0x20),0,0,0xb21,
               "Y:\\RC-932\\RC_932_00255\\fulcrum\\rc\\platforms\\rcbottom\\rcbottom\\nvme.c");
    if (0x10 < iVar18) {
      cVar19 = '\x01';
    }
  }
  *(undefined2 *)(param_1 + 0x15d00) = 0;
  *(longlong *)(param_1 + 0x15f98) = param_1;
  *(undefined2 *)(param_1 + 0x15ce0) = 0;
  uVar13 = 0;
  *(undefined4 *)(param_1 + 0x15cd8) = 0;
  *(undefined4 *)(param_1 + 0x15974) = 0;
  puVar1 = *(ulonglong **)(param_1 + 0x10);
  *(ulonglong **)(param_1 + 0x15978) = puVar1;
  *(ulonglong **)(param_1 + 0x159a8) = puVar1;
  if (*(char *)(param_1 + 0xb4) == '\x01') {
    uVar13 = *(undefined4 *)(param_1 + 0xb0);
  }
  *(undefined4 *)(param_1 + 0x15d10) = uVar13;
  if ((*(int *)(param_1 + 0xb0) == 1) || (param_2 != '\0')) {
    *(undefined4 *)(param_1 + 0x15d1c) = 1;
    iVar11 = 1;
    *(undefined4 *)(param_1 + 0x15d10) = 1;
  }
  else {
    iVar11 = *(int *)(param_1 + 0xb0) + -1;
    *(int *)(param_1 + 0x15d1c) = iVar11;
    if (4 < iVar11) {
      *(undefined4 *)(param_1 + 0x15d1c) = 4;
      iVar11 = 4;
    }
  }
  *(longlong *)(param_1 + 0x15940) = param_1 + 0x15d20;
  if (iVar11 < 1) {
    iVar11 = *(int *)(param_1 + 0x15d1c);
  }
  else {
    plVar16 = (longlong *)(param_1 + 0x15948);
    uVar17 = uVar14;
    do {
      iVar11 = (int)uVar17;
      uVar12 = iVar11 + 1;
      uVar17 = (ulonglong)uVar12;
      *plVar16 = param_1 + 0x15d98 + (longlong)iVar11 * 0x78;
      plVar16 = plVar16 + 1;
      iVar11 = *(int *)(param_1 + 0x15d1c);
    } while ((int)uVar12 < iVar11);
  }
  local_40 = puVar1;
  if (cVar19 == '\0') {
    (**(code **)(DAT_140014958 + 0xa8))
              (DAT_140014980,local_50,iVar11 * 0x30000 + 0x9fff,0,&local_58);
    puVar6 = (ulonglong *)(**(code **)(DAT_140014958 + 0xb8))(DAT_140014980,local_58);
    puVar7 = (ulonglong *)(**(code **)(DAT_140014958 + 0xb0))(DAT_140014980,local_58);
    *(ulonglong **)(param_1 + 0x1c318) = puVar7;
    *(int *)(param_1 + 0x1c320) = iVar11 * 0x30000 + 0x9000;
    *(ulonglong **)(param_1 + 0x159b0) = puVar6;
    *(ulonglong **)(param_1 + 0x159b8) = puVar7;
    local_60 = puVar7;
  }
  else {
    puVar7 = *(ulonglong **)(param_1 + 0x159b8);
    local_60 = puVar1;
    puVar6 = puVar1;
  }
  FUN_140011400(puVar7,0,0x1000);
  puVar2 = *(undefined2 **)(param_1 + 0x15940);
  uVar10 = (short)*puVar1 + 1;
  if ((short)*puVar1 == -1) {
    uVar10 = 0xffff;
  }
  *puVar2 = 0;
  if (cVar19 == '\0') {
    FUN_140011400(puVar2,0,0x78);
    uVar5 = uVar10;
    if (0x100 < uVar10) {
      uVar5 = 0x100;
    }
    puVar2[5] = uVar5;
    puVar2[1] = uVar5;
    if (0x400 < uVar10) {
      uVar10 = 0x400;
    }
    uVar8 = (longlong)puVar6 + 0x13ffU & 0xfffffffffffffc00;
    puVar2[6] = uVar10;
    *(ulonglong *)(puVar2 + 0x14) = uVar8;
    uVar17 = (longlong)local_60 + 0x13ffU & 0xfffffffffffffc00;
    *(ulonglong *)(puVar2 + 0xc) = uVar17;
    *(ulonglong *)(puVar2 + 0x18) = uVar8 + 0x4000;
    puVar6 = (ulonglong *)(uVar8 + 0x8000);
    *(ulonglong *)(puVar2 + 0x10) = uVar17 + 0x4000;
    local_60 = (ulonglong *)(uVar17 + 0x8000);
  }
  else {
    uVar17 = *(ulonglong *)(puVar2 + 0xc);
  }
  FUN_140011400(uVar17,0,0x4000);
  FUN_140011400(*(undefined8 *)(puVar2 + 0x10),0,0x4000);
  if (cVar19 == '\0') {
    *(undefined **)(puVar2 + 0x24) = &DAT_1402170c0 + (longlong)(iVar18 + -1) * 0xf000;
  }
  *(undefined4 *)(puVar2 + 8) = 0x10000;
  *(undefined4 *)(puVar2 + 2) = 0xffff;
  puVar2[4] = 0;
  lVar15 = 0x100;
  puVar2[7] = 0;
  uVar17 = uVar14;
  do {
    FUN_140011400(*(longlong *)(puVar2 + 0x24) + uVar17,0,0x78);
    lVar4 = local_48;
    uVar3 = local_50;
    uVar17 = uVar17 + 0x78;
    lVar15 = lVar15 + -1;
  } while (lVar15 != 0);
  lVar15 = local_48 + 0x15940;
  if (0 < *(int *)(local_48 + 0x15d1c)) {
    do {
      FUN_14000f454(lVar15,uVar3,iVar18,uVar14,cVar19,param_2,local_60,puVar6);
      local_60 = local_60 + 0x6000;
      puVar6 = puVar6 + 0x6000;
      uVar12 = (int)uVar14 + 1;
      uVar14 = (ulonglong)uVar12;
    } while ((int)uVar12 < *(int *)(lVar4 + 0x15d1c));
  }
  uVar14 = 0;
  piVar9 = &DAT_140216700;
  do {
    if (*piVar9 == *(int *)(local_48 + 0x16060)) {
      *(undefined1 *)(lVar4 + 0x15fa0) = 1;
      lVar15 = uVar14 * 0xe;
      *(undefined1 *)(lVar4 + 0x15fa1) = (&DAT_140216706)[lVar15];
      *(undefined *)(lVar4 + 0x15fa2) = (&DAT_140216707)[lVar15];
      *(undefined1 *)(lVar4 + 0x15fa3) = *(undefined1 *)((longlong)&DAT_140216708 + lVar15);
      break;
    }
    uVar12 = (int)uVar14 + 1;
    uVar14 = (ulonglong)uVar12;
    piVar9 = (int *)((longlong)piVar9 + 0xe);
  } while (uVar12 < 8);
  *(ulonglong *)(local_48 + 0x16088) = *local_40;
  if (*local_40 == 0xffffffffffffffff) {
    *(undefined2 *)(lVar4 + 0x15d00) = 0x101;
  }
  else {
    uVar14 = *local_40;
    *(undefined1 *)(local_48 + 0x16075) = 1;
    iVar18 = ((uint)(uVar14 >> 0x19) & 0x7f) * 5;
    *(int *)(local_48 + 0x16078) = iVar18;
    *(int *)(local_48 + 0x16080) = iVar18;
    *(undefined4 *)((longlong)local_40 + 0x14) = 0x460000;
  }
  return;
}



// ===== FUN_14000e59c @ 14000e59c =====

void FUN_14000e59c(longlong param_1)

{
  longlong lVar1;
  char cVar2;
  char cVar3;
  uint uVar4;
  undefined1 local_38 [32];
  
  lVar1 = *(longlong *)(param_1 + 0x159a8);
  if (*(char *)(param_1 + 0xb5) == '\0') {
    *(undefined4 *)(lVar1 + 0xc) = 0xffffffff;
  }
  FUN_14000ce5c(param_1 + 0x15940,FUN_14000cc28);
  cVar2 = FUN_140005f90(param_1,0,local_38);
  uVar4 = 0;
  do {
    if ((*(uint *)(param_1 + 0x15cd8) & 0x20) == 0) break;
    cVar3 = FUN_14000c82c(param_1,0xffffffff);
    if (cVar3 != '\0') {
      FUN_14000e820(param_1,0,0);
    }
    KeStallExecutionProcessor(1000);
    uVar4 = uVar4 + 1;
  } while (uVar4 < 2000);
  if (cVar2 != '\0') {
    KeReleaseInStackQueuedSpinLock(local_38);
  }
  *(uint *)(lVar1 + 0x14) = *(uint *)(lVar1 + 0x14) & 0xffff3fff;
  *(uint *)(lVar1 + 0x14) = *(uint *)(lVar1 + 0x14) | 0x4000;
  FUN_14000fdd8(param_1 + 0x15940,0x1c,8,0,4000);
  return;
}



// ===== FUN_14000e800 @ 14000e800 =====

void FUN_14000e800(undefined8 param_1,undefined4 param_2)

{
  FUN_14000e820(param_1,param_2,1);
  return;
}



// ===== FUN_14000e494 @ 14000e494 =====

void FUN_14000e494(longlong param_1)

{
  if (*(char *)(param_1 + 0x16074) != '\0') {
    FUN_14000fca4();
    return;
  }
  if (*(char *)(param_1 + 0x16075) != '\0') {
    FUN_14000fd10();
  }
  return;
}



// ===== FUN_14000c814 @ 14000c814 =====

undefined1 FUN_14000c814(longlong param_1)

{
  return *(undefined1 *)(param_1 + 0x15d00);
}



// ===== FUN_14000c82c @ 14000c82c =====

bool FUN_14000c82c(longlong param_1,int param_2)

{
  longlong lVar1;
  longlong lVar2;
  longlong *plVar3;
  bool bVar4;
  
  if (param_2 == -1) {
    lVar1 = *(longlong *)(param_1 + 0x15940);
    lVar2 = (longlong)*(int *)(param_1 + 0x15d1c);
    bVar4 = (*(ushort *)
              (*(longlong *)(lVar1 + 0x20) + 0xe + (ulonglong)*(ushort *)(lVar1 + 0x10) * 0x10) & 1)
            == *(ushort *)(lVar1 + 0x12);
    if (0 < lVar2) {
      plVar3 = (longlong *)(param_1 + 0x15948);
      do {
        lVar1 = *plVar3;
        plVar3 = plVar3 + 1;
        if ((*(ushort *)
              (*(longlong *)(lVar1 + 0x20) + 0xe + (ulonglong)*(ushort *)(lVar1 + 0x10) * 0x10) & 1)
            == *(ushort *)(lVar1 + 0x12)) {
          bVar4 = true;
        }
        lVar2 = lVar2 + -1;
      } while (lVar2 != 0);
    }
    if ((*(char *)(param_1 + 0xb5) == '\0') && (bVar4 != false)) {
      *(undefined4 *)(*(longlong *)(param_1 + 0x159a8) + 0xc) = 0xffffffff;
    }
  }
  else {
    if (*(char *)(param_1 + 0xb5) == '\0') {
      *(int *)(*(longlong *)(param_1 + 0x159a8) + 0xc) = 1 << ((byte)param_2 & 0x1f);
    }
    bVar4 = true;
  }
  return bVar4;
}



// ===== FUN_14000d06c @ 14000d06c =====

undefined1 FUN_14000d06c(longlong param_1,longlong param_2,undefined8 param_3,longlong param_4)

{
  char cVar1;
  longlong lVar2;
  undefined1 uVar3;
  
  cVar1 = *(char *)(param_2 + 0x14c);
  if ((cVar1 == '\x01') || (cVar1 == '\x04')) {
    lVar2 = *(longlong *)(param_1 + 0x15948 + (ulonglong)*(uint *)(param_1 + 0x15968) * 8);
    if ((*(ushort *)(lVar2 + 6) + 1) % (uint)*(ushort *)(lVar2 + 10) != (uint)*(ushort *)(lVar2 + 8)
       ) {
      uVar3 = FUN_14000ed2c(param_1,param_2,param_3,param_4);
      return uVar3;
    }
  }
  else {
    if (cVar1 != '\n') {
      *(undefined4 *)(param_2 + 0x100) = *(undefined4 *)(param_2 + 0x13c);
      *(undefined1 *)(param_2 + 0x110) = 2;
      *(undefined1 *)(param_2 + 0x112) = 0;
      if (param_4 != 0) {
        (**(code **)(DAT_140014958 + 0x838))(DAT_140014980,param_4,0);
        return 1;
      }
      FUN_1400097ac(param_2);
      return 1;
    }
    lVar2 = *(longlong *)(param_1 + 0x15948 + (ulonglong)*(uint *)(param_1 + 0x15968) * 8);
    if ((*(ushort *)(lVar2 + 6) + 1) % (uint)*(ushort *)(lVar2 + 10) != (uint)*(ushort *)(lVar2 + 8)
       ) {
      uVar3 = FUN_14000ec64();
      return uVar3;
    }
  }
  FUN_14000e960(param_1,param_2,param_3,param_4);
  return 0;
}



// ===== FUN_14001023c @ 14001023c =====

void FUN_14001023c(longlong param_1)

{
  if (*(char *)(param_1 + 0x15d01) == '\0') {
    FUN_14000c1e4();
  }
  return;
}



// ===== FUN_1400100c0 @ 1400100c0 =====

void FUN_1400100c0(longlong param_1,uint *param_2)

{
  uint uVar1;
  short sVar2;
  
  if (*(char *)(param_1 + 0x15fa0) != '\0') {
    if ((*param_2 & 1) == 0) {
      *(uint *)(param_1 + 0x15fa8) = *(uint *)(param_1 + 0x15fa4);
      uVar1 = *(uint *)(param_1 + 0x15fa4) | *param_2;
      *(uint *)(param_1 + 0x15fa4) = uVar1;
      uVar1 = ~param_2[1] & uVar1;
      *(uint *)(param_1 + 0x15fa4) = uVar1;
    }
    else {
      *(undefined4 *)(param_1 + 0x15fa4) = *(undefined4 *)(param_1 + 0x15fa8);
      *param_2 = *param_2 & 0xfffffffe;
      uVar1 = *(uint *)(param_1 + 0x15fa4);
    }
    sVar2 = ((uVar1 & 0xa0000c20) != 0) + 0x100;
    if ((uVar1 & 0x1200) != 0) {
      sVar2 = 0x102;
    }
    if ((uVar1 >> 0x11 & 1) != 0) {
      sVar2 = 0x103;
    }
    FUN_1400107b8(*(undefined1 *)(param_1 + 0x15fa1),*(undefined1 *)(param_1 + 0x15fa2),
                  *(undefined1 *)(param_1 + 0x15fa3),sVar2);
  }
  return;
}



// ===== FUN_14000f454 @ 14000f454 =====

void FUN_14000f454(longlong param_1,undefined8 param_2,int param_3,int param_4,char param_5,
                  char param_6,longlong param_7,longlong param_8)

{
  short *psVar1;
  ushort uVar2;
  longlong lVar3;
  ushort uVar4;
  longlong lVar5;
  
  lVar5 = 0x100;
  uVar4 = (ushort)**(undefined8 **)(param_1 + 0x68);
  psVar1 = *(short **)(param_1 + 8 + (longlong)param_4 * 8);
  if (param_5 == '\0') {
    FUN_140011400(psVar1,0,0x78);
    *(longlong *)(psVar1 + 0xc) = param_7;
    *psVar1 = (short)param_4 + 1;
    uVar2 = uVar4;
    if (0x100 < uVar4) {
      uVar2 = 0x100;
    }
    psVar1[5] = uVar2;
    if (0x400 < uVar4) {
      uVar4 = 0x400;
    }
    psVar1[1] = uVar2;
    *(longlong *)(psVar1 + 0x14) = param_8;
    psVar1[6] = uVar4;
    *(longlong *)(psVar1 + 0x18) = param_8 + 0x8000;
    *(longlong *)(psVar1 + 0x1c) = param_8 + 0x10000;
    *(longlong *)(psVar1 + 0x10) = param_7 + 0x8000;
    *(longlong *)(psVar1 + 0x20) = param_7 + 0x10000;
  }
  else {
    param_7 = *(longlong *)(psVar1 + 0xc);
  }
  FUN_140011400(param_7,0,0x4000);
  FUN_140011400(*(undefined8 *)(psVar1 + 0x10),0,0x4000);
  FUN_140011400(*(undefined8 *)(psVar1 + 0x20),0,0x20000);
  lVar3 = 0;
  if (param_5 == '\0') {
    *(undefined **)(psVar1 + 0x24) =
         &DAT_14021e8c0 + (longlong)(param_4 + (param_3 + -1) * 4) * 0xf000;
  }
  psVar1[8] = 0;
  psVar1[9] = 1;
  psVar1[2] = -1;
  psVar1[3] = 0;
  psVar1[4] = 0;
  psVar1[7] = 0;
  *(undefined4 *)(param_1 + 0x28) = 0;
  *(undefined4 *)(param_1 + 0x30) = 0;
  do {
    FUN_140011400(*(longlong *)(psVar1 + 0x24) + lVar3,0,0x78);
    lVar3 = lVar3 + 0x78;
    lVar5 = lVar5 + -1;
  } while (lVar5 != 0);
  if ((param_5 == '\0') && (param_6 == '\0')) {
    *(undefined1 *)(param_1 + 0x3d4) = 1;
    FUN_14000f608(psVar1,param_2,psVar1[5]);
  }
  return;
}



// ===== FUN_140001ed8 @ 140001ed8 =====

void FUN_140001ed8(longlong param_1)

{
  longlong lVar1;
  longlong lVar2;
  longlong lVar3;
  longlong *plVar4;
  uint uVar5;
  ulonglong uVar6;
  uint uVar7;
  
  plVar4 = (longlong *)(param_1 + 0x158);
  uVar7 = 0;
  do {
    if (((*(ulonglong *)(param_1 + 0x158e0) >> ((ulonglong)uVar7 & 0x3f) & 1) != 0) &&
       (plVar4[0xdc] != 0)) {
      if (DAT_140014731 == '\0') {
        (**(code **)(DAT_140014958 + 0x9e0))(DAT_140014980);
      }
      lVar1 = *plVar4;
      *(undefined1 *)(plVar4 + 0x57) = 0;
      FUN_140001180(param_1,uVar7);
      uVar6 = 0;
      if (*(char *)(param_1 + 0x158fd) != '\0') {
        do {
          lVar2 = plVar4[uVar6 + 1];
          if (lVar2 != 0) {
            lVar3 = plVar4[uVar6 + 0x21];
            FUN_140001008(param_1,lVar2,lVar1 + 0x58,lVar1 + 0x40,1,0);
            plVar4[uVar6 + 1] = 0;
            plVar4[uVar6 + 0x21] = 0;
            if (lVar3 == 0) {
              FUN_1400097ac(lVar2);
            }
            else {
              (**(code **)(DAT_140014958 + 0x838))(DAT_140014980,lVar3,0);
            }
          }
          uVar5 = (int)uVar6 + 1;
          uVar6 = (ulonglong)uVar5;
        } while (uVar5 < *(byte *)(param_1 + 0x158fd));
      }
      *(undefined8 *)((longlong)plVar4 + 0x2a4) = 0;
      plVar4[0x56] = 0;
      if (DAT_140014731 == '\0') {
        (**(code **)(DAT_140014958 + 0x9e8))(DAT_140014980,plVar4[0xdc]);
      }
    }
    plVar4 = plVar4 + 0xe5;
    uVar7 = uVar7 + 1;
  } while (uVar7 < 0x30);
  return;
}



// ===== FUN_14000be7c @ 14000be7c =====

void FUN_14000be7c(longlong param_1,int param_2,undefined1 *param_3)

{
  uint uVar1;
  longlong lVar2;
  
  param_2 = param_2 * 4;
  lVar2 = 4;
  do {
    uVar1 = param_2 * 8;
    param_2 = param_2 + 1;
    *param_3 = *(undefined1 *)((ulonglong)uVar1 + 0x16188 + param_1);
    param_3[1] = *(undefined1 *)((ulonglong)(uVar1 + 1) + 0x16188 + param_1);
    param_3[2] = *(undefined1 *)((ulonglong)(uVar1 + 2) + 0x16188 + param_1);
    param_3[3] = *(undefined1 *)((ulonglong)(uVar1 + 3) + 0x16188 + param_1);
    param_3[4] = *(undefined1 *)((ulonglong)(uVar1 + 4) + 0x16188 + param_1);
    param_3[5] = *(undefined1 *)((ulonglong)(uVar1 + 5) + 0x16188 + param_1);
    param_3[6] = *(undefined1 *)((ulonglong)(uVar1 + 6) + 0x16188 + param_1);
    lVar2 = lVar2 + -1;
    param_3 = param_3 + 8;
  } while (lVar2 != 0);
  return;
}



// ===== FUN_14000b730 @ 14000b730 =====

/* WARNING: Function: __security_check_cookie replaced with injection: security_check_cookie */
/* WARNING: Type propagation algorithm not settling */

void FUN_14000b730(undefined8 param_1,longlong param_2)

{
  int iVar1;
  uint uVar2;
  ulonglong uVar3;
  uint uVar4;
  uint uVar5;
  undefined4 *puVar6;
  uint uVar7;
  uint uVar8;
  undefined1 auStack_178 [32];
  uint local_158 [4];
  int local_148 [2];
  int local_140;
  ushort local_13a;
  undefined4 local_138;
  undefined2 local_134;
  undefined1 local_132 [234];
  ulonglong local_48;
  
  local_48 = DAT_140014180 ^ (ulonglong)auStack_178;
  local_158[1] = 0x100;
  iVar1 = FUN_14000aa04(param_1,&DAT_1400119f0,local_158);
  uVar7 = 0;
  uVar5 = 0;
  if (iVar1 == 0) {
    iVar1 = FUN_14000aa04(param_1,"PRID._ADR",local_158);
    uVar8 = local_158[0];
    if (iVar1 == 0) {
      iVar1 = FUN_14000aa04(param_1,"PRID.P_D0._ADR",local_158);
      if (iVar1 == 0) {
        iVar1 = FUN_14000a904(param_1,"PRID.P_D0._GTF",local_148,local_158 + 1);
        if (((iVar1 == 0) && (local_148[0] == 0x426f6541)) && (local_140 == 1)) {
          puVar6 = &local_138;
          uVar4 = 0;
          if (local_13a >> 2 != 0) {
            do {
              uVar2 = local_158[0] + uVar8;
              if ((uVar2 < 8) && (uVar4 < 4)) {
                uVar3 = (ulonglong)((uVar4 + uVar2 * 4) * 8);
                *(undefined4 *)(uVar3 + param_2) = *puVar6;
                *(undefined2 *)(uVar3 + 4 + param_2) = *(undefined2 *)(puVar6 + 1);
                *(undefined1 *)(uVar3 + 6 + param_2) = *(undefined1 *)((longlong)puVar6 + 6);
              }
              puVar6 = puVar6 + 2;
              uVar4 = uVar4 + 1;
            } while (uVar4 < local_13a >> 2);
          }
        }
      }
      iVar1 = FUN_14000aa04(param_1,"PRID.P_D1._ADR",local_158);
      if (iVar1 == 0) {
        iVar1 = FUN_14000a904(param_1,"PRID.P_D1._GTF",local_148,local_158 + 1);
        if (((iVar1 == 0) && (local_148[0] == 0x426f6541)) && (local_140 == 1)) {
          puVar6 = &local_138;
          uVar4 = 0;
          if (local_13a >> 2 != 0) {
            do {
              uVar2 = local_158[0] + uVar8;
              if ((uVar2 < 8) && (uVar4 < 4)) {
                uVar3 = (ulonglong)((uVar4 + uVar2 * 4) * 8);
                *(undefined4 *)(uVar3 + param_2) = *puVar6;
                *(undefined2 *)(uVar3 + 4 + param_2) = *(undefined2 *)(puVar6 + 1);
                *(undefined1 *)(uVar3 + 6 + param_2) = *(undefined1 *)((longlong)puVar6 + 6);
              }
              puVar6 = puVar6 + 2;
              uVar4 = uVar4 + 1;
            } while (uVar4 < local_13a >> 2);
          }
        }
      }
    }
    iVar1 = FUN_14000aa04(param_1,"SECD._ADR",local_158);
    uVar8 = local_158[0];
    if (iVar1 == 0) {
      iVar1 = FUN_14000aa04(param_1,"SECD.S_D0._ADR",local_158);
      if (iVar1 == 0) {
        iVar1 = FUN_14000a904(param_1,"SECD.S_D0._GTF",local_148,local_158 + 1);
        if (((iVar1 == 0) && (local_148[0] == 0x426f6541)) && (local_140 == 1)) {
          puVar6 = &local_138;
          uVar4 = 0;
          if (local_13a >> 2 != 0) {
            do {
              uVar2 = local_158[0] + uVar8;
              if ((uVar2 < 8) && (uVar4 < 4)) {
                uVar3 = (ulonglong)((uVar4 + uVar2 * 4) * 8);
                *(undefined4 *)(uVar3 + param_2) = *puVar6;
                *(undefined2 *)(uVar3 + 4 + param_2) = *(undefined2 *)(puVar6 + 1);
                *(undefined1 *)(uVar3 + 6 + param_2) = *(undefined1 *)((longlong)puVar6 + 6);
              }
              puVar6 = puVar6 + 2;
              uVar4 = uVar4 + 1;
            } while (uVar4 < local_13a >> 2);
          }
        }
      }
      iVar1 = FUN_14000aa04(param_1,"SECD.S_D1._ADR",local_158);
      if (iVar1 == 0) {
        iVar1 = FUN_14000a904(param_1,"SECD.S_D1._GTF",local_148,local_158 + 1);
        if (((iVar1 == 0) && (local_148[0] == 0x426f6541)) && (local_140 == 1)) {
          puVar6 = &local_138;
          uVar4 = 0;
          if (local_13a >> 2 != 0) {
            do {
              uVar2 = local_158[0] + uVar8;
              if ((uVar2 < 8) && (uVar4 < 4)) {
                uVar3 = (ulonglong)((uVar4 + uVar2 * 4) * 8);
                *(undefined4 *)(uVar3 + param_2) = *puVar6;
                *(undefined2 *)(uVar3 + 4 + param_2) = *(undefined2 *)(puVar6 + 1);
                *(undefined1 *)(uVar3 + 6 + param_2) = *(undefined1 *)((longlong)puVar6 + 6);
              }
              puVar6 = puVar6 + 2;
              uVar4 = uVar4 + 1;
            } while (uVar4 < local_13a >> 2);
          }
        }
      }
    }
    iVar1 = FUN_14000aa04(param_1,"PRT0._ADR",local_158);
    if (iVar1 == 0) {
      uVar8 = local_158[0] >> 0x10;
      iVar1 = FUN_14000a904(param_1,"PRT0._GTF",local_148,local_158 + 1);
      if (((iVar1 == 0) && (local_148[0] == 0x426f6541)) && (local_140 == 1)) {
        puVar6 = &local_138;
        uVar4 = uVar5;
        if (local_13a >> 3 != 0) {
          do {
            if ((uVar8 < 8) && (uVar4 < 4)) {
              uVar3 = (ulonglong)((uVar4 + uVar8 * 4) * 8);
              *(undefined4 *)(uVar3 + param_2) = *puVar6;
              *(undefined2 *)(uVar3 + 4 + param_2) = *(undefined2 *)(puVar6 + 1);
              *(undefined1 *)(uVar3 + 6 + param_2) = *(undefined1 *)((longlong)puVar6 + 6);
            }
            puVar6 = puVar6 + 2;
            uVar4 = uVar4 + 1;
          } while (uVar4 < local_13a >> 3);
        }
      }
    }
    iVar1 = FUN_14000aa04(param_1,"PRT0._ADR",local_158);
    if (iVar1 == 0) {
      uVar8 = local_158[0] >> 0x10;
      iVar1 = FUN_14000a904(param_1,"PRT0._GTF",local_148,local_158 + 1);
      if (((iVar1 == 0) && (local_148[0] == 0x426f6541)) && (local_140 == 1)) {
        puVar6 = &local_138;
        uVar4 = uVar5;
        if (local_13a >> 3 != 0) {
          do {
            if ((uVar8 < 8) && (uVar4 < 4)) {
              uVar3 = (ulonglong)((uVar4 + uVar8 * 4) * 8);
              *(undefined4 *)(uVar3 + param_2) = *puVar6;
              *(undefined2 *)(uVar3 + 4 + param_2) = *(undefined2 *)(puVar6 + 1);
              *(undefined1 *)(uVar3 + 6 + param_2) = *(undefined1 *)((longlong)puVar6 + 6);
            }
            puVar6 = puVar6 + 2;
            uVar4 = uVar4 + 1;
          } while (uVar4 < local_13a >> 3);
        }
      }
    }
    iVar1 = FUN_14000aa04(param_1,"PRT1._ADR",local_158);
    if (iVar1 == 0) {
      uVar8 = local_158[0] >> 0x10;
      iVar1 = FUN_14000a904(param_1,"PRT1._GTF",local_148,local_158 + 1);
      if (((iVar1 == 0) && (local_148[0] == 0x426f6541)) && (local_140 == 1)) {
        puVar6 = &local_138;
        uVar4 = uVar5;
        if (local_13a >> 3 != 0) {
          do {
            if ((uVar8 < 8) && (uVar4 < 4)) {
              uVar3 = (ulonglong)((uVar4 + uVar8 * 4) * 8);
              *(undefined4 *)(uVar3 + param_2) = *puVar6;
              *(undefined2 *)(uVar3 + 4 + param_2) = *(undefined2 *)(puVar6 + 1);
              *(undefined1 *)(uVar3 + 6 + param_2) = *(undefined1 *)((longlong)puVar6 + 6);
            }
            puVar6 = puVar6 + 2;
            uVar4 = uVar4 + 1;
          } while (uVar4 < local_13a >> 3);
        }
      }
    }
    iVar1 = FUN_14000aa04(param_1,"PRT2._ADR",local_158);
    if (iVar1 == 0) {
      uVar8 = local_158[0] >> 0x10;
      iVar1 = FUN_14000a904(param_1,"PRT2._GTF",local_148,local_158 + 1);
      if (((iVar1 == 0) && (local_148[0] == 0x426f6541)) && (local_140 == 1)) {
        puVar6 = &local_138;
        uVar4 = uVar5;
        if (local_13a >> 3 != 0) {
          do {
            if ((uVar8 < 8) && (uVar4 < 4)) {
              uVar3 = (ulonglong)((uVar4 + uVar8 * 4) * 8);
              *(undefined4 *)(uVar3 + param_2) = *puVar6;
              *(undefined2 *)(uVar3 + 4 + param_2) = *(undefined2 *)(puVar6 + 1);
              *(undefined1 *)(uVar3 + 6 + param_2) = *(undefined1 *)((longlong)puVar6 + 6);
            }
            puVar6 = puVar6 + 2;
            uVar4 = uVar4 + 1;
          } while (uVar4 < local_13a >> 3);
        }
      }
    }
    iVar1 = FUN_14000aa04(param_1,"PRT3._ADR",local_158);
    if (iVar1 == 0) {
      uVar8 = local_158[0] >> 0x10;
      iVar1 = FUN_14000a904(param_1,"PRT3._GTF",local_148,local_158 + 1);
      if (((iVar1 == 0) && (local_148[0] == 0x426f6541)) && (local_140 == 1)) {
        puVar6 = &local_138;
        uVar4 = uVar5;
        if (local_13a >> 3 != 0) {
          do {
            if ((uVar8 < 8) && (uVar4 < 4)) {
              uVar3 = (ulonglong)((uVar4 + uVar8 * 4) * 8);
              *(undefined4 *)(uVar3 + param_2) = *puVar6;
              *(undefined2 *)(uVar3 + 4 + param_2) = *(undefined2 *)(puVar6 + 1);
              *(undefined1 *)(uVar3 + 6 + param_2) = *(undefined1 *)((longlong)puVar6 + 6);
            }
            puVar6 = puVar6 + 2;
            uVar4 = uVar4 + 1;
          } while (uVar4 < local_13a >> 3);
        }
      }
    }
    iVar1 = FUN_14000aa04(param_1,"PRT4._ADR",local_158);
    if (iVar1 == 0) {
      uVar8 = local_158[0] >> 0x10;
      iVar1 = FUN_14000a904(param_1,"PRT4._GTF",local_148,local_158 + 1);
      if (((iVar1 == 0) && (local_148[0] == 0x426f6541)) && (local_140 == 1)) {
        puVar6 = &local_138;
        if (local_13a >> 3 != 0) {
          do {
            if ((uVar8 < 8) && (uVar5 < 4)) {
              uVar3 = (ulonglong)((uVar5 + uVar8 * 4) * 8);
              *(undefined4 *)(uVar3 + param_2) = *puVar6;
              *(undefined2 *)(uVar3 + 4 + param_2) = *(undefined2 *)(puVar6 + 1);
              *(undefined1 *)(uVar3 + 6 + param_2) = *(undefined1 *)((longlong)puVar6 + 6);
            }
            puVar6 = puVar6 + 2;
            uVar5 = uVar5 + 1;
          } while (uVar5 < local_13a >> 3);
        }
      }
    }
    iVar1 = FUN_14000aa04(param_1,"PRT5._ADR",local_158);
    if (iVar1 == 0) {
      iVar1 = FUN_14000a904(param_1,"PRT5._GTF",local_148,local_158 + 1);
      if (((iVar1 == 0) && (local_148[0] == 0x426f6541)) && (local_140 == 1)) {
        puVar6 = &local_138;
        if (local_13a >> 3 != 0) {
          do {
            if ((local_158[0] >> 0x10 < 8) && (uVar7 < 4)) {
              uVar3 = (ulonglong)((uVar7 + (local_158[0] >> 0x10) * 4) * 8);
              *(undefined4 *)(uVar3 + param_2) = *puVar6;
              *(undefined2 *)(uVar3 + 4 + param_2) = *(undefined2 *)(puVar6 + 1);
              *(undefined1 *)(uVar3 + 6 + param_2) = *(undefined1 *)((longlong)puVar6 + 6);
            }
            puVar6 = puVar6 + 2;
            uVar7 = uVar7 + 1;
          } while (uVar7 < local_13a >> 3);
        }
      }
    }
  }
  return;
}



// ===== FUN_14000204c @ 14000204c =====

void FUN_14000204c(longlong param_1,uint param_2)

{
  longlong lVar1;
  ulonglong uVar2;
  uint uVar3;
  uint uVar4;
  
  lVar1 = *(longlong *)(param_1 + 0x15780);
  uVar3 = *(uint *)(lVar1 + 4);
  if (uVar3 != 0xffffffff) {
    if (param_2 == 0xffffffff) {
      uVar3 = uVar3 & 0xfffffffd;
      uVar4 = 0;
      *(uint *)(lVar1 + 4) = uVar3;
      if (*(char *)(param_1 + 0x157c4) != '\0') {
        do {
          uVar2 = 1L << ((byte)uVar4 & 0x3f);
          if ((uVar2 & *(ulonglong *)(param_1 + 0x157a8)) == uVar2) {
            *(undefined4 *)(*(longlong *)((ulonglong)uVar4 * 0x728 + param_1) + 0x14) = 0x7d40005b;
          }
          uVar4 = uVar4 + 1;
        } while (uVar4 < *(byte *)(param_1 + 0x157c4));
      }
    }
    else {
      uVar2 = 1L << ((byte)param_2 & 0x3f);
      if ((*(ulonglong *)(param_1 + 0x157a8) & uVar2) == uVar2) {
        *(undefined4 *)(*(longlong *)((ulonglong)param_2 * 0x728 + param_1) + 0x14) = 0x7d40005b;
      }
    }
    *(uint *)(lVar1 + 4) = uVar3 | 2;
  }
  return;
}



// ===== FUN_140006c34 @ 140006c34 =====

void FUN_140006c34(undefined8 param_1,longlong param_2)

{
  char cVar1;
  undefined8 *puVar2;
  longlong lVar3;
  
  lVar3 = (**(code **)(DAT_140014958 + 0x650))(DAT_140014980,param_1,PTR_DAT_140014090);
  cVar1 = *(char *)(*(longlong *)(param_2 + 0xb8) + 8);
  if (*(int *)(*(longlong *)(param_2 + 0xb8) + 0x10) == 3) {
    if (cVar1 == '\x01') {
      *(int *)(lVar3 + 0x1c7c8) = *(int *)(lVar3 + 0x1c7c8) + 1;
      puVar2 = *(undefined8 **)(param_2 + 0xb8);
      puVar2[-9] = *puVar2;
      puVar2[-8] = puVar2[1];
      puVar2[-7] = puVar2[2];
      puVar2[-6] = puVar2[3];
      *(undefined4 *)(puVar2 + -5) = *(undefined4 *)(puVar2 + 4);
      *(undefined4 *)((longlong)puVar2 + -0x24) = *(undefined4 *)((longlong)puVar2 + 0x24);
      *(undefined4 *)(puVar2 + -4) = *(undefined4 *)(puVar2 + 5);
      *(undefined4 *)((longlong)puVar2 + -0x1c) = *(undefined4 *)((longlong)puVar2 + 0x2c);
      puVar2[-3] = puVar2[6];
      *(undefined1 *)((longlong)puVar2 + -0x45) = 0;
      IoSetCompletionRoutineEx(*(undefined8 *)(lVar3 + 0x1c7b8),param_2,FUN_140006d3c,lVar3,1,1,1);
    }
    else {
      if (cVar1 != '\0') goto LAB_140006d0c;
      *(int *)(lVar3 + 0x1c7c8) = *(int *)(lVar3 + 0x1c7c8) + -1;
      *(char *)(param_2 + 0x43) = *(char *)(param_2 + 0x43) + '\x01';
      *(longlong *)(param_2 + 0xb8) = *(longlong *)(param_2 + 0xb8) + 0x48;
    }
    IofCallDriver(*(undefined8 *)(lVar3 + 0x1c7c0),param_2);
  }
  else {
LAB_140006d0c:
    (**(code **)(DAT_140014958 + 0x110))(DAT_140014980,param_1,param_2);
  }
  return;
}



// ===== FUN_140006da0 @ 140006da0 =====

undefined4 FUN_140006da0(undefined8 param_1,longlong param_2)

{
  longlong lVar1;
  undefined4 uVar2;
  longlong lVar3;
  
  lVar1 = *(longlong *)(param_2 + 0xb8);
  uVar2 = 0;
  lVar3 = (**(code **)(DAT_140014958 + 0x650))(DAT_140014980,param_1,PTR_DAT_140014090);
  if ((*(char *)(lVar1 + 1) == '\x02') && (*(char *)(lVar3 + 0x1c838) != '\0')) {
    *(longlong *)(lVar3 + 0x1c840) = param_2;
  }
  else {
    *(char *)(param_2 + 0x43) = *(char *)(param_2 + 0x43) + '\x01';
    *(longlong *)(param_2 + 0xb8) = *(longlong *)(param_2 + 0xb8) + 0x48;
    uVar2 = (**(code **)(DAT_140014958 + 0x110))(DAT_140014980,param_1,param_2);
  }
  return uVar2;
}



