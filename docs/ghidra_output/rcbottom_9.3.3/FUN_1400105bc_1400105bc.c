// FUN_1400105bc @ 1400105bc

void FUN_1400105bc(longlong param_1)

{
  if (*(int *)(param_1 + 0x16080) == 0) {
    if (*(char *)(param_1 + 0x1607c) != '\0') {
      *(undefined2 *)(param_1 + 0x15d08) = 0x101;
    }
  }
  else {
    *(int *)(param_1 + 0x16080) = *(int *)(param_1 + 0x16080) + -1;
    if ((*(uint *)(*(longlong *)(param_1 + 0x159a8) + 0x1c) & 1) != 0) {
      *(undefined1 *)(param_1 + 0x1607c) = 0;
      *(uint *)(param_1 + 0x15cdc) = *(uint *)(param_1 + 0x15cdc) | 1;
      FUN_14000dfbc();
      return;
    }
  }
  return;
}

