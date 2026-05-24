// FUN_1400080c8 @ 1400080c8

void FUN_1400080c8(longlong param_1,undefined8 param_2,undefined4 param_3,undefined8 param_4,
                  undefined4 param_5,undefined4 param_6,undefined4 param_7,undefined4 param_8,
                  undefined4 param_9)

{
  undefined4 uVar1;
  longlong lVar2;
  bool bVar3;
  
  if (DAT_140015743 == '\0') {
    (**(code **)(DAT_140015968 + 0x9e0))(DAT_140015990,DAT_140217870);
  }
  lVar2 = (ulonglong)DAT_1400152a0 * 0x38;
  DAT_1400152a0 = DAT_1400152a0 + 1;
  bVar3 = DAT_140015743 == '\0';
  *(undefined8 *)(&DAT_140217880 + lVar2) = 0;
  *(undefined8 *)(&DAT_140217888 + lVar2) = 0;
  *(undefined8 *)(&DAT_140217890 + lVar2) = 0;
  *(undefined8 *)(&DAT_140217898 + lVar2) = 0;
  *(undefined8 *)(&DAT_1402178a0 + lVar2) = 0;
  *(undefined8 *)(&DAT_1402178a8 + lVar2) = 0;
  *(undefined8 *)(&DAT_1402178b0 + lVar2) = 0;
  *(undefined4 *)(&DAT_140217880 + lVar2) = 2;
  uVar1 = *(undefined4 *)(param_1 + 0x16058);
  *(undefined8 *)(&DAT_140217888 + lVar2) = param_2;
  *(undefined4 *)(&DAT_140217890 + lVar2) = param_3;
  *(undefined8 *)(&DAT_140217898 + lVar2) = param_4;
  *(undefined4 *)(&DAT_1402178a0 + lVar2) = param_5;
  *(undefined4 *)(&DAT_1402178a4 + lVar2) = param_6;
  *(undefined4 *)(&DAT_1402178a8 + lVar2) = param_7;
  *(undefined4 *)(&DAT_1402178ac + lVar2) = param_8;
  *(undefined4 *)(&DAT_1402178b0 + lVar2) = param_9;
  *(undefined4 *)(&DAT_140217884 + lVar2) = uVar1;
  if (bVar3) {
    (**(code **)(DAT_140015968 + 0x9e8))(DAT_140015990,DAT_140217870);
  }
  return;
}

