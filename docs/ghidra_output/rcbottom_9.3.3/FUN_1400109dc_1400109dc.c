// FUN_1400109dc @ 1400109dc

void FUN_1400109dc(longlong param_1,uint *param_2)

{
  uint uVar1;
  short sVar2;
  
  if (*(char *)(param_1 + 0x15fa8) != '\0') {
    if ((*param_2 & 1) == 0) {
      *(uint *)(param_1 + 0x15fb0) = *(uint *)(param_1 + 0x15fac);
      uVar1 = *(uint *)(param_1 + 0x15fac) | *param_2;
      *(uint *)(param_1 + 0x15fac) = uVar1;
      uVar1 = ~param_2[1] & uVar1;
      *(uint *)(param_1 + 0x15fac) = uVar1;
    }
    else {
      *(undefined4 *)(param_1 + 0x15fac) = *(undefined4 *)(param_1 + 0x15fb0);
      *param_2 = *param_2 & 0xfffffffe;
      uVar1 = *(uint *)(param_1 + 0x15fac);
    }
    sVar2 = ((uVar1 & 0xa0000c20) != 0) + 0x100;
    if ((uVar1 & 0x1200) != 0) {
      sVar2 = 0x103;
    }
    if ((uVar1 >> 0x11 & 1) != 0) {
      sVar2 = 0x102;
    }
    FUN_1400110c0(*(undefined1 *)(param_1 + 0x15fa9),*(undefined1 *)(param_1 + 0x15faa),
                  *(undefined1 *)(param_1 + 0x15fab),sVar2);
  }
  return;
}

