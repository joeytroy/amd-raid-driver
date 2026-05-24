// FUN_14000eef4 @ 14000eef4

void FUN_14000eef4(longlong param_1,undefined8 param_2,undefined8 param_3,undefined8 param_4)

{
  longlong lVar1;
  
  lVar1 = ExAllocatePool2(0x40,0x28,0x72634148);
  *(undefined8 *)(lVar1 + 0x10) = param_2;
  *(undefined8 *)(lVar1 + 0x18) = param_3;
  *(undefined8 *)(lVar1 + 0x20) = param_4;
  if (DAT_140015743 == '\0') {
    (**(code **)(DAT_140015968 + 0x9e0))(DAT_140015990,*(undefined8 *)(param_1 + 0x15f88));
  }
  if (*(longlong *)(param_1 + 90000) == 0) {
    *(longlong *)(param_1 + 90000) = lVar1;
  }
  else {
    **(longlong **)(param_1 + 0x15f98) = lVar1;
  }
  *(longlong *)(param_1 + 0x15f98) = lVar1;
  if (DAT_140015743 == '\0') {
    (**(code **)(DAT_140015968 + 0x9e8))(DAT_140015990,*(undefined8 *)(param_1 + 0x15f88));
  }
  return;
}

