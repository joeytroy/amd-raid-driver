// FUN_14000fbb0 @ 14000fbb0

void FUN_14000fbb0(longlong param_1,undefined8 param_2,int param_3,int param_4,char param_5,
                  char param_6,longlong param_7,longlong param_8)

{
  uint *puVar1;
  short *psVar2;
  ushort uVar3;
  uint uVar4;
  ulonglong uVar5;
  ulonglong uVar6;
  ushort uVar7;
  longlong lVar8;
  
  lVar8 = 0x100;
  uVar7 = (ushort)**(undefined8 **)(param_1 + 0x68);
  psVar2 = *(short **)(param_1 + 8 + (longlong)param_4 * 8);
  if (param_5 == '\0') {
    FUN_140011d00(psVar2,0,0x78);
    *(longlong *)(psVar2 + 0xc) = param_7;
    *psVar2 = (short)param_4 + 1;
    uVar3 = uVar7;
    if (0x100 < uVar7) {
      uVar3 = 0x100;
    }
    psVar2[5] = uVar3;
    if (0x400 < uVar7) {
      uVar7 = 0x400;
    }
    psVar2[1] = uVar3;
    *(longlong *)(psVar2 + 0x14) = param_8;
    psVar2[6] = uVar7;
    *(longlong *)(psVar2 + 0x18) = param_8 + 0x8000;
    *(longlong *)(psVar2 + 0x1c) = param_8 + 0x10000;
    *(longlong *)(psVar2 + 0x10) = param_7 + 0x8000;
    *(longlong *)(psVar2 + 0x20) = param_7 + 0x10000;
  }
  else {
    param_7 = *(longlong *)(psVar2 + 0xc);
  }
  FUN_140011d00(param_7,0,0x4000);
  FUN_140011d00(*(undefined8 *)(psVar2 + 0x10),0,0x4000);
  FUN_140011d00(*(undefined8 *)(psVar2 + 0x20),0,0x20000);
  uVar6 = 0;
  uVar5 = uVar6;
  if (psVar2[6] != 0) {
    do {
      uVar4 = (int)uVar5 + 1;
      puVar1 = (uint *)(*(longlong *)(psVar2 + 0x10) + 0xc + uVar5 * 0x10);
      *puVar1 = *puVar1 | 0xffff;
      uVar5 = (ulonglong)uVar4;
    } while (uVar4 < (ushort)psVar2[6]);
  }
  if (param_5 == '\0') {
    *(undefined **)(psVar2 + 0x24) =
         &DAT_14021fa20 + (longlong)(param_4 + (param_3 + -1) * 4) * 0xf000;
  }
  psVar2[8] = 0;
  psVar2[9] = 1;
  psVar2[2] = -1;
  psVar2[3] = 0;
  psVar2[4] = 0;
  psVar2[7] = 0;
  *(undefined4 *)(param_1 + 0x28) = 0;
  *(undefined4 *)(param_1 + 0x30) = 0;
  do {
    FUN_140011d00(*(longlong *)(psVar2 + 0x24) + uVar6,0,0x78);
    uVar6 = uVar6 + 0x78;
    lVar8 = lVar8 + -1;
  } while (lVar8 != 0);
  if ((param_5 == '\0') && (param_6 == '\0')) {
    *(undefined1 *)(param_1 + 0x3dc) = 1;
    FUN_14000fd88(psVar2,param_2,psVar2[5]);
  }
  return;
}

