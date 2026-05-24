// FUN_140009f3c @ 140009f3c

/* WARNING: Function: __security_check_cookie replaced with injection: security_check_cookie */
/* WARNING: Removing unreachable block (ram,0x00014000a629) */
/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

void FUN_140009f3c(undefined8 param_1,undefined8 param_2)

{
  longlong lVar1;
  longlong *plVar2;
  undefined1 uVar3;
  int iVar4;
  undefined8 uVar5;
  longlong lVar6;
  ulonglong uVar7;
  longlong lVar8;
  uint uVar9;
  undefined *puVar10;
  ulonglong uVar11;
  undefined8 *puVar12;
  ulonglong uVar13;
  undefined1 auStack_c8 [32];
  char *local_a8;
  char *local_a0;
  undefined8 local_98;
  undefined1 local_90 [24];
  undefined8 local_78;
  longlong *plStack_70;
  ulonglong local_68;
  undefined8 uStack_60;
  ulonglong local_58;
  ulonglong local_40;
  
  local_40 = DAT_140015180 ^ (ulonglong)auStack_c8;
  uVar11 = 0;
  DAT_140015743 = '\0';
  uVar5 = (**(code **)(DAT_140015968 + 0x4e8))(DAT_140015990,param_1);
  lVar6 = (**(code **)(DAT_140015968 + 0x650))(DAT_140015990,uVar5,PTR_DAT_140015040);
  local_58 = 0;
  plStack_70 = (longlong *)0x0;
  local_78 = 0x28;
  local_68 = 0;
  uStack_60 = 0;
  (**(code **)(DAT_140015968 + 0x850))(DAT_140015990,param_2,&local_78);
  plVar2 = plStack_70;
  if (local_78._2_1_ < 0xcc) {
    if (local_78._2_1_ == 0xcb) {
      uVar7 = uVar11;
      if (*(int *)(lVar6 + 0x10) != 0) {
        do {
          lVar8 = *(longlong *)(lVar6 + 0x18 + uVar7 * 8);
          if (lVar8 != 0) {
            if ((*(char *)(lVar8 + 0xb4) == '\x01') &&
               (uVar13 = uVar11, 1 < *(uint *)(lVar8 + 0xb0))) {
              do {
                if (DAT_140015743 == '\0') {
                  KeAcquireInStackQueuedSpinLock(lVar8 + (uVar13 + 0x38e6) * 8,local_90);
                }
                (**(code **)(lVar8 + 0x16128))(lVar8);
                (**(code **)(lVar8 + 0x16130))(lVar8,0xffffffff,1);
                KeReleaseInStackQueuedSpinLock(local_90);
                uVar9 = (int)uVar13 + 1;
                uVar13 = (ulonglong)uVar9;
              } while (uVar9 < *(uint *)(lVar8 + 0xb0));
            }
            else {
              if (DAT_140015743 == '\0') {
                KeAcquireInStackQueuedSpinLock(lVar8 + 0x1c730,local_90);
              }
              (**(code **)(lVar8 + 0x16128))(lVar8,0xffffffff);
              (**(code **)(lVar8 + 0x16130))(lVar8,0xffffffff,1);
              KeReleaseInStackQueuedSpinLock(local_90);
            }
            (**(code **)(lVar8 + 0x16138))(lVar8);
          }
          uVar9 = (int)uVar7 + 1;
          uVar7 = (ulonglong)uVar9;
        } while (uVar9 < *(uint *)(lVar6 + 0x10));
      }
    }
    else {
      if (local_78._2_1_ == 100) {
        (**(code **)(DAT_140015968 + 0x8c8))(DAT_140015990,param_2,*(undefined8 *)(lVar6 + 8));
        return;
      }
      if (local_78._2_1_ == 0x65) {
        lVar8 = *plStack_70;
        if (*(ulonglong **)(lVar8 + 0x38) == (ulonglong *)0x0) {
          uVar11 = (ulonglong)*(uint *)(lVar8 + 0xb0);
        }
        else {
          uVar11 = **(ulonglong **)(lVar8 + 0x38);
        }
        lVar6 = *(longlong *)(lVar6 + 0x18 + uVar11 * 8);
        if ((lVar6 != 0) && (*(char *)(lVar6 + 0x1605c) == '\0')) {
          (**(code **)(lVar6 + 0x16108))(lVar6,lVar8,plStack_70[1],param_2);
          return;
        }
        *(uint *)(lVar8 + 0x154) = *(uint *)(lVar8 + 0x154) | 0x20000;
        *(undefined1 *)(lVar8 + 0x110) = 2;
        *(undefined2 *)(lVar8 + 0x10e) = 0;
      }
      else if (local_78._2_1_ == 0x6c) {
        lVar6 = *(longlong *)(lVar6 + 0x18 + *plStack_70 * 8);
        if (lVar6 != 0) {
          (**(code **)(lVar6 + 0x16110))(lVar6,plStack_70,local_68,param_2);
          return;
        }
      }
      else if (local_78._2_1_ == 0x6d) {
        lVar6 = *(longlong *)(lVar6 + 0x18 + *(longlong *)*plStack_70 * 8);
        if (lVar6 != 0) {
          (**(code **)(lVar6 + 0x16158))(lVar6,(longlong *)*plStack_70,(int)plStack_70[1]);
        }
      }
      else {
        if (local_78._2_1_ == 0x6e) {
          FUN_14000a6dc(lVar6,param_2,plStack_70);
          return;
        }
        if (local_78._2_1_ == 0x6f) {
          _DAT_140015c90 = plStack_70[1];
          *plStack_70 = (longlong)FUN_14000a808;
        }
        else {
          if (local_78._2_1_ == 0x70) {
            lVar6 = *(longlong *)(lVar6 + 0x18 + (local_68 & 0xffffffff) * 8);
            if (lVar6 == 0) {
              return;
            }
            if (*(code **)(lVar6 + 0x16170) == (code *)0x0) {
              return;
            }
            (**(code **)(lVar6 + 0x16170))(lVar6,plStack_70,local_58 & 0xffffffff);
            return;
          }
          if (local_78._2_1_ == 200) {
            lVar6 = *(longlong *)(lVar6 + 0x18);
            if (*(longlong *)(lVar6 + 0x16030) == 0) {
              plStack_70[1] = 0;
              *plStack_70 = 0;
            }
            else {
              plStack_70[1] = 0x4400000;
              lVar8 = (**(code **)(DAT_140015968 + 0xb8))
                                (DAT_140015990,*(undefined8 *)(lVar6 + 0x16030));
              *plVar2 = lVar8;
              uVar11 = (**(code **)(DAT_140015968 + 0xb0))
                                 (DAT_140015990,*(undefined8 *)(lVar6 + 0x16030));
            }
            plVar2[2] = uVar11;
          }
          else if (local_78._2_1_ == 0xc9) {
            lVar8 = *(longlong *)(lVar6 + 0x18 + (ulonglong)DAT_1402180e0 * 8);
            DAT_1402175a8 = lVar6;
            if (*(longlong *)(lVar8 + 0x16038) == 0) {
              *(undefined8 *)(lVar8 + 0x16048) = 0;
              *(undefined8 *)(lVar8 + 0x16040) = 0;
              uVar7 = uVar11;
            }
            else {
              *(undefined8 *)(lVar8 + 0x16048) = 0xff00;
              uVar5 = (**(code **)(DAT_140015968 + 0xb8))(DAT_140015990);
              *(undefined8 *)(lVar8 + 0x16040) = uVar5;
              uVar7 = (**(code **)(DAT_140015968 + 0xb0))
                                (DAT_140015990,*(undefined8 *)(lVar8 + 0x16038));
            }
            *(ulonglong *)(lVar8 + 0x16050) = uVar7;
            *plStack_70 = (longlong)FUN_14000986c;
            plStack_70[1] = *(longlong *)(lVar8 + 0x16050);
            *(int *)(plStack_70 + 2) = *(int *)(lVar6 + 0x10);
            if (*(int *)(lVar6 + 0x10) != 0) {
              do {
                lVar8 = *(longlong *)(lVar6 + 0x18 + uVar11 * 8);
                if (lVar8 != 0) {
                  *(undefined8 *)((longlong)plStack_70 + uVar11 * 8 + 0x14) =
                       *(undefined8 *)(lVar8 + 0x1c320);
                  *(undefined4 *)((longlong)plStack_70 + uVar11 * 4 + 0xa4) =
                       *(undefined4 *)(*(longlong *)(lVar6 + 0x18 + uVar11 * 8) + 0x1c328);
                }
                uVar9 = (int)uVar11 + 1;
                uVar11 = (ulonglong)uVar9;
              } while (uVar9 < *(uint *)(lVar6 + 0x10));
            }
            if (DAT_1402175a8 == 0) {
              DAT_1402175a8 = 0;
            }
            else {
              DAT_1402175a0 = *(uint *)(DAT_1402175a8 + 0x10);
              if (DAT_1402175a0 != 0) {
                puVar10 = &DAT_140015ca0;
                uVar11 = (ulonglong)DAT_1402175a0;
                puVar12 = (undefined8 *)(DAT_1402175a8 + 0x18);
                do {
                  FUN_140011a40(puVar10,*puVar12,0x1c880);
                  puVar10 = puVar10 + 0x1c880;
                  puVar12 = puVar12 + 1;
                  uVar11 = uVar11 - 1;
                } while (uVar11 != 0);
              }
            }
          }
        }
      }
    }
  }
  else if (local_78._2_1_ == 0xcc) {
    cpuid(0x80000001);
    *(int *)plStack_70 = 0x2a110000;
  }
  else if (local_78._2_1_ == 0xcd) {
    if (DAT_140015741 == '\0') {
      FUN_14000aa5c(uVar5,(int)*plStack_70 + -1,(int *)((longlong)plStack_70 + 4));
    }
  }
  else if (local_78._2_1_ == 0xce) {
    iVar4 = (**(code **)(DAT_140015968 + 0x4f0))(DAT_140015990,*(undefined8 *)(lVar6 + 8),&local_98)
    ;
    if (-1 < iVar4) {
      (**(code **)(DAT_140015968 + 0x838))(DAT_140015990,local_98,0xc0000240);
    }
    if (*(int *)(lVar6 + 0x10) != 0) {
      do {
        lVar8 = *(longlong *)(lVar6 + 0x18 + uVar11 * 8);
        if (lVar8 != 0) {
          (**(code **)(lVar8 + 0x16168))(lVar8);
        }
        uVar9 = (int)uVar11 + 1;
        uVar11 = (ulonglong)uVar9;
      } while (uVar9 < *(uint *)(lVar6 + 0x10));
    }
  }
  else if (local_78._2_1_ == 0xd0) {
    if (*(int *)(lVar6 + 0x10) != 0) {
      do {
        lVar8 = *(longlong *)(lVar6 + 0x18 + uVar11 * 8);
        if (lVar8 != 0) {
          local_a0 = "V:\\RC-933\\RC_933_00291\\fulcrum\\rc\\platforms\\rcbottom\\rcbottom\\Queue.c"
          ;
          local_a8 = (char *)CONCAT44(local_a8._4_4_,0x400);
          (**(code **)(DAT_140015968 + 0xdb0))(DAT_140015990,*(undefined8 *)(lVar8 + 0x20),1,0);
        }
        uVar9 = (int)uVar11 + 1;
        uVar11 = (ulonglong)uVar9;
      } while (uVar9 < *(uint *)(lVar6 + 0x10));
    }
  }
  else if (local_78._2_1_ == 0xd1) {
    if (*(int *)(lVar6 + 0x10) != 0) {
      do {
        lVar8 = *(longlong *)(lVar6 + 0x18 + uVar11 * 8);
        if (lVar8 != 0) {
          local_a8 = "V:\\RC-933\\RC_933_00291\\fulcrum\\rc\\platforms\\rcbottom\\rcbottom\\Queue.c"
          ;
          (**(code **)(DAT_140015968 + 0xdb8))(DAT_140015990,*(undefined8 *)(lVar8 + 0x20),0,0x40f);
        }
        uVar9 = (int)uVar11 + 1;
        uVar11 = (ulonglong)uVar9;
      } while (uVar9 < *(uint *)(lVar6 + 0x10));
    }
  }
  else if (local_78._2_1_ == 0xd2) {
    DAT_140015740 = 1;
    if (*(int *)(lVar6 + 0x10) != 0) {
      do {
        lVar8 = *(longlong *)(lVar6 + 0x18 + uVar11 * 8);
        if (lVar8 != 0) {
          *(undefined1 *)(lVar8 + 0x1c840) = 1;
        }
        uVar9 = (int)uVar11 + 1;
        uVar11 = (ulonglong)uVar9;
      } while (uVar9 < *(uint *)(lVar6 + 0x10));
    }
  }
  else if (local_78._2_1_ == 0xd3) {
    DAT_140015740 = 0;
    if (*(int *)(lVar6 + 0x10) != 0) {
      do {
        lVar8 = *(longlong *)(lVar6 + 0x18 + uVar11 * 8);
        if (lVar8 != 0) {
          lVar1 = *(longlong *)(lVar8 + 0x1c848);
          *(undefined1 *)(lVar8 + 0x1c840) = 0;
          if (lVar1 != 0) {
            *(char *)(lVar1 + 0x43) = *(char *)(lVar1 + 0x43) + '\x01';
            *(longlong *)(lVar1 + 0xb8) = *(longlong *)(lVar1 + 0xb8) + 0x48;
            (**(code **)(DAT_140015968 + 0x110))
                      (DAT_140015990,*(undefined8 *)(lVar8 + 0x20),*(undefined8 *)(lVar8 + 0x1c848))
            ;
            *(undefined8 *)(lVar8 + 0x1c848) = 0;
          }
        }
        uVar9 = (int)uVar11 + 1;
        uVar11 = (ulonglong)uVar9;
      } while (uVar9 < *(uint *)(lVar6 + 0x10));
    }
  }
  else if (local_78._2_1_ == 0xd4) {
    uVar3 = FUN_14000a888();
    *(undefined1 *)plVar2 = uVar3;
  }
  (**(code **)(DAT_140015968 + 0x838))(DAT_140015990,param_2,0);
  return;
}

