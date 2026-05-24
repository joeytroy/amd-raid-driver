// FUN_140013234 @ 140013234

/* WARNING: Function: __security_check_cookie replaced with injection: security_check_cookie */
/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

undefined8 FUN_140013234(longlong param_1,longlong param_2)

{
  int iVar1;
  int iVar2;
  uint uVar3;
  longlong lVar4;
  longlong lVar5;
  ulonglong uVar6;
  int *piVar7;
  longlong lVar8;
  int *piVar9;
  ulonglong uVar10;
  ulonglong uVar11;
  ulonglong uVar12;
  undefined1 auStack_d8 [32];
  undefined4 local_b8;
  int local_a8;
  int local_a4;
  int local_a0;
  longlong local_98;
  uint local_90;
  ulonglong local_88;
  ulonglong local_80;
  int *local_78;
  int local_68 [3];
  undefined8 local_5c;
  int local_54;
  ulonglong local_48;
  
  local_48 = DAT_140081770 ^ (ulonglong)auStack_d8;
  uVar3 = 0;
  local_a4 = 0;
  local_a8 = 0;
  local_98 = param_2;
  FUN_1400759c0(&DAT_1400eeee0,0,0x2000);
  DAT_1400eeec8 = 0;
  DAT_1400eeed8 = 0;
  DAT_1400eee98 = 0;
  DAT_1400eee90 = 0;
  DAT_1400eb3f8 = 0;
  DAT_1400eeecc = 0;
  DAT_1400eb3fc = 0;
  DAT_1400eee08 = 0;
  DAT_1400eee00 = 0;
  DAT_1400eb3f0 = 0;
  DAT_1400eee14 = 0;
  DAT_1400eeedc = 0;
  (**(code **)(DAT_1400f6bf8 + 0x84))(&DAT_1400eee18,DAT_1400f4574,0x28);
  (**(code **)(DAT_1400f6bf8 + 0x84))(&DAT_1400eee40,DAT_1400f4574,0x28);
  (**(code **)(DAT_1400f6bf8 + 0x84))(&DAT_1400eee68,DAT_1400f4574,0x28);
  local_b8 = 0x1c;
  (**(code **)(DAT_1400f6bf8 + 0x7c))
            (local_68,*(undefined8 *)(param_1 + 8),*(undefined2 *)(param_2 + 0x10),
             *(undefined2 *)(param_1 + 0x10));
  do {
    DAT_1400f2e04 = DAT_1400f2e04 | 1 << (uVar3 & 0x1f);
    uVar3 = uVar3 + 1;
  } while (uVar3 < 0x20);
  DAT_1400f4578 = *(undefined2 *)(param_1 + 0x10);
  (**(code **)(DAT_1400f6bf8 + 0x84))
            (*(undefined8 *)(param_2 + 8),DAT_1400f4574,*(undefined4 *)(param_2 + 4));
  DAT_1400f2e08 = *(longlong *)(param_2 + 8);
  lVar4 = *(longlong *)(param_1 + 8);
  uVar10 = lVar4 + 0x1c;
  uVar3 = *(uint *)(param_2 + 4) / 0x170 - 2;
  uVar12 = (ulonglong)uVar3;
  uVar6 = (ulonglong)uVar3;
  DAT_1400eeed0 = DAT_1400f2e08 + uVar12 * 8;
  lVar5 = DAT_1400eeed0 + uVar12 * 8;
  _DAT_1400eeea0 = uVar6 * 0xd0 + uVar10;
  _DAT_1400eeea8 = (ulonglong)(uVar3 * 0x4000) + _DAT_1400eeea0;
  local_90 = uVar3;
  if (local_68[0] == 0x9012000) {
    if (local_54 == *(int *)(param_1 + 4)) {
      DAT_140081458 = 0;
      local_a0 = *(int *)(param_1 + 8) - (int)local_5c;
      uVar11 = uVar10;
      _DAT_1400eeeb0 = lVar5;
      _DAT_1400eeeb8 = uVar10;
      if (uVar3 != 0) {
        do {
          local_80 = uVar6;
          uVar10 = uVar11 + 0xd0;
          local_b8 = 0xd0;
          local_88 = uVar11;
          (**(code **)(DAT_1400f6bf8 + 0x7c))
                    (lVar5 + 0x90,uVar11,*(undefined2 *)(param_2 + 0x10),
                     *(undefined2 *)(param_1 + 0x10));
          local_78 = (int *)(lVar5 + 0xa0);
          piVar7 = local_78;
          piVar9 = (int *)(lVar5 + 0x50);
          for (lVar4 = 0x10; lVar4 != 0; lVar4 = lVar4 + -1) {
            *piVar9 = *piVar7;
            piVar7 = piVar7 + 1;
            piVar9 = piVar9 + 1;
          }
          iVar1 = FUN_140015c54(lVar5);
          if (iVar1 == 0) {
LAB_1400136a6:
            (**(code **)(DAT_1400f6bf8 + 0x84))(local_88,*(undefined2 *)(param_1 + 0x10),0xd0);
            param_2 = local_98;
            (**(code **)(DAT_1400f6bf8 + 0x84))(lVar5,*(undefined2 *)(local_98 + 0x10),0x160);
            *(longlong *)(DAT_1400f2e08 + (ulonglong)DAT_1400eeed8 * 8) = lVar5;
            DAT_1400eeed8 = DAT_1400eeed8 + 1;
          }
          else {
            iVar1 = 0;
            lVar4 = 0xe0;
            lVar8 = 0x10;
            piVar7 = local_78;
            do {
              if (*piVar7 == 0) {
                *(undefined8 *)(lVar4 + lVar5) = 0;
              }
              else {
                *(longlong *)(lVar4 + lVar5) = (longlong)local_a0 + *(longlong *)(lVar4 + lVar5);
                iVar1 = iVar1 + 1;
              }
              piVar7 = piVar7 + 1;
              lVar4 = lVar4 + 8;
              lVar8 = lVar8 + -1;
            } while (lVar8 != 0);
            local_a4 = local_a4 + iVar1;
            uVar12 = (ulonglong)local_90;
            if (iVar1 == 0) goto LAB_1400136a6;
            FUN_1400138d0(lVar5);
            FUN_140013af0(lVar5);
            if (DAT_1400f2df4 != 0) {
              (**(code **)(DAT_1400f6bf8 + 0xac))("RESTORED FROM NVRAM\n");
              FUN_14000c9ec(lVar5);
            }
            local_a8 = local_a8 + 1;
            param_2 = local_98;
          }
          lVar5 = lVar5 + 0x160;
          uVar11 = uVar10;
          uVar6 = local_80 - 1;
        } while (local_80 - 1 != 0);
        local_80 = 0;
      }
      iVar1 = local_a0;
      uVar10 = uVar10 + 0x3fff & 0xffffffffffffc000;
      uVar6 = uVar12;
      if ((int)uVar12 != 0) {
        do {
          iVar2 = FUN_140013d60(uVar10);
          if (iVar2 == 0) {
            *(ulonglong *)(DAT_1400eeed0 + (ulonglong)DAT_1400eeec8 * 8) = uVar10;
            DAT_1400eeec8 = DAT_1400eeec8 + 1;
          }
          uVar10 = uVar10 + 0x4000;
          uVar6 = uVar6 - 1;
        } while (uVar6 != 0);
      }
      lVar4 = DAT_1400eee18;
      if (iVar1 != 0) {
        for (; lVar4 != 0; lVar4 = *(longlong *)(lVar4 + 0x18)) {
          FUN_14001584c(lVar4);
        }
      }
      goto LAB_1400134c6;
    }
    (**(code **)(DAT_1400f6bf8 + 0xac))("NVRAM Valid Cookie but invalid size, CLEARING!!!\n");
    local_68[0] = 0;
  }
  (**(code **)(DAT_1400f6bf8 + 0xac))("RC_InitDataAndCacheMemory() NO VALID COOKIE\n");
  uVar10 = lVar4 + 0x23U & 0xfffffffffffffff8;
  _DAT_1400eeeb8 = uVar10;
  (**(code **)(DAT_1400f6bf8 + 0x84))(uVar10,*(undefined2 *)(param_1 + 0x10),uVar3 * 0xd0);
  _DAT_1400eeea0 = uVar10 + uVar6 * 0xd0;
  uVar10 = _DAT_1400eeea0 + 0x3fffU & 0xffffffffffffc000;
  _DAT_1400eeeb0 = lVar5;
  if (uVar3 != 0) {
    do {
      FUN_1400126bc(lVar5);
      lVar5 = lVar5 + 0x160;
      *(ulonglong *)(DAT_1400eeed0 + (ulonglong)DAT_1400eeec8 * 8) = uVar10;
      uVar10 = uVar10 + 0x4000;
      DAT_1400eeec8 = DAT_1400eeec8 + 1;
      uVar6 = uVar6 - 1;
    } while (uVar6 != 0);
  }
LAB_1400134c6:
  iVar1 = local_a4;
  local_5c = *(undefined8 *)(param_1 + 8);
  local_54 = *(undefined4 *)(param_1 + 4);
  local_68[0] = 0x9012000;
  (**(code **)(DAT_1400f6bf8 + 0xac))
            (">>>> NV Recovery Complete Restored %d dirty buffers from %d dirty tags <<<<\n",
             local_a4,local_a8);
  _DAT_1400f84e0 = iVar1;
  uVar3 = (uint)uVar12;
  _DAT_1400eeea8 = uVar10;
  DAT_1400eeec4 = uVar3;
  FUN_140014424();
  FUN_140013808();
  FUN_1400335a4();
  _DAT_1400f81b8 = FUN_140057bf0(0,0xae0);
  DAT_1400f8188 = FUN_140057bf0(0,0xae2);
  *(code **)(DAT_1400f8188 + 0x28) = FUN_140032250;
  FUN_140032250(DAT_1400f8188);
  DAT_1400f81c8 = FUN_140057bf0(0,0xae6);
  *(code **)(DAT_1400f81c8 + 0x28) = FUN_140032b84;
  FUN_140032b84(DAT_1400f81c8);
  DAT_1400f8190 = FUN_140057bf0(0,0xaeb);
  DAT_1400f324c = uVar3 * 3 >> 2;
  DAT_1400f3258 = (undefined4)(uVar12 >> 4);
  DAT_1400f3250 = DAT_1400f324c - (uVar3 >> 3);
  local_b8 = 0x1c;
  DAT_1400f3254 = DAT_1400f324c;
  (**(code **)(DAT_1400f6bf8 + 0x7c))
            (*(undefined8 *)(param_1 + 8),local_68,*(undefined2 *)(param_1 + 0x10),
             *(undefined2 *)(local_98 + 0x10));
  return 0;
}

