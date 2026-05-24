// FUN_140004398 @ 140004398

void FUN_140004398(void)

{
  longlong lVar1;
  longlong lVar2;
  
  if (DAT_1400813b4 != 0) {
    FUN_140001b1c("##### %s(): At line %d\n","RC_HW_BTM_ManageLEDSchedule",0x7de);
    FUN_140001b1c("#####      RC_InitFinished %d\n",DAT_1400f328c);
    FUN_140001b1c("#####      RC_HW_BTM_IsHiberDump() %d\n",DAT_140622ee8);
    FUN_140001b1c("#####      RC_FreeIocCount %d\n",DAT_1400f8180);
    FUN_140001b1c("#####      RC_CleanLru.BufferCount %d\n",DAT_1400eee64);
    FUN_140001b1c("#####      RC_FreeDataBufferCount %d\n",DAT_1400eeec8);
    FUN_140001b1c("#####      RC_ActiveBuffersLimitLower %d\n",DAT_1400f3250);
  }
  while( true ) {
    if ((((DAT_1400f328c == 0) || (DAT_140622ee8 != 0)) || (DAT_1400f8180 == 0)) ||
       (((uint)(DAT_1400eeec8 + DAT_1400eee64) < DAT_1400f3250 >> 1 ||
        (lVar1 = FUN_1400031d0(0,&DAT_140085b5c), lVar1 == 0)))) goto LAB_14000451f;
    if (DAT_1400813b4 != 0) {
      FUN_140001b1c("##### %s(): At line %d\n","RC_HW_BTM_ManageLEDSchedule",0x7ec);
    }
    lVar2 = FUN_140057bf0(0,0);
    if (lVar2 == 0) break;
    *(undefined4 *)(lVar2 + 0xc) = 0x200002;
    *(longlong *)(lVar2 + 0x5c) = lVar1 + 0x28;
    *(undefined4 *)(lVar2 + 4) = 0x1ffff;
    *(undefined4 *)(lVar2 + 100) = 0x400;
    *(undefined4 *)(lVar2 + 0x20) = 1;
    *(code **)(lVar2 + 0x28) = FUN_140003b84;
    *(longlong *)(lVar2 + 0x3a4) = lVar1;
    FUN_1400024a0(lVar1);
    FUN_140001f68();
  }
  FUN_140004834(lVar1);
LAB_14000451f:
  if (DAT_1400813b4 != 0) {
    FUN_140001b1c("##### %s(): At line %d\n","RC_HW_BTM_ManageLEDSchedule",0x80f);
    DAT_1400813b4 = DAT_1400813b4 + -1;
  }
  return;
}

