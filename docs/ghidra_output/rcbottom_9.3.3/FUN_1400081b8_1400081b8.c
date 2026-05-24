// FUN_1400081b8 @ 1400081b8

/* WARNING: Function: __security_check_cookie replaced with injection: security_check_cookie */

void FUN_1400081b8(wchar_t *param_1,longlong param_2,char param_3)

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
  
  local_48 = DAT_140015180 ^ (ulonglong)auStack_128;
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
  *(undefined2 *)(param_2 + 0x16060) = local_e0[0];
  *(undefined2 *)(param_2 + 0x1605e) = local_dc[0];
  *(undefined2 *)(param_2 + 0x16064) = local_d8[0];
  *(undefined2 *)(param_2 + 0x16062) = local_d4[0];
  iVar2 = wcsncmp(param_1,L"PCI\\VEN_1022&DEV_7905",0x15);
  iVar7 = 2;
  if ((iVar2 == 0) || (iVar2 = wcsncmp(param_1,L"PCI\\VEN_1022&DEV_7901",0x15), iVar2 == 0)) {
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
        *(undefined1 *)(param_2 + 0x16074) = 0;
        if (param_3 == '\0') {
          iVar7 = 99;
          *(undefined4 *)(param_2 + 0x16070) = 99;
        }
        else {
          *(undefined4 *)(param_2 + 0x16070) = 2;
        }
        goto LAB_1400083ce;
      }
    }
  }
  *(undefined4 *)(param_2 + 0x16070) = 1;
  *(undefined1 *)(param_2 + 0x16074) = 1;
  iVar7 = 1;
LAB_1400083ce:
  if (iVar7 == 1) {
    *(code **)(param_2 + 0x16108) = FUN_1400040cc;
    *(code **)(param_2 + 0x16110) = FUN_140001438;
    *(code **)(param_2 + 0x16118) = FUN_140002204;
    *(code **)(param_2 + 0x16120) = FUN_140003fac;
    *(code **)(param_2 + 0x16160) = FUN_14000306c;
    *(code **)(param_2 + 0x16128) = FUN_140003078;
    *(code **)(param_2 + 0x16130) = FUN_1400027d8;
    *(code **)(param_2 + 0x16138) = FUN_140002928;
    *(code **)(param_2 + 0x16140) = FUN_140001bd4;
    *(code **)(param_2 + 0x16148) = FUN_140001bec;
    *(code **)(param_2 + 0x16150) = FUN_140003868;
    *(undefined1 **)(param_2 + 0x16158) = &LAB_140001778;
    *(code **)(param_2 + 0x16168) = FUN_1400035c0;
    *(undefined1 **)(param_2 + 0x16170) = &LAB_140002838;
    if ((sVar5 != 0) && (sVar3 != 0)) {
      local_f8 = 0;
      local_100 = 1;
      local_108 = CONCAT22(local_108._2_2_,0x40);
      local_e4[0] = 0;
      local_e8[0] = 0;
      (**(code **)(DAT_140015968 + 0x418))
                (DAT_140015990,*(undefined8 *)(param_2 + 0x20),&DAT_1400132a0,param_2 + 0x1c2a0);
      local_108 = 2;
      (**(code **)(param_2 + 0x1c2d8))(*(undefined8 *)(param_2 + 0x1c2a8),0,local_e4,sVar5);
      local_108 = 2;
      (**(code **)(param_2 + 0x1c2d8))(*(undefined8 *)(param_2 + 0x1c2a8),0,local_e8,sVar3);
      iVar6 = (uint)local_e8[0] * 0x10000 + (uint)local_e4[0];
    }
    *(int *)(param_2 + 0x1c2e0) = iVar6;
  }
  else if (iVar7 == 2) {
    *(code **)(param_2 + 0x16108) = FUN_140010410;
    local_f8 = 0;
    *(code **)(param_2 + 0x16110) = FUN_14000c390;
    local_100 = 1;
    *(code **)(param_2 + 0x16118) = FUN_14000e390;
    *(code **)(param_2 + 0x16120) = FUN_14000ffc4;
    *(code **)(param_2 + 0x16160) = FUN_14000306c;
    *(code **)(param_2 + 0x16128) = FUN_14000ed94;
    *(undefined1 **)(param_2 + 0x16130) = &LAB_14000953c;
    *(code **)(param_2 + 0x16138) = FUN_14000eb18;
    *(code **)(param_2 + 0x16140) = FUN_14000cb04;
    *(code **)(param_2 + 0x16148) = FUN_14000cb1c;
    *(code **)(param_2 + 0x16150) = FUN_14000d4f0;
    *(undefined1 **)(param_2 + 0x16158) = &LAB_140002838;
    *(code **)(param_2 + 0x16168) = FUN_140010b44;
    *(code **)(param_2 + 0x16170) = FUN_1400109dc;
    local_108 = CONCAT22(local_108._2_2_,0x40);
    (**(code **)(DAT_140015968 + 0x418))
              (DAT_140015990,*(undefined8 *)(param_2 + 0x20),&DAT_1400132a0,param_2 + 0x1c2a0);
    local_108 = 1;
    (**(code **)(param_2 + 0x1c2d8))(*(undefined8 *)(param_2 + 0x1c2a8),0,local_e8,0x34);
    uVar1 = local_e8[0] & 0xff;
    uVar4 = 1;
    uVar8 = 0;
    do {
      uVar8 = uVar8 + 1;
      local_108 = 2;
      (**(code **)(param_2 + 0x1c2d8))(*(undefined8 *)(param_2 + 0x1c2a8),0,local_e4,uVar1);
      if ((char)local_e4[0] == '\x10') {
        local_108 = 2;
        (**(code **)(param_2 + 0x1c2d8))
                  (*(undefined8 *)(param_2 + 0x1c2a8),0,local_d0,(byte)local_e8[0] + 0x12);
        *(uint *)(param_2 + 0x1c7b4) = local_d0[0] & 0xf;
        *(uint *)(param_2 + 0x1c7b0) = local_d0[0] >> 4 & 0x3f;
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
    *(code **)(param_2 + 0x16108) = FUN_140010be0;
    *(code **)(param_2 + 0x16110) = FUN_140010be0;
    *(undefined1 **)(param_2 + 0x16118) = &LAB_140010c20;
    *(code **)(param_2 + 0x16120) = FUN_14000306c;
    *(code **)(param_2 + 0x16160) = FUN_14000306c;
    *(undefined1 **)(param_2 + 0x16128) = &LAB_140010c30;
    *(undefined1 **)(param_2 + 0x16130) = &LAB_14000953c;
    *(code **)(param_2 + 0x16138) = FUN_14000306c;
    *(code **)(param_2 + 0x16140) = FUN_140010bfc;
    *(undefined1 **)(param_2 + 0x16148) = &LAB_140010c0c;
    *(code **)(param_2 + 0x16150) = FUN_140010be0;
    *(undefined1 **)(param_2 + 0x16158) = &LAB_140002838;
    *(code **)(param_2 + 0x16168) = FUN_14000306c;
    *(undefined1 **)(param_2 + 0x16170) = &LAB_140002838;
  }
  return;
}

