// FUN_14000db84 @ 14000db84

void FUN_14000db84(longlong param_1,ushort *param_2)

{
  longlong lVar1;
  ulonglong uVar2;
  longlong lVar3;
  longlong lVar4;
  uint uVar5;
  ushort uVar6;
  
  lVar1 = *(longlong *)(param_1 + 0x68);
  uVar6 = param_2[8];
  uVar2 = (ulonglong)uVar6;
  lVar4 = (ulonglong)uVar6 * 0x10 + *(longlong *)(param_2 + 0x10);
  uVar5 = *(uint *)(lVar4 + 0xc);
  if (((ushort)(uVar5 >> 0x10) & 1) == param_2[9]) {
    do {
      uVar6 = (ushort)uVar2;
      lVar3 = (ulonglong)(uVar5 & 0xffff) * 0x78 + *(longlong *)(param_2 + 0x24);
      if ((*(ushort *)(lVar4 + 10) == *param_2) && (*(short *)(lVar3 + 0x20) == (short)uVar5)) {
        param_2[4] = *(ushort *)(lVar4 + 8);
        if (*(code **)(lVar3 + 0x28) != (code *)0x0) {
          (**(code **)(lVar3 + 0x28))(param_1,param_2,lVar3,lVar4);
          *(undefined8 *)(lVar3 + 0x28) = 0;
          *(uint *)(lVar4 + 0xc) = *(uint *)(lVar4 + 0xc) | 0xffff;
        }
        param_2[8] = param_2[8] + 1;
        uVar6 = param_2[8];
        if (uVar6 == param_2[6]) {
          param_2[9] = param_2[9] ^ 1;
          uVar6 = 0;
          param_2[8] = 0;
        }
      }
      uVar2 = (ulonglong)uVar6;
      lVar4 = uVar2 * 0x10 + *(longlong *)(param_2 + 0x10);
      uVar5 = *(uint *)(lVar4 + 0xc);
    } while (((ushort)(uVar5 >> 0x10) & 1) == param_2[9]);
  }
  *(uint *)(lVar1 + 0x1004 + (ulonglong)*param_2 * 8) = (uint)uVar6;
  return;
}

