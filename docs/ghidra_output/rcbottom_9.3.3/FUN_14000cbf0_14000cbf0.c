// FUN_14000cbf0 @ 14000cbf0

void FUN_14000cbf0(longlong param_1,longlong param_2,char param_3,ushort param_4)

{
  undefined4 uVar1;
  undefined1 local_28 [24];
  
  if (*(longlong *)(param_1 + 0x58) != 0) {
    (**(code **)(DAT_140015968 + 0x338))(DAT_140015990,*(longlong *)(param_1 + 0x58),local_28);
    (**(code **)(DAT_140015968 + 0x680))(DAT_140015990,*(undefined8 *)(param_1 + 0x58));
    *(undefined8 *)(param_1 + 0x58) = 0;
  }
  *(undefined4 *)(param_1 + 0x100) = *(undefined4 *)(param_1 + 0x13c);
  *(char *)(param_1 + 0x110) = param_3;
  if (param_3 == '\0') {
    uVar1 = *(undefined4 *)(param_1 + 0x140);
    *(undefined2 *)(param_1 + 0x10e) = 0;
  }
  else {
    *(ushort *)(param_1 + 0x10e) = param_4 << 8 | param_4 >> 8;
    uVar1 = 0;
  }
  *(undefined4 *)(param_1 + 0x104) = uVar1;
  *(undefined2 *)(param_1 + 0x10c) = 0;
  *(undefined1 *)(param_1 + 0x112) = 0;
  if (param_2 == 0) {
    FUN_140009cd4(param_1);
  }
  else {
    (**(code **)(DAT_140015968 + 0x838))(DAT_140015990,param_2,0);
  }
  return;
}

