#ifndef _HACKIOS_MENU_H_
#define _HACKIOS_MENU_H_

/**
  The boot menu of the HackiOS port (volume up while the phone starts): iOS, iOS with the USB link, Android of the other A/B slot,
  the stock bootloader, recovery -- and below them the platform's own entries (Internal Storage, USB Storage, ...). Drawn 3 times
  the size of the firmware's console font, centred. The choice among ours is left in the volatile variable HackiosBootMode for
  the XNU loader (xnuloader/loader.c, hackios_boot_mode()); one of the others is booted as the boot manager would.

  @retval EFI_SUCCESS     the boot goes on (to the XNU loader)
  @retval other           no menu could be shown: the caller starts the firmware's own boot manager menu
**/
EFI_STATUS
HackiosBootMenu (
  VOID
  );

#endif /* _HACKIOS_MENU_H_ */
