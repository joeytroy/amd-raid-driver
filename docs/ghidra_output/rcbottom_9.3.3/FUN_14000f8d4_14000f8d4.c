// FUN_14000f8d4 @ 14000f8d4

void FUN_14000f8d4(longlong param_1,longlong param_2,longlong param_3)

{
  uint uVar1;
  undefined4 uVar2;
  uint uVar3;
  longlong lVar4;
  longlong lVar5;
  char cVar6;
  int iVar7;
  undefined8 local_80;
  undefined8 local_78;
  undefined8 uStack_70;
  undefined8 local_68;
  undefined8 uStack_60;
  undefined8 local_58;
  undefined8 uStack_50;
  undefined *local_48;
  
  lVar4 = *(longlong *)(param_2 + 0x178);
  if (*(uint *)(param_2 + 0x188) < 0x1001) {
    if ((*(uint *)(param_2 + 0x154) & 4) == 0) {
      cVar6 = FUN_14000ff20(param_1,param_2,param_3);
    }
    else {
      cVar6 = FUN_14000fe98();
    }
    if (cVar6 != '\0') {
      return;
    }
  }
  uVar1 = *(uint *)(lVar4 + 0x2c);
  lVar5 = *(longlong *)(lVar4 + 0x20);
  uVar2 = *(undefined4 *)(param_2 + 0x188);
  uVar3 = *(uint *)(param_2 + 0x154);
  local_68 = 0;
  local_48 = PTR_DAT_140015068;
  uStack_60 = 0x100000001;
  uStack_70 = 0;
  local_78 = 0x38;
  local_58 = 0;
  uStack_50 = 0;
  iVar7 = (**(code **)(DAT_140015968 + 0x310))
                    (DAT_140015990,*(undefined8 *)(param_1 + 0x16010),&local_78,&local_80);
  if (iVar7 < 0) {
    *(undefined4 *)(param_2 + 0x100) = *(undefined4 *)(param_2 + 0x13c);
    *(undefined1 *)(param_2 + 0x110) = 2;
    *(undefined2 *)(param_2 + 0x10e) = 0;
  }
  else {
    *(longlong *)(param_2 + 400) = param_1;
    *(longlong *)(param_2 + 0x198) = param_2;
    *(longlong *)(param_2 + 0x1a0) = param_3;
    *(longlong *)(param_2 + 0x1a8) = param_1;
    *(undefined8 *)(param_2 + 0x58) = local_80;
    iVar7 = (**(code **)(DAT_140015968 + 0x318))
                      (DAT_140015990,local_80,FUN_1400075a4,uVar3 >> 1 & 1,lVar4,
                       (ulonglong)uVar1 + lVar5 + (ulonglong)*(uint *)(param_2 + 0x180),uVar2);
    if ((-1 < iVar7) &&
       (iVar7 = (**(code **)(DAT_140015968 + 0x328))
                          (DAT_140015990,local_80,(longlong *)(param_2 + 400)), -1 < iVar7)) {
      return;
    }
    *(undefined4 *)(param_2 + 0x100) = *(undefined4 *)(param_2 + 0x13c);
    *(undefined1 *)(param_2 + 0x110) = 2;
    *(undefined2 *)(param_2 + 0x10e) = 0;
    (**(code **)(DAT_140015968 + 0x680))(DAT_140015990,local_80);
    *(undefined8 *)(param_2 + 0x58) = 0;
  }
  if (param_3 == 0) {
    FUN_140009cd4(param_2);
  }
  else {
    (**(code **)(DAT_140015968 + 0x838))(DAT_140015990,param_3,0);
  }
  return;
}

