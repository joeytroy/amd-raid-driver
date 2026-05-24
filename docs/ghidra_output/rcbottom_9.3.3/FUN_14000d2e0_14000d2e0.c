// FUN_14000d2e0 @ 14000d2e0

void FUN_14000d2e0(undefined8 *param_1,undefined8 param_2)

{
  undefined8 *puVar1;
  undefined4 *puVar2;
  ushort *puVar3;
  longlong lVar4;
  longlong lVar5;
  undefined4 uVar6;
  undefined4 uVar7;
  undefined4 uVar8;
  undefined8 uVar9;
  longlong lVar10;
  longlong lVar11;
  int iVar12;
  
  iVar12 = 0;
  if (0 < *(int *)(param_1 + 6)) {
    do {
      lVar10 = FUN_14000c5f0(*param_1,param_2);
      if (lVar10 != 0) {
        *(undefined1 *)(lVar10 + 0x30) = 0;
        *(int *)(lVar10 + 0x58) = iVar12 + 1;
        *(undefined2 *)(lVar10 + 0x32) = *(undefined2 *)(lVar10 + 0x20);
        puVar3 = (ushort *)*param_1;
        uVar9 = *(undefined8 *)(lVar10 + 0x38);
        lVar4 = param_1[0xd];
        lVar11 = (ulonglong)*(ushort *)(lVar10 + 0x22) * 0x40;
        lVar5 = *(longlong *)(puVar3 + 0xc);
        *(undefined8 *)(lVar11 + lVar5) = *(undefined8 *)(lVar10 + 0x30);
        ((undefined8 *)(lVar11 + lVar5))[1] = uVar9;
        uVar9 = *(undefined8 *)(lVar10 + 0x48);
        puVar1 = (undefined8 *)(lVar11 + 0x10 + lVar5);
        *puVar1 = *(undefined8 *)(lVar10 + 0x40);
        puVar1[1] = uVar9;
        uVar6 = *(undefined4 *)(lVar10 + 0x54);
        uVar7 = *(undefined4 *)(lVar10 + 0x58);
        uVar8 = *(undefined4 *)(lVar10 + 0x5c);
        puVar2 = (undefined4 *)(lVar11 + 0x20 + lVar5);
        *puVar2 = *(undefined4 *)(lVar10 + 0x50);
        puVar2[1] = uVar6;
        puVar2[2] = uVar7;
        puVar2[3] = uVar8;
        uVar9 = *(undefined8 *)(lVar10 + 0x68);
        puVar1 = (undefined8 *)(lVar11 + 0x30 + lVar5);
        *puVar1 = *(undefined8 *)(lVar10 + 0x60);
        puVar1[1] = uVar9;
        *(uint *)(lVar4 + 0x1000 + (ulonglong)*puVar3 * 8) =
             (*(ushort *)(lVar10 + 0x22) + 1) % (uint)puVar3[5];
      }
      iVar12 = iVar12 + 1;
    } while (iVar12 < *(int *)(param_1 + 6));
  }
  return;
}

