// FUN_140011b34 @ 140011b34

/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

void FUN_140011b34(longlong param_1)

{
  if (DAT_1400f45c4 != 0) {
    (**(code **)(DAT_1400f6bf8 + 0xac))
              ("%s: raw command during rescan, failing IOC\n","RC_StartRawIo");
    *(undefined4 *)(param_1 + 0x24) = 0x1d;
                    /* WARNING: Could not recover jumptable at 0x000140011b75. Too many branches */
                    /* WARNING: Treating indirect jump as call */
    (**(code **)(param_1 + 0x28))(param_1);
    return;
  }
  _DAT_1400eb3e8 = _DAT_1400eb3e8 + 1;
  DAT_1400f8394 = DAT_1400f8394 + 1;
  if (DAT_1400f8398 < DAT_1400f8394) {
    DAT_1400f8398 = DAT_1400f8394;
  }
  if (*(int *)(param_1 + 0x1a4) == 0) {
    *(undefined4 *)(param_1 + 0x1a4) = DAT_1400f3d34;
  }
  if (*(int *)(param_1 + 0x1a8) == 0) {
    *(undefined4 *)(param_1 + 0x1a8) = 0xf;
  }
  *(undefined8 *)(param_1 + 0x188) = *(undefined8 *)(param_1 + 0x28);
  *(undefined4 *)(param_1 + 0x184) = *(undefined4 *)(param_1 + 0xc);
  *(code **)(param_1 + 0x28) = FUN_1400118f8;
  *(undefined4 *)(param_1 + 0x24) = 1;
  FUN_140057aac(param_1,param_1 + 0x160,0);
  return;
}

