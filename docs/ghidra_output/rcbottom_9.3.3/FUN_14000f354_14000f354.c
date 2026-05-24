// FUN_14000f354 @ 14000f354

undefined8 FUN_14000f354(longlong param_1,longlong param_2,undefined8 param_3,undefined8 param_4)

{
  char cVar1;
  undefined8 uVar2;
  undefined8 uVar3;
  
  cVar1 = *(char *)(param_2 + 0x60);
  if (*(char *)(param_2 + 0x14c) == '\v') {
    if (cVar1 == '\x02') {
      FUN_14000d8bc(param_1,param_2,param_3,param_4);
      return 1;
    }
    if (cVar1 != '\t') {
      if (cVar1 == -0x7e) {
        FUN_14000da3c(param_1,param_2,param_3,param_4);
        return 1;
      }
      if (cVar1 == -1) {
        uVar2 = (**(code **)(DAT_140015968 + 0x108))(DAT_140015990,*(undefined8 *)(param_1 + 0x20));
        IoRequestDeviceEject(uVar2);
        uVar2 = 0;
        uVar3 = 0;
        goto LAB_14000f432;
      }
      goto LAB_14000f429;
    }
    if ((*(uint *)(param_1 + 0x15cdc) & 1) != 0) {
      FUN_14000f7c8(param_1,param_2,param_4,*(undefined1 *)(param_2 + 0x61));
      return 1;
    }
    uVar2 = 0;
  }
  else {
    if (cVar1 == '\x04') {
      FUN_14001075c(param_1,param_2,param_3,param_4);
      return 1;
    }
    if (cVar1 == '\t') {
      FUN_14000d3bc(param_1,param_2,param_3,param_4);
      return 1;
    }
LAB_14000f429:
    uVar2 = 0x2600;
  }
  uVar3 = 2;
LAB_14000f432:
  FUN_14000cbf0(param_2,param_4,uVar3,uVar2);
  return 1;
}

