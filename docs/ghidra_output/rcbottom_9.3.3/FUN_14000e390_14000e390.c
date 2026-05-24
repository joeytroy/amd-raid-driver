// FUN_14000e390 @ 14000e390

void FUN_14000e390(longlong param_1,char param_2)

{
  uint *puVar1;
  byte bVar2;
  ulonglong *puVar3;
  undefined2 *puVar4;
  undefined8 uVar5;
  ushort uVar6;
  ulonglong *puVar7;
  ulonglong uVar8;
  ulonglong uVar9;
  int *piVar10;
  ushort uVar11;
  int iVar12;
  uint uVar13;
  undefined4 uVar14;
  ulonglong uVar15;
  longlong *plVar16;
  longlong lVar17;
  ulonglong uVar18;
  uint uVar19;
  char cVar20;
  int local_64;
  undefined8 local_58;
  undefined8 local_50;
  ulonglong *local_48;
  
  bVar2 = *(byte *)(param_1 + 0x15d08);
  uVar15 = 0;
  local_50 = *(undefined8 *)(param_1 + 0x16010);
  local_64 = 0xffff;
  uVar19 = 1;
  *(undefined4 *)(param_1 + 0x16078) = 1;
  *(char *)(param_1 + 0x1c7b8) = param_2;
  uVar13 = (uint)bVar2;
  if (bVar2 == 0) {
    local_64 = FUN_14000d7e0();
    (**(code **)(DAT_140015968 + 0xdb0))
              (DAT_140015990,*(undefined8 *)(param_1 + 0x20),0,0,0xc02,
               "V:\\RC-933\\RC_933_00291\\fulcrum\\rc\\platforms\\rcbottom\\rcbottom\\nvme.c");
    if (0x10 < local_64) {
      uVar13 = uVar19;
    }
  }
  *(undefined2 *)(param_1 + 0x15d08) = 0;
  *(longlong *)(param_1 + 0x15fa0) = param_1;
  *(undefined2 *)(param_1 + 0x15ce8) = 0;
  *(undefined4 *)(param_1 + 0x15cdc) = 0;
  uVar14 = 0;
  *(undefined4 *)(param_1 + 0x15974) = 0;
  puVar3 = *(ulonglong **)(param_1 + 0x10);
  *(ulonglong **)(param_1 + 0x15978) = puVar3;
  *(ulonglong **)(param_1 + 0x159a8) = puVar3;
  if (*(char *)(param_1 + 0xb4) == '\x01') {
    uVar14 = *(undefined4 *)(param_1 + 0xb0);
  }
  *(undefined4 *)(param_1 + 0x15d18) = uVar14;
  if ((*(int *)(param_1 + 0xb0) == 1) || (param_2 != '\0')) {
    *(undefined4 *)(param_1 + 0x15d24) = 1;
    *(undefined4 *)(param_1 + 0x15d18) = 1;
  }
  else {
    uVar19 = *(int *)(param_1 + 0xb0) - 1;
    *(uint *)(param_1 + 0x15d24) = uVar19;
    if (4 < (int)uVar19) {
      *(undefined4 *)(param_1 + 0x15d24) = 4;
      uVar19 = 4;
    }
  }
  *(longlong *)(param_1 + 0x15940) = param_1 + 0x15d28;
  if ((int)uVar19 < 1) {
    iVar12 = *(int *)(param_1 + 0x15d24);
  }
  else {
    plVar16 = (longlong *)(param_1 + 0x15948);
    uVar8 = uVar15;
    do {
      iVar12 = (int)uVar8;
      uVar19 = iVar12 + 1;
      uVar8 = (ulonglong)uVar19;
      *plVar16 = param_1 + 0x15da0 + (longlong)iVar12 * 0x78;
      plVar16 = plVar16 + 1;
      iVar12 = *(int *)(param_1 + 0x15d24);
    } while ((int)uVar19 < iVar12);
  }
  cVar20 = (char)uVar13;
  local_48 = puVar3;
  if (cVar20 == '\0') {
    (**(code **)(DAT_140015968 + 0xa8))
              (DAT_140015990,local_50,iVar12 * 0x30000 + 0x9fff,0,&local_58);
    puVar7 = (ulonglong *)(**(code **)(DAT_140015968 + 0xb8))(DAT_140015990,local_58);
    uVar8 = (**(code **)(DAT_140015968 + 0xb0))(DAT_140015990,local_58);
    *(ulonglong *)(param_1 + 0x1c320) = uVar8;
    *(int *)(param_1 + 0x1c328) = iVar12 * 0x30000 + 0x9000;
    *(ulonglong **)(param_1 + 0x159b0) = puVar7;
    *(ulonglong *)(param_1 + 0x159b8) = uVar8;
    uVar18 = uVar8;
  }
  else {
    uVar8 = *(ulonglong *)(param_1 + 0x159b8);
    puVar7 = puVar3;
    uVar18 = uVar15;
  }
  FUN_140011d00(uVar8,0,0x1000);
  puVar4 = *(undefined2 **)(param_1 + 0x15940);
  uVar11 = (short)*puVar3 + 1;
  if ((short)*puVar3 == -1) {
    uVar11 = 0xffff;
  }
  *puVar4 = 0;
  if (cVar20 == '\0') {
    FUN_140011d00(puVar4,0,0x78);
    uVar6 = uVar11;
    if (0x100 < uVar11) {
      uVar6 = 0x100;
    }
    puVar4[5] = uVar6;
    puVar4[1] = uVar6;
    if (0x400 < uVar11) {
      uVar11 = 0x400;
    }
    uVar9 = (longlong)puVar7 + 0x13ffU & 0xfffffffffffffc00;
    puVar4[6] = uVar11;
    uVar8 = uVar18 + 0x13ff & 0xfffffffffffffc00;
    *(ulonglong *)(puVar4 + 0x14) = uVar9;
    *(ulonglong *)(puVar4 + 0xc) = uVar8;
    *(ulonglong *)(puVar4 + 0x18) = uVar9 + 0x4000;
    *(ulonglong *)(puVar4 + 0x10) = uVar8 + 0x4000;
    puVar7 = (ulonglong *)(uVar9 + 0x8000);
    uVar18 = uVar8 + 0x8000;
  }
  else {
    uVar8 = *(ulonglong *)(puVar4 + 0xc);
  }
  FUN_140011d00(uVar8,0,0x4000);
  FUN_140011d00(*(undefined8 *)(puVar4 + 0x10),0,0x4000);
  uVar8 = uVar15;
  if (puVar4[6] != 0) {
    do {
      uVar13 = (int)uVar8 + 1;
      puVar1 = (uint *)(*(longlong *)(puVar4 + 0x10) + 0xc + uVar8 * 0x10);
      *puVar1 = *puVar1 | 0xffff;
      uVar8 = (ulonglong)uVar13;
    } while (uVar13 < (ushort)puVar4[6]);
  }
  if (cVar20 == '\0') {
    *(undefined **)(puVar4 + 0x24) = &DAT_140218220 + (longlong)(local_64 + -1) * 0xf000;
  }
  *(undefined4 *)(puVar4 + 8) = 0x10000;
  *(undefined4 *)(puVar4 + 2) = 0xffff;
  puVar4[4] = 0;
  lVar17 = 0x100;
  puVar4[7] = 0;
  uVar8 = uVar15;
  do {
    FUN_140011d00(*(longlong *)(puVar4 + 0x24) + uVar8,0,0x78);
    uVar5 = local_50;
    uVar8 = uVar8 + 0x78;
    lVar17 = lVar17 + -1;
  } while (lVar17 != 0);
  if (0 < *(int *)(param_1 + 0x15d24)) {
    do {
      FUN_14000fbb0(param_1 + 0x15940,uVar5,local_64,uVar15,cVar20,param_2,uVar18,puVar7);
      uVar18 = uVar18 + 0x30000;
      puVar7 = puVar7 + 0x6000;
      uVar13 = (int)uVar15 + 1;
      uVar15 = (ulonglong)uVar13;
    } while ((int)uVar13 < *(int *)(param_1 + 0x15d24));
  }
  uVar15 = 0;
  piVar10 = &DAT_140217800;
  do {
    if (*piVar10 == *(int *)(param_1 + 0x16068)) {
      *(undefined1 *)(param_1 + 0x15fa8) = 1;
      lVar17 = uVar15 * 0xe;
      *(undefined1 *)(param_1 + 0x15fa9) = (&DAT_140217806)[lVar17];
      *(undefined *)(param_1 + 0x15faa) = (&DAT_140217807)[lVar17];
      *(undefined1 *)(param_1 + 0x15fab) = *(undefined1 *)((longlong)&DAT_140217808 + lVar17);
      break;
    }
    uVar13 = (int)uVar15 + 1;
    uVar15 = (ulonglong)uVar13;
    piVar10 = (int *)((longlong)piVar10 + 0xe);
  } while (uVar13 < 8);
  *(ulonglong *)(param_1 + 0x16090) = *local_48;
  if (*local_48 == 0xffffffffffffffff) {
    *(undefined2 *)(param_1 + 0x15d08) = 0x101;
  }
  else {
    uVar15 = *local_48;
    *(undefined1 *)(param_1 + 0x1607d) = 1;
    iVar12 = ((uint)(uVar15 >> 0x19) & 0x7f) * 5;
    *(int *)(param_1 + 0x16080) = iVar12;
    *(int *)(param_1 + 0x16088) = iVar12;
    *(undefined4 *)((longlong)local_48 + 0x14) = 0x460000;
  }
  return;
}

