// FUN_1400110c0 @ 1400110c0

/* WARNING: Function: __security_check_cookie replaced with injection: security_check_cookie */

undefined8 FUN_1400110c0(uint param_1,undefined4 param_2,undefined1 param_3,undefined2 param_4)

{
  undefined8 uVar1;
  undefined1 auStack_58 [32];
  undefined1 local_38;
  undefined1 local_37;
  undefined1 local_36;
  undefined8 local_30;
  undefined4 local_28;
  undefined4 local_24;
  undefined1 *local_20;
  ulonglong local_18;
  
  local_18 = DAT_140015180 ^ (ulonglong)auStack_58;
  if (param_1 < 0xb) {
    local_28 = 0;
    local_37 = (undefined1)param_4;
    local_36 = (undefined1)((ushort)param_4 >> 8);
    local_20 = &local_38;
    local_30 = 1;
    local_24 = 3;
    local_38 = param_3;
    uVar1 = FUN_140010d90(&DAT_1400159c0 + (ulonglong)param_1 * 0x40,param_2,&local_30);
  }
  else {
    uVar1 = 0xffffffff;
  }
  return uVar1;
}

