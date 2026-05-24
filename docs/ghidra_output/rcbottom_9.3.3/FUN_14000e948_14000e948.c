// FUN_14000e948 @ 14000e948

undefined1 FUN_14000e948(longlong param_1,longlong param_2,undefined8 param_3,undefined8 param_4)

{
  undefined1 uVar1;
  ushort uVar2;
  longlong lVar3;
  longlong lVar4;
  char cVar5;
  undefined4 uVar6;
  longlong lVar7;
  undefined8 uVar8;
  longlong lVar9;
  ushort *puVar10;
  undefined8 *puVar11;
  
  puVar11 = (undefined8 *)(param_1 + 0x15940);
  if (*(char *)(param_2 + 0x60) == '\x01') {
    uVar8 = *puVar11;
  }
  else {
    if (*(char *)(param_2 + 0x60) != '\x02') {
      return 0;
    }
    uVar8 = *(undefined8 *)(param_1 + 0x15948 + (ulonglong)*(uint *)(param_1 + 0x15968) * 8);
  }
  lVar7 = FUN_14000c5f0(uVar8,FUN_140010aa0);
  if (lVar7 == 0) {
    return 0;
  }
  uVar1 = *(undefined1 *)(param_2 + 0x61);
  *(undefined8 *)(lVar7 + 0x40) = 0;
  *(undefined1 *)(lVar7 + 0x30) = uVar1;
  *(undefined1 *)(lVar7 + 0x31) = *(undefined1 *)(param_2 + 0x62);
  uVar6 = *(undefined4 *)(param_2 + 0x68);
  *(undefined8 *)(lVar7 + 0x48) = 0;
  *(undefined8 *)(lVar7 + 0x50) = 0;
  *(undefined4 *)(lVar7 + 0x34) = uVar6;
  if ((*(uint *)(param_2 + 100) & 4) == 0) {
    uVar6 = 0;
  }
  else {
    uVar6 = *(undefined4 *)(param_2 + 0x6c);
  }
  *(undefined4 *)(lVar7 + 0x58) = uVar6;
  if ((*(uint *)(param_2 + 100) & 8) == 0) {
    uVar6 = 0;
  }
  else {
    uVar6 = *(undefined4 *)(param_2 + 0x70);
  }
  *(undefined4 *)(lVar7 + 0x5c) = uVar6;
  if ((*(uint *)(param_2 + 100) & 0x10) == 0) {
    uVar6 = 0;
  }
  else {
    uVar6 = *(undefined4 *)(param_2 + 0x74);
  }
  *(undefined4 *)(lVar7 + 0x60) = uVar6;
  if ((*(uint *)(param_2 + 100) & 0x20) == 0) {
    uVar6 = 0;
  }
  else {
    uVar6 = *(undefined4 *)(param_2 + 0x78);
  }
  *(undefined4 *)(lVar7 + 100) = uVar6;
  if ((*(uint *)(param_2 + 100) & 0x40) == 0) {
    uVar6 = 0;
  }
  else {
    uVar6 = *(undefined4 *)(param_2 + 0x7c);
  }
  *(undefined4 *)(lVar7 + 0x68) = uVar6;
  if ((char)*(undefined4 *)(param_2 + 100) < '\0') {
    uVar6 = *(undefined4 *)(param_2 + 0x80);
  }
  else {
    uVar6 = 0;
  }
  *(undefined4 *)(lVar7 + 0x6c) = uVar6;
  *(undefined2 *)(lVar7 + 0x32) = *(undefined2 *)(lVar7 + 0x20);
  cVar5 = FUN_14000ccd4(puVar11,param_2,param_3,0,lVar7);
  if (cVar5 != '\0') {
    *(int *)(lVar7 + 0x1c) = (int)((ulonglong)param_2 >> 0x20);
    uVar2 = *(ushort *)(lVar7 + 0x20);
    *(int *)(lVar7 + 0x18) = (int)param_2;
    *(undefined8 *)(lVar7 + 0x70) = param_4;
    *(undefined4 *)(param_2 + 0xc4) = 0;
    *(uint *)(param_2 + 0xc0) = (uint)uVar2;
    if (*(char *)(param_2 + 0x60) == '\x01') {
      puVar10 = (ushort *)*puVar11;
    }
    else {
      if (*(char *)(param_2 + 0x60) != '\x02') {
        FUN_14000cbf0(param_2,param_4,2,0x2600);
        return 1;
      }
      puVar10 = *(ushort **)(param_1 + 0x15948 + (ulonglong)*(uint *)(param_1 + 0x15968) * 8);
    }
    lVar3 = *(longlong *)(puVar10 + 0xc);
    uVar8 = *(undefined8 *)(lVar7 + 0x38);
    lVar4 = *(longlong *)(param_1 + 0x159a8);
    lVar9 = (ulonglong)*(ushort *)(lVar7 + 0x22) * 0x40;
    *(undefined8 *)(lVar9 + lVar3) = *(undefined8 *)(lVar7 + 0x30);
    ((undefined8 *)(lVar9 + lVar3))[1] = uVar8;
    uVar8 = *(undefined8 *)(lVar7 + 0x48);
    puVar11 = (undefined8 *)(lVar9 + 0x10 + lVar3);
    *puVar11 = *(undefined8 *)(lVar7 + 0x40);
    puVar11[1] = uVar8;
    uVar8 = *(undefined8 *)(lVar7 + 0x58);
    puVar11 = (undefined8 *)(lVar9 + 0x20 + lVar3);
    *puVar11 = *(undefined8 *)(lVar7 + 0x50);
    puVar11[1] = uVar8;
    uVar8 = *(undefined8 *)(lVar7 + 0x68);
    puVar11 = (undefined8 *)(lVar9 + 0x30 + lVar3);
    *puVar11 = *(undefined8 *)(lVar7 + 0x60);
    puVar11[1] = uVar8;
    *(uint *)(lVar4 + 0x1000 + (ulonglong)*puVar10 * 8) =
         (*(ushort *)(lVar7 + 0x22) + 1) % (uint)puVar10[5];
    return 1;
  }
  return 1;
}

