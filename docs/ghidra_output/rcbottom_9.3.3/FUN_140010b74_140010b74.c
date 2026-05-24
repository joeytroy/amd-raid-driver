// FUN_140010b74 @ 140010b74

undefined1 FUN_140010b74(undefined8 param_1,longlong param_2,undefined8 param_3,undefined8 param_4)

{
  undefined1 uVar1;
  
  uVar1 = 0;
  if (*(char *)(param_2 + 0x60) == '\x01') {
    uVar1 = FUN_140010868(param_1,param_2,param_3,param_4);
  }
  else if (*(char *)(param_2 + 0x60) == '\x02') {
    FUN_14000cbf0(param_2,param_4,2,0x2000);
    uVar1 = 1;
  }
  return uVar1;
}

