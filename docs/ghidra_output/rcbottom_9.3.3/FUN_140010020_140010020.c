// FUN_140010020 @ 140010020

void FUN_140010020(longlong param_1)

{
  longlong lVar1;
  longlong lVar2;
  char cVar3;
  char cVar4;
  uint uVar5;
  undefined1 local_38 [32];
  
  lVar1 = param_1 + 0x15940;
  lVar2 = *(longlong *)(param_1 + 0x159a8);
  if (*(char *)(param_1 + 0x1c2e4) == '\0') {
    if (*(int *)(param_1 + 0x15fec) != 0) {
      FUN_14000d390(lVar1);
    }
    cVar3 = FUN_1400106f0(lVar1,0x14,1,0,2000);
    if (cVar3 != '\0') {
      FUN_14000d2e0(lVar1,FUN_14000d0ac);
      cVar3 = FUN_140006000(param_1,0,local_38);
      uVar5 = 0;
      do {
        if ((*(uint *)(param_1 + 0x15cdc) & 0x20) == 0) break;
        cVar4 = FUN_14000cb1c(param_1,0xffffffff);
        if (cVar4 != '\0') {
          FUN_14000edb4(param_1,0,0);
        }
        KeStallExecutionProcessor(1000);
        uVar5 = uVar5 + 1;
      } while (uVar5 < 2000);
      if (cVar3 != '\0') {
        KeReleaseInStackQueuedSpinLock(local_38);
      }
      *(uint *)(lVar2 + 0x14) = *(uint *)(lVar2 + 0x14) & 0xffff3fff;
      *(uint *)(lVar2 + 0x14) = *(uint *)(lVar2 + 0x14) | 0x4000;
      FUN_1400106f0(lVar1,0x1c,8,0,*(uint *)(param_1 + 0x15c94) / 1000);
    }
  }
  return;
}

