// FUN_14000c390 @ 14000c390

undefined1 FUN_14000c390(longlong param_1,undefined8 param_2,longlong param_3,undefined8 param_4)

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
  lVar8 = FUN_14000c5f0(*puVar10,FUN_14000c334);
  if (lVar8 != 0) {
    uVar11 = 1;
    if (param_3 == 0) {
      *(uint *)(param_1 + 0x15cdc) = *(uint *)(param_1 + 0x15cdc) & 0xffffffef;
      FUN_14000d2e0(puVar10,FUN_14000d1d4);
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

