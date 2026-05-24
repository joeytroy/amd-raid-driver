// FUN_14000d7e0 @ 14000d7e0

void FUN_14000d7e0(longlong param_1)

{
  int iVar1;
  
  if (*(int *)(&DAT_140015c40 + (ulonglong)*(uint *)(param_1 + 0x16058) * 4) == 0) {
    LOCK();
    UNLOCK();
    iVar1 = DAT_140015754 + 1;
    DAT_140015754 = DAT_140015754 + 1;
    *(int *)(&DAT_140015c40 + (ulonglong)*(uint *)(param_1 + 0x16058) * 4) = iVar1;
  }
  return;
}

