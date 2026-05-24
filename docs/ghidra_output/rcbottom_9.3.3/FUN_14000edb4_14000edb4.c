// FUN_14000edb4 @ 14000edb4

void FUN_14000edb4(longlong param_1,int param_2,char param_3)

{
  longlong *plVar1;
  longlong lVar2;
  longlong lVar3;
  int iVar4;
  longlong *plVar5;
  int iVar6;
  
  plVar1 = (longlong *)(param_1 + 0x15940);
  lVar3 = *plVar1;
  lVar2 = *(longlong *)(param_1 + 0x159a8);
  iVar4 = 1 << ((byte)param_2 & 0x1f);
  if (param_2 == -1) {
    iVar4 = -1;
    if ((*(ushort *)
          (*(longlong *)(lVar3 + 0x20) + 0xe + (ulonglong)*(ushort *)(lVar3 + 0x10) * 0x10) & 1) ==
        *(ushort *)(lVar3 + 0x12)) {
      FUN_14000db84(plVar1,lVar3);
    }
    iVar6 = 0;
    if (0 < *(int *)(param_1 + 0x15d24)) {
      plVar5 = (longlong *)(param_1 + 0x15948);
      do {
        lVar3 = *plVar5;
        if ((*(ushort *)
              (*(longlong *)(lVar3 + 0x20) + 0xe + (ulonglong)*(ushort *)(lVar3 + 0x10) * 0x10) & 1)
            == *(ushort *)(lVar3 + 0x12)) {
          FUN_14000db84(plVar1);
        }
        iVar6 = iVar6 + 1;
        plVar5 = plVar5 + 1;
      } while (iVar6 < *(int *)(param_1 + 0x15d24));
    }
  }
  else {
    if (param_2 == 0) {
      if ((*(ushort *)
            (*(longlong *)(lVar3 + 0x20) + 0xe + (ulonglong)*(ushort *)(lVar3 + 0x10) * 0x10) & 1)
          != *(ushort *)(lVar3 + 0x12)) goto LAB_14000eec0;
    }
    else if ((*(int *)(param_1 + 0x15d24) <= param_2 + -1) ||
            (lVar3 = *(longlong *)(param_1 + 0x15948 + (ulonglong)(param_2 - 1) * 8),
            (*(ushort *)
              (*(longlong *)(lVar3 + 0x20) + 0xe + (ulonglong)*(ushort *)(lVar3 + 0x10) * 0x10) & 1)
            != *(ushort *)(lVar3 + 0x12))) goto LAB_14000eec0;
    FUN_14000db84(plVar1,lVar3);
  }
LAB_14000eec0:
  if (((*(char *)(param_1 + 0xb5) == '\0') && (param_3 != '\0')) &&
     (*(char *)(param_1 + 0x1c7b8) == '\0')) {
    *(int *)(lVar2 + 0x10) = iVar4;
  }
  return;
}

