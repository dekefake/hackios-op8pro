/**
  The boot menu of the HackiOS port. See HackiosMenu.h.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextIn.h>

#include "HackiosMenu.h"
#include "HackiosFont.h"

#define GLYPH_W      HACKIOS_FONT_W      // 24 x 48 pixels: three times the firmware's 8 x 19 console font, and more
#define GLYPH_H      HACKIOS_FONT_H
#define MENU_MAX     24
#define MENU_CHARS   64

//
// The variable the XNU loader reads (xnuloader/loader.c, hackios_boot_mode()).
//
STATIC EFI_GUID  mHackiosGuid = { 0x8d3c2a41, 0x5b7e, 0x4d09, { 0xa1, 0x6f, 0x0c, 0x12, 0x7e, 0x44, 0x91, 0x2b } };

typedef struct {
  CHAR8  Text[MENU_CHARS];
  INTN   Option;           // >= 0: the platform's boot option with this index; -1: one of ours
  UINT8  Mode;             // for ours: the value of HackiosBootMode
} MENU_ITEM;

STATIC MENU_ITEM  mItems[MENU_MAX];
STATIC UINTN      mItemCount;
STATIC UINTN      mBlockChars;

STATIC
VOID
AddItem (
  IN CONST CHAR8 *Text,
  IN INTN         Option,
  IN UINT8        Mode
  )
{
  if (mItemCount >= MENU_MAX) {
    return;
  }

  AsciiStrnCpyS (mItems[mItemCount].Text, MENU_CHARS, Text, MENU_CHARS - 1);
  mItems[mItemCount].Option = Option;
  mItems[mItemCount].Mode   = Mode;
  mItemCount++;
}

//
// One line of text, from the menu's own font (HackiosFont.h) into a buffer and onto the screen at (X, Y). The line is padded with spaces to the width of the block, so that the highlight
// is a bar of one width. (The firmware's AsciiPrintXY, read back and magnified, came out as garbage on the panel.)
//
STATIC
VOID
DrawLine (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  IN CONST CHAR8                  *Text,
  IN BOOLEAN                       Selected,
  IN UINTN                         X,
  IN UINTN                         Y
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Fg, Bg, *Buffer;
  UINTN                          Width  = mBlockChars * GLYPH_W;
  UINTN                          Length = AsciiStrLen (Text);

  Fg.Red = Fg.Green = Fg.Blue = Selected ? 0x00 : 0xff;
  Bg.Red = Bg.Green = Bg.Blue = Selected ? 0xff : 0x00;
  Fg.Reserved = Bg.Reserved = 0;

  Buffer = AllocatePool (Width * GLYPH_H * sizeof (*Buffer));
  if (Buffer == NULL) {
    return;
  }

  for (UINTN Cell = 0; Cell < mBlockChars; Cell++) {
    UINT8         Char  = (Cell >= 1 && Cell - 1 < Length) ? (UINT8)Text[Cell - 1] : ' ';
    CONST UINT8  *Glyph = mHackiosFont[(Char >= 32 && Char < 127) ? Char - 32 : '?' - 32];

    for (UINTN Row = 0; Row < GLYPH_H; Row++) {
      for (UINTN Column = 0; Column < GLYPH_W; Column++) {
        UINT8  Bits = Glyph[Row * ((GLYPH_W + 7) / 8) + Column / 8];

        Buffer[Row * Width + Cell * GLYPH_W + Column] = ((Bits >> (7 - Column % 8)) & 1) ? Fg : Bg;
      }
    }
  }

  Gop->Blt (Gop, Buffer, EfiBltBufferToVideo, 0, 0, X, Y, Width, GLYPH_H, 0);
  FreePool (Buffer);
}

STATIC
VOID
DrawMenu (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  IN UINTN                         Selected
  )
{
  UINT32                         ScreenWidth  = Gop->Mode->Info->HorizontalResolution;
  UINT32                         ScreenHeight = Gop->Mode->Info->VerticalResolution;
  UINTN                          LineHeight   = GLYPH_H + 14;
  UINTN                          Rows         = mItemCount + 3;        // the title, a blank line, the items, the hint
  UINTN                          BlockWidth   = mBlockChars * GLYPH_W;
  UINTN                          X            = (ScreenWidth > BlockWidth) ? (ScreenWidth - BlockWidth) / 2 : 0;
  UINTN                          Y            = (ScreenHeight > Rows * LineHeight) ? (ScreenHeight - Rows * LineHeight) / 2 : 0;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black        = { 0, 0, 0, 0 };

  Gop->Blt (Gop, &Black, EfiBltVideoFill, 0, 0, 0, 0, ScreenWidth, ScreenHeight, 0);

  DrawLine (Gop, "HACKiOS boot menu", FALSE, X, Y);
  for (UINTN Index = 0; Index < mItemCount; Index++) {
    DrawLine (Gop, mItems[Index].Text, Index == Selected, X, Y + (Index + 2) * LineHeight);
  }
  DrawLine (Gop, "Vol: move   Power: select", FALSE, X, Y + (mItemCount + 3) * LineHeight);

}

EFI_STATUS
HackiosBootMenu (
  VOID
  )
{
  EFI_STATUS                     Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;
  UINTN                          OptionCount;
  UINTN                          Selected = 0;
  EFI_INPUT_KEY                  Key;
  UINTN                          EventIndex;

  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  }

  if (EFI_ERROR (Status) || Gop == NULL || gST->ConIn == NULL) {
    return EFI_UNSUPPORTED;
  }

  //
  // Ours first, then every visible boot option the platform registered (Internal Storage, USB Storage, Mass Storage, Power Off, Exit ...)
  //
  mItemCount = 0;
  AddItem ("iOS (USB debug, default)",    -1, 1);
  AddItem ("iOS (no USB debug)",          -1, 0);
  //
  // No "Reboot to Android (other slot)" (mode 2) any more: its A/B exchange changed the attribute bytes of boot_a/boot_b and not the type GUIDs
  // that ABL's own `fastboot --set-active` moves with them, and left the phone refusing to boot until it was repaired over EDL (2026-10-28).
  // Slot changes are made with `fastboot --set-active=a|b`.
  //
  AddItem ("Reboot to bootloader",        -1, 3);
  AddItem ("Reboot to recovery",          -1, 4);

  Options = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);
  for (UINTN Index = 0; Options != NULL && Index < OptionCount; Index++) {
    CHAR8  Text[MENU_CHARS];

    if ((Options[Index].Attributes & LOAD_OPTION_ACTIVE) == 0 || (Options[Index].Attributes & LOAD_OPTION_HIDDEN) != 0 ||
        Options[Index].Description == NULL) {
      continue;
    }

    UnicodeStrToAsciiStrS (Options[Index].Description, Text, sizeof (Text));
    AddItem (Text, (INTN)Index, 0);
  }

  mBlockChars = 0;
  for (UINTN Index = 0; Index < mItemCount; Index++) {
    mBlockChars = MAX (mBlockChars, AsciiStrLen (mItems[Index].Text));
  }

  mBlockChars = MAX (mBlockChars, 26) + 2;
  mBlockChars = MIN (mBlockChars, (UINTN)(Gop->Mode->Info->HorizontalResolution / GLYPH_W));

  // keys that are already waiting (the one that opened the menu) are not a choice
  while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
  }

  for ( ; ; ) {
    DrawMenu (Gop, Selected);

    gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &EventIndex);
    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      continue;
    }

    if (Key.ScanCode == SCAN_UP) {
      Selected = (Selected + mItemCount - 1) % mItemCount;
      continue;
    }

    if (Key.ScanCode == SCAN_DOWN) {
      Selected = (Selected + 1) % mItemCount;
      continue;
    }

    if (mItems[Selected].Option < 0) {
      UINT8  Mode = mItems[Selected].Mode;

      gRT->SetVariable (L"HackiosBootMode", &mHackiosGuid, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof (Mode), &Mode);
      break;
    }

    // one of the platform's: boot it as the boot manager would (when it comes back, the menu again)
    {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black = { 0, 0, 0, 0 };

      Gop->Blt (Gop, &Black, EfiBltVideoFill, 0, 0, 0, 0, Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution, 0);
      EfiBootManagerBoot (&Options[mItems[Selected].Option]);
    }
  }

  {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black = { 0, 0, 0, 0 };

    Gop->Blt (Gop, &Black, EfiBltVideoFill, 0, 0, 0, 0, Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution, 0);
  }

  if (Options != NULL) {
    EfiBootManagerFreeLoadOptions (Options, OptionCount);
  }

  return EFI_SUCCESS;
}
