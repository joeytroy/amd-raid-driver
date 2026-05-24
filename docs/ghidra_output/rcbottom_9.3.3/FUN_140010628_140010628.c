// FUN_140010628 @ 140010628

void FUN_140010628(longlong param_1)

{
  ulonglong *puVar1;
  ulonglong uVar2;
  
  puVar1 = *(ulonglong **)(param_1 + 0x159a8);
  if (*(int *)(param_1 + 0x16080) == 0) {
    if (*(char *)(param_1 + 0x1607d) != '\0') {
      *(undefined2 *)(param_1 + 0x15d08) = 0x101;
      *(undefined1 *)(param_1 + 0x1607d) = 0;
    }
  }
  else {
    *(int *)(param_1 + 0x16080) = *(int *)(param_1 + 0x16080) + -1;
    if ((*(uint *)((longlong)puVar1 + 0x1c) & 1) == 0) {
      uVar2 = *puVar1;
      puVar1[5] = *(ulonglong *)(*(longlong *)(param_1 + 0x15940) + 0x28);
      puVar1[6] = *(ulonglong *)(*(longlong *)(param_1 + 0x15940) + 0x30);
      *(uint *)((longlong)puVar1 + 0x24) =
           (uint)*(ushort *)(*(longlong *)(param_1 + 0x15940) + 0xc) * 0x10000 - 1 & 0xfff0000 |
           *(ushort *)(*(longlong *)(param_1 + 0x15940) + 10) - 1 & 0xfff;
      *(undefined4 *)((longlong)puVar1 + 0x14) = 0x460001;
      *(undefined2 *)(param_1 + 0x1607c) = 1;
      *(uint *)(param_1 + 0x16080) = ((uint)(uVar2 >> 0x19) & 0x7f) * 5;
      return;
    }
  }
  return;
}

