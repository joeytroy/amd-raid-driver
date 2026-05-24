// FUN_14000c5f0 @ 14000c5f0

longlong FUN_14000c5f0(longlong param_1,longlong param_2)

{
  ushort uVar1;
  ushort uVar2;
  ulonglong uVar3;
  undefined2 uVar4;
  longlong lVar5;
  ushort uVar6;
  bool bVar7;
  
  uVar1 = *(ushort *)(param_1 + 6);
  if ((uVar1 + 1) % (uint)*(ushort *)(param_1 + 10) != (uint)*(ushort *)(param_1 + 8)) {
    uVar2 = *(ushort *)(param_1 + 2);
    uVar6 = 1;
    if (1 < uVar2) {
      do {
        uVar3 = (ulonglong)((uint)*(ushort *)(param_1 + 4) + (uint)uVar6) % (ulonglong)uVar2;
        lVar5 = uVar3 * 0x78 + *(longlong *)(param_1 + 0x48);
        LOCK();
        bVar7 = *(longlong *)(lVar5 + 0x28) == 0;
        if (bVar7) {
          *(longlong *)(lVar5 + 0x28) = param_2;
        }
        UNLOCK();
        if (bVar7) {
          uVar4 = (undefined2)uVar3;
          *(undefined2 *)(param_1 + 4) = uVar4;
          FUN_140011d00(lVar5 + 0x30,0,0x40);
          *(longlong *)(lVar5 + 0x28) = param_2;
          *(undefined2 *)(lVar5 + 0x20) = uVar4;
          *(ushort *)(lVar5 + 0x22) = uVar1;
          *(short *)(param_1 + 6) =
               (short)((*(ushort *)(param_1 + 6) + 1) % (uint)*(ushort *)(param_1 + 10));
          return lVar5;
        }
        uVar2 = *(ushort *)(param_1 + 2);
        uVar6 = uVar6 + 1;
      } while (uVar6 < uVar2);
    }
  }
  return 0;
}

