// FUN_14000f47c @ 14000f47c

undefined1
FUN_14000f47c(longlong param_1,longlong param_2,undefined8 param_3,undefined8 param_4,
             undefined8 param_5)

{
  char cVar1;
  undefined1 uVar2;
  undefined8 uVar3;
  undefined8 uVar4;
  ulonglong uVar5;
  
  cVar1 = *(char *)(param_2 + 0x60);
  if (cVar1 != '\0') {
    if (cVar1 == '\x12') {
      uVar2 = FUN_14001014c(param_1,param_2,param_3,param_4);
      return uVar2;
    }
    if (cVar1 != '\x1b') {
      if (cVar1 == '5') {
        uVar2 = FUN_14000d620(param_1,param_2,param_3,param_4);
        return uVar2;
      }
      if ((cVar1 == -0x78) || (cVar1 == -0x76)) {
        if ((*(uint *)(param_1 + 0x15cdc) & 0x10) != 0) {
          uVar2 = FUN_14000efc0(param_1,param_2,param_3,param_4,param_5);
          return uVar2;
        }
        uVar5 = CONCAT62((int6)((ulonglong)param_4 >> 0x10),
                         -(ushort)((*(uint *)(param_2 + 0x154) & 4) != 0)) & 0xffffffffffff0500;
        uVar4 = CONCAT62((int6)(uVar5 >> 0x10),(short)uVar5 + 0xc00);
      }
      else {
        if (cVar1 == -0x62) {
          uVar2 = FUN_140010340(param_1,param_2,param_3,param_4);
          return uVar2;
        }
        uVar4 = 0x2600;
      }
      uVar3 = 2;
      goto LAB_14000f571;
    }
  }
  uVar4 = 0;
  uVar3 = 0;
LAB_14000f571:
  FUN_14000cbf0(param_2,param_4,uVar3,uVar4);
  return 1;
}

