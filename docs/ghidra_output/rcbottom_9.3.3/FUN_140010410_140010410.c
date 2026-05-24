// FUN_140010410 @ 140010410

undefined1 FUN_140010410(longlong param_1,longlong param_2,undefined8 param_3,undefined8 param_4)

{
  byte bVar1;
  longlong lVar2;
  longlong lVar3;
  undefined1 uVar4;
  
  *(undefined8 *)(param_2 + 0x58) = 0;
  *(undefined8 *)(param_2 + 0x48) = 0;
  bVar1 = *(byte *)(param_2 + 0x14c);
  lVar2 = *(longlong *)(param_2 + 0x178);
  if ((bVar1 == 1) || (bVar1 == 4)) {
    lVar3 = *(longlong *)(param_1 + 0x15948 + (ulonglong)*(uint *)(param_1 + 0x15968) * 8);
    if ((*(ushort *)(lVar3 + 6) + 1) % (uint)*(ushort *)(lVar3 + 10) == (uint)*(ushort *)(lVar3 + 8)
       ) goto LAB_140010569;
    if (lVar2 == 0) {
      uVar4 = FUN_14000f47c(param_1,param_2,param_3,param_4,0);
      return uVar4;
    }
  }
  else {
    if (bVar1 == 6) {
      uVar4 = FUN_140010b74(param_1,param_2,param_3,param_4);
      return uVar4;
    }
    if (bVar1 < 10) {
LAB_140010515:
      FUN_14000cbf0(param_2,param_4,2,0x2000);
      return 1;
    }
    if (bVar1 < 0xc) {
      lVar3 = *(longlong *)(param_1 + 0x15948 + (ulonglong)*(uint *)(param_1 + 0x15968) * 8);
      if ((*(ushort *)(lVar3 + 6) + 1) % (uint)*(ushort *)(lVar3 + 10) ==
          (uint)*(ushort *)(lVar3 + 8)) {
LAB_140010569:
        FUN_14000eef4(param_1,param_2,param_3,param_4);
        return 0;
      }
      if (lVar2 == 0) {
        uVar4 = FUN_14000f354(param_1,param_2,param_3,param_4);
        return uVar4;
      }
    }
    else {
      if (bVar1 != 0xc) goto LAB_140010515;
      lVar3 = *(longlong *)(param_1 + 0x15948 + (ulonglong)*(uint *)(param_1 + 0x15968) * 8);
      if ((*(ushort *)(lVar3 + 6) + 1) % (uint)*(ushort *)(lVar3 + 10) ==
          (uint)*(ushort *)(lVar3 + 8)) goto LAB_140010569;
      if (lVar2 == 0) {
        uVar4 = FUN_14000e948(param_1,param_2,param_3,param_4);
        return uVar4;
      }
    }
  }
  FUN_14000f8d4(param_1,param_2,param_4);
  return 1;
}

