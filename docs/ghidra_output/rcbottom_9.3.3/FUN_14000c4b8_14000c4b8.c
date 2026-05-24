// FUN_14000c4b8 @ 14000c4b8

void FUN_14000c4b8(longlong param_1,char param_2)

{
  int iVar1;
  longlong lVar2;
  uint uVar3;
  
  uVar3 = 0x80110800;
  *(undefined2 *)(param_1 + 0x3a8) = 1;
  *(undefined4 *)(param_1 + 0x3ac) = 0x80110800;
  *(uint *)(param_1 + 0x3c0) = *(ushort *)(*(longlong *)(param_1 + 8) + 10) - 1;
  if ((*(byte *)(param_1 + 0xd1) & 3) == 1) {
    uVar3 = 0x80510800;
    *(undefined4 *)(param_1 + 0x3ac) = 0x80510800;
  }
  lVar2 = *(longlong *)(param_1 + 0x660);
  iVar1 = *(int *)(lVar2 + 0x1c7b0);
  if (iVar1 == 8) {
    uVar3 = uVar3 | 0x80;
LAB_14000c537:
    *(uint *)(param_1 + 0x3ac) = uVar3;
  }
  else {
    if (iVar1 == 4) {
      uVar3 = uVar3 | 0x40;
      goto LAB_14000c537;
    }
    if (iVar1 == 2) {
      uVar3 = uVar3 | 0x20;
      goto LAB_14000c537;
    }
  }
  iVar1 = *(int *)(lVar2 + 0x1c7b4);
  if (iVar1 == 5) {
    uVar3 = uVar3 | 0x10;
  }
  else if (iVar1 == 4) {
    uVar3 = uVar3 | 8;
  }
  else {
    if (iVar1 != 2) goto LAB_14000c568;
    uVar3 = uVar3 | 0x10000000;
  }
  *(uint *)(param_1 + 0x3ac) = uVar3;
LAB_14000c568:
  if (*(char *)(lVar2 + 0x1c855) == '\0') {
    uVar3 = uVar3 | 0x800000;
    *(uint *)(param_1 + 0x3ac) = uVar3;
  }
  if (*(char *)(param_1 + 0xd4) == '\f') {
    *(longlong *)(param_1 + 0x3b8) = *(longlong *)(param_1 + 0x3b8) << 3;
    *(uint *)(param_1 + 0x3ac) = uVar3 | 0x8000;
  }
  else if (*(char *)(param_1 + 0xd4) == '\t') {
    *(uint *)(param_1 + 0x3ac) = uVar3 | 0x200000;
  }
  else {
    *(undefined1 *)(param_1 + 0x3a8) = 0;
  }
  if (((param_2 == '\0') && (*(int *)(param_1 + 0x6ac) != 0)) &&
     (*(char *)(lVar2 + 0x1c7b8) == '\0')) {
    FUN_14000ec20(lVar2,*(undefined4 *)(lVar2 + 0x16180));
  }
  return;
}

