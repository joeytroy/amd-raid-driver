// FUN_14004cbb0 @ 14004cbb0

void FUN_14004cbb0(longlong param_1)

{
  char cVar1;
  uint uVar2;
  longlong lVar3;
  longlong lVar4;
  bool bVar5;
  int iVar6;
  uint *puVar7;
  ulonglong uVar8;
  longlong *plVar9;
  ulonglong uVar10;
  uint uVar11;
  uint uVar12;
  ulonglong uVar13;
  
  lVar3 = *(longlong *)(param_1 + 0x234);
  uVar8 = (ulonglong)DAT_1400f8160;
  uVar12 = 0;
  uVar2 = *(uint *)(*(longlong *)(lVar3 + 0x2ec) + 0xb8);
  uVar13 = (ulonglong)uVar2;
  if ((DAT_1400f8160 == 0) && (1 < DAT_140081628)) {
    puVar7 = &DAT_1400f3b68;
    uVar10 = (ulonglong)(DAT_140081628 - 1);
    do {
      if (*(code **)(puVar7 + 4) == FUN_14004cbb0) {
        uVar8 = (ulonglong)*puVar7;
      }
      puVar7 = puVar7 + 0x18;
      uVar10 = uVar10 - 1;
    } while (uVar10 != 0);
    DAT_1400f8160 = (uint)uVar8;
  }
  if (((code *)(&DAT_1400f3b18)[uVar8 * 0xc] == FUN_14004cbb0) &&
     ((&DAT_1400f3b10)[uVar8 * 0x18] != 0)) {
    if (((&DAT_1400f3b14)[uVar8 * 0x18] == (uint)*(ushort *)(param_1 + 4)) ||
       ((&DAT_1400f3b14)[uVar8 * 0x18] == 0xffff)) {
      *(undefined4 *)(param_1 + 0x24) = 0x19;
      (&DAT_1400f3b10)[uVar8 * 0x18] = (&DAT_1400f3b10)[uVar8 * 0x18] + -1;
    }
  }
  iVar6 = FUN_140062bb8(lVar3);
  if (iVar6 != 0) {
    DAT_1400f7f70 = DAT_1400f7f70 + -1;
    cVar1 = *(char *)(lVar3 + 0x294);
    uVar11 = (uint)(*(int *)(*(longlong *)(lVar3 + 0x2ec) + 4) == 0x1bfa);
    if (DAT_1400f7f74 != 0) {
      DAT_1400f7f74 = DAT_1400f7f74 + -1;
    }
    if (uVar2 != 0) {
      plVar9 = (longlong *)(lVar3 + 0x3c8);
      uVar12 = 0;
      do {
        lVar4 = *plVar9;
        if ((*(uint *)(lVar4 + 0xc) & 0x80000) == 0) {
          if (*(int *)(lVar4 + 0x24) != 1) {
            (**(code **)(DAT_1400f6bf8 + 0xac))
                      ("FAIL at RC_CachedStripeWriteWritesDone(ERROR) dev:%d blk:%d size:%d\n",
                       *(undefined2 *)(lVar4 + 4),*(undefined4 *)(lVar4 + 0x264),
                       *(undefined4 *)(lVar4 + 0x20));
            FUN_140021a50(lVar4);
            goto LAB_14004cd04;
          }
        }
        else {
LAB_14004cd04:
          uVar12 = uVar12 + 1;
        }
        plVar9 = plVar9 + 1;
        uVar13 = uVar13 - 1;
      } while (uVar13 != 0);
    }
    if ((uVar11 + 2 <= uVar12) || (bVar5 = false, cVar1 != '\0')) {
      bVar5 = true;
      *(undefined4 *)(lVar3 + 0x24) = 0x19;
    }
    uVar12 = 0;
    if (uVar2 != 0) {
      plVar9 = (longlong *)(lVar3 + 0x3c8);
      do {
        lVar4 = *plVar9;
        *(undefined2 *)(*(longlong *)(lVar4 + 0x2d4) + 2) = 0;
        if (bVar5) {
          if (uVar12 < uVar2 - (uVar11 + 1)) {
            iVar6 = FUN_140013ea0(*(undefined8 *)(lVar4 + 0x2d4));
            if (iVar6 != 0) {
              if (DAT_1400f6bf8 == 0) {
                FUN_14004c838(0x418,*(ushort *)(*(longlong *)(lVar4 + 0x2d4) + 0x90) & 0x7fff,0,0,0,
                              0,0,0,0);
              }
              else {
                FUN_14004c4c0(*(undefined4 *)(DAT_1400f6bf8 + 0x15c),0x418,
                              *(ushort *)(*(longlong *)(lVar4 + 0x2d4) + 0x90) & 0x7fff,0,0,0,0,0,0,
                              0);
              }
            }
          }
          FUN_140015278(lVar4);
        }
        else {
          FUN_140015404(lVar4);
        }
        FUN_1400120c0(*(undefined8 *)(lVar4 + 0x2d4));
        FUN_140057658(*(undefined8 *)(lVar4 + 0x2d4));
        *(undefined4 *)(*(longlong *)(lVar4 + 0x2d4) + 4) = 0;
        FUN_1400672dc(lVar4,0,0,0,0,1);
        uVar12 = uVar12 + 1;
        plVar9 = plVar9 + 1;
      } while (uVar12 < uVar2);
    }
    FUN_1400658f8(lVar3,uVar2);
    if ((DAT_1400f3268 != 0) && (*(longlong *)(lVar3 + 0x2f4) != 0)) {
      FUN_140040194();
    }
    FUN_140015bd4(lVar3);
    if (*(code **)(lVar3 + 0x188) == FUN_140011c08) {
      FUN_140058520(*(undefined8 *)(lVar3 + 0x234));
    }
    else {
      FUN_1400575d4(lVar3);
      FUN_1400121a8();
    }
  }
  return;
}

