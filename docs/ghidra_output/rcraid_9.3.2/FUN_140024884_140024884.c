// FUN_140024884 @ 140024884

/* WARNING: Function: __security_check_cookie replaced with injection: security_check_cookie */

undefined8 FUN_140024884(int *param_1,undefined4 *param_2)

{
  int *piVar1;
  uint uVar2;
  int iVar3;
  longlong lVar4;
  char *pcVar5;
  undefined8 uVar6;
  longlong lVar7;
  ulonglong uVar8;
  ulonglong uVar9;
  ulonglong uVar10;
  ulonglong uVar11;
  uint uVar12;
  undefined1 auStack_158 [32];
  uint local_138;
  ulonglong local_130;
  int local_128;
  undefined4 local_120;
  uint local_118;
  uint local_114;
  undefined4 local_110;
  int local_10c;
  uint local_108;
  uint local_104;
  int local_100;
  int local_fc;
  longlong local_f8;
  uint local_f0;
  uint local_ec;
  longlong local_e8;
  longlong local_e0;
  longlong local_d8;
  undefined4 *local_d0;
  int local_c8;
  int iStack_c4;
  int iStack_c0;
  int iStack_bc;
  int local_b8;
  int local_a8;
  int iStack_a4;
  int iStack_a0;
  int iStack_9c;
  int local_98;
  undefined1 local_88 [8];
  ulonglong local_80;
  ulonglong local_78;
  ulonglong local_70;
  ulonglong local_60;
  ulonglong local_50;
  ulonglong local_48;
  
  local_48 = DAT_140081770 ^ (ulonglong)auStack_158;
  uVar8 = 0;
  uVar11 = 0;
  local_fc = 0;
  local_110 = 0;
  *(undefined8 *)(param_2 + 8) = 0;
  local_f0 = param_1[0xd];
  uVar2 = param_1[0xc];
  local_ec = param_1[8] & 1;
  local_100 = 0;
  lVar7 = (&DAT_1400f43f0)[*(ushort *)(param_1 + 3) & 0x3f];
  local_118 = param_1[0xb];
  uVar9 = *(ulonglong *)(lVar7 + 0x70) >> 0xb;
  local_e0 = lVar7;
  local_d0 = param_2;
  if ((8 < local_f0) && (local_ec == 0)) {
    uVar6 = 8;
    pcVar5 = "RC_CalculateTransformMaxSize: Error, invalid protect level %d\n";
LAB_14002492a:
    (**(code **)(DAT_1400f6bf8 + 0xac))(pcVar5,uVar6);
    return 7;
  }
  if ((local_118 == 0) || (uVar2 == 0)) {
    (**(code **)(DAT_1400f6bf8 + 0xac))
              ("RC_CalculateTransformMaxSize: Error, first_count = %d, second_count = %d\n",
               local_118,uVar2);
    return 7;
  }
  iVar3 = FUN_140063224(param_1);
  if (iVar3 == 1) {
    (**(code **)(DAT_1400f6bf8 + 0xac))("RC_CalculateTransformMaxSize: Error - Duplicate devices\n")
    ;
    return 0x2c;
  }
  uVar10 = uVar8;
  if (param_1[6] != 0) {
    do {
      local_f8 = uVar10 * 5;
      lVar4 = FUN_1400655f4(*(undefined8 *)(param_1 + uVar10 * 5 + 0x38));
      if ((lVar4 != 0) && (*(int *)(lVar4 + 0x38) == 0x3002)) {
        piVar1 = param_1 + local_f8 + 0x38;
        local_c8 = *piVar1;
        iStack_c4 = piVar1[1];
        iStack_c0 = piVar1[2];
        iStack_bc = piVar1[3];
        local_b8 = param_1[local_f8 + 0x3c];
        iVar3 = FUN_14004501c(lVar7,&local_c8);
        if (iVar3 == 0) {
          (**(code **)(DAT_1400f6bf8 + 0xac))
                    ("RC_CalculateTransformMaxSize: Error - devices have smart error\n");
          return 0x4b;
        }
      }
      uVar12 = (int)uVar10 + 1;
      uVar10 = (ulonglong)uVar12;
    } while (uVar12 < (uint)param_1[6]);
  }
  uVar12 = local_118;
  iVar3 = *param_1;
  if ((iVar3 == 0x1bf5) || (iVar3 == 0x1bfa)) {
    if (0x10 < uVar2) {
      uVar6 = 0x10;
      pcVar5 = "RC_CalculateTransformMaxSize: Error, raid5/6 set greater than %d elements\n";
      goto LAB_14002492a;
    }
  }
  else if (iVar3 == 0x1bf6) {
    if ((local_118 == 1) && (uVar2 == 1)) {
      pcVar5 = "RC_CalculateTransformMaxSize: Error, raid0 set with only 1 element\n";
      goto LAB_140024a10;
    }
    if (0x10 < uVar2) {
      uVar6 = 0x10;
      pcVar5 = "RC_CalculateTransformMaxSize: Error, raid1 set greater than %d elements\n";
      goto LAB_14002492a;
    }
  }
  iVar3 = FUN_140066500(iVar3,uVar2);
  if (iVar3 == 0) {
    pcVar5 = "RC_CalculateTransformMaxSize: unknown base_type\n";
LAB_140024a10:
    (**(code **)(DAT_1400f6bf8 + 0xac))(pcVar5);
    return 7;
  }
  FUN_140064968(uVar12,uVar2,*(undefined4 *)(lVar7 + 0xbc),*(undefined4 *)(lVar7 + 0xec));
  lVar4 = FUN_140061bb4(uVar12,uVar2,*(undefined4 *)(lVar7 + 0xbc),*(undefined4 *)(lVar7 + 0xec));
  FUN_140069168(*param_1,uVar2);
  *(int *)(lVar4 + 4) = *param_1;
  FUN_140017c24(lVar4);
  *(int *)(lVar4 + 0xb0) = param_1[6];
  FUN_1400649f4(lVar7);
  local_114 = FUN_1400649f4(lVar4);
  local_104 = 0;
  if (local_118 != 0) {
    local_f8 = 0;
    uVar10 = uVar8;
    do {
      local_108 = 0;
      uVar12 = (uint)uVar10;
      if (uVar2 != 0) {
        local_e8 = 0;
        local_10c = (uint)uVar10 * uVar2;
        uVar10 = uVar8;
        do {
          local_d8 = *(longlong *)(*(longlong *)(lVar4 + 0x1d8) + local_f8);
          uVar10 = (ulonglong)(uint)(local_10c + (int)uVar10);
          piVar1 = param_1 + uVar10 * 5 + 0x38;
          local_a8 = *piVar1;
          iStack_a4 = piVar1[1];
          iStack_a0 = piVar1[2];
          iStack_9c = piVar1[3];
          local_98 = param_1[uVar10 * 5 + 0x3c];
          iVar3 = FUN_1400683dc(&local_a8);
          if (iVar3 == 0) {
            (**(code **)(DAT_1400f6bf8 + 0xac))
                      ("RC_CreateTransformRaidArray: Error - Invalid or missing device: Handle %d, Device ID = %I64x\n"
                       ,(undefined2)iStack_a0,CONCAT44(iStack_a4,local_a8));
            uVar6 = 0x2a;
            goto LAB_140024c74;
          }
          iVar3 = FUN_140068394(&local_a8);
          if (iVar3 == 0) {
            (**(code **)(DAT_1400f6bf8 + 0xac))
                      ("RC_CreateTransformRaidArray: Error - Device not initialized: Handle %d, Device ID = %I64x\n"
                       ,(undefined2)iStack_a0,CONCAT44(iStack_a4,local_a8));
            uVar6 = 0x37;
            goto LAB_140024c74;
          }
          piVar1 = (int *)(local_e8 + local_d8);
          *piVar1 = local_a8;
          piVar1[1] = iStack_a4;
          piVar1[2] = iStack_a0;
          piVar1[3] = iStack_9c;
          *(int *)(local_e8 + 0x10 + local_d8) = local_98;
          local_e8 = local_e8 + 0x44;
          local_108 = local_108 + 1;
          uVar10 = (ulonglong)local_108;
          uVar12 = local_104;
        } while (local_108 < uVar2);
      }
      local_104 = uVar12 + 1;
      uVar10 = (ulonglong)local_104;
      local_f8 = local_f8 + 8;
    } while (local_104 < local_118);
  }
  local_10c = FUN_140063518(lVar7,lVar4);
  if (local_10c != 0) {
    FUN_1400632c8(lVar7,lVar4);
    local_100 = FUN_140063c08(lVar7);
    if (*(int *)(lVar7 + 4) == 0x1bf7) {
      if (*(int *)(lVar4 + 4) == 0x1bf7) {
        local_110 = FUN_1400657e0(lVar7,lVar4);
      }
    }
    else if ((*(int *)(lVar7 + 4) == 0x1bf6) && (*(int *)(lVar4 + 4) == 0x1bf7)) {
      local_110 = FUN_14006588c(lVar7,lVar4);
    }
  }
  local_120 = *(undefined4 *)(lVar4 + 4);
  local_128 = param_1[6];
  local_130 = CONCAT44(local_130._4_4_,*(undefined4 *)(lVar4 + 0xb8));
  local_138 = *(uint *)(lVar4 + 0xb4);
  FUN_140028dbc(lVar7,lVar4,param_1 + 0x38,local_88);
  if (0x5000 < local_50) {
    local_128 = local_100;
    local_130 = CONCAT44(local_130._4_4_,local_110);
    local_138 = 1;
    FUN_14006998c(local_e0,lVar4,local_50,local_50 - 0x5000);
    lVar7 = local_e0;
    *(ulonglong *)(lVar4 + 0x70) = (ulonglong)local_114 * (local_50 - 0x5000);
    local_fc = FUN_14006364c(local_e0,lVar4,1);
  }
  uVar12 = FUN_140069c0c(lVar7,lVar4);
  if ((local_10c == 0) || (local_fc == 1)) {
    iVar3 = *(int *)(lVar4 + 4);
LAB_140024df9:
    uVar10 = uVar8;
    if ((iVar3 - 0x1bf7U & 0xfffffffb) != 0) {
joined_r0x000140024d97:
      uVar11 = 0;
      uVar10 = uVar9;
      if (0x5000 < local_80) {
        uVar11 = local_80 - 0x5000 >> 0xb;
        uVar10 = local_114 * uVar11;
      }
      goto LAB_140024e21;
    }
  }
  else {
    iVar3 = *(int *)(lVar4 + 4);
    if ((uVar12 & 0xf000) == 0x1000) goto LAB_140024df9;
    if ((iVar3 - 0x1bf7U & 0xfffffffb) != 0) {
      if (local_80 <= local_60) {
        local_80 = local_60;
      }
      goto joined_r0x000140024d97;
    }
    uVar10 = uVar9;
    if (local_78 < local_70) {
      uVar10 = local_70 >> 0xb;
      if (local_70 < 0x5001) {
        uVar10 = uVar9;
      }
      goto LAB_140024e21;
    }
  }
  if (0x5000 < local_78) {
    uVar10 = local_78 >> 0xb;
  }
LAB_140024e21:
  uVar9 = uVar8;
  if (uVar10 != 0) {
    local_138 = local_f0;
    local_130 = uVar11;
    uVar9 = FUN_1400247b4(*param_1,local_118,uVar2,local_ec);
  }
  if ((*param_1 - 0x1bf7U & 0xfffffffb) != 0) {
    uVar10 = local_114 * uVar9;
  }
  uVar6 = 1;
  if (*(ulonglong *)(lVar7 + 0x70) <= uVar10 << 0xb) {
    uVar8 = uVar10;
  }
  *(ulonglong *)(param_1 + 9) = uVar8;
  *(ulonglong *)(local_d0 + 8) = uVar8;
  local_d0[10] = *(undefined4 *)(lVar4 + 0xb4);
  local_d0[0xb] = *(undefined4 *)(lVar4 + 0xb8);
  *local_d0 = *(undefined4 *)(lVar4 + 4);
LAB_140024c74:
  FUN_140065d04(lVar4,1);
  return uVar6;
}

