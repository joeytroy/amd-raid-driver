// FUN_14000d4f0 @ 14000d4f0

undefined1
FUN_14000d4f0(longlong param_1,longlong param_2,undefined8 param_3,longlong param_4,
             undefined8 param_5)

{
  char cVar1;
  longlong lVar2;
  undefined1 uVar3;
  
  cVar1 = *(char *)(param_2 + 0x14c);
  if ((cVar1 == '\x01') || (cVar1 == '\x04')) {
    lVar2 = *(longlong *)(param_1 + 0x15948 + (ulonglong)*(uint *)(param_1 + 0x15968) * 8);
    if ((*(ushort *)(lVar2 + 6) + 1) % (uint)*(ushort *)(lVar2 + 10) != (uint)*(ushort *)(lVar2 + 8)
       ) {
      uVar3 = FUN_14000f47c(param_1,param_2,param_3,param_4,param_5);
      return uVar3;
    }
  }
  else {
    if (cVar1 != '\n') {
      *(undefined4 *)(param_2 + 0x100) = *(undefined4 *)(param_2 + 0x13c);
      *(undefined1 *)(param_2 + 0x110) = 2;
      *(undefined1 *)(param_2 + 0x112) = 0;
      if (param_4 != 0) {
        (**(code **)(DAT_140015968 + 0x838))(DAT_140015990,param_4,0);
        return 1;
      }
      FUN_140009cd4(param_2);
      return 1;
    }
    lVar2 = *(longlong *)(param_1 + 0x15948 + (ulonglong)*(uint *)(param_1 + 0x15968) * 8);
    if ((*(ushort *)(lVar2 + 6) + 1) % (uint)*(ushort *)(lVar2 + 10) != (uint)*(ushort *)(lVar2 + 8)
       ) {
      uVar3 = FUN_14000f354();
      return uVar3;
    }
  }
  FUN_14000eef4(param_1,param_2,param_3,param_4);
  return 0;
}

