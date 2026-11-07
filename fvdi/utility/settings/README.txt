fVDI settings
=============

FVDISET.PRG is a standalone GEM application, compiled for the 68000. It does
not require a CPX host, an RSC file, MiNT, ARAnyM, or a particular AES. Copy
it to the guest's TOOLS folder or another convenient directory.

It opens FVDI.SYS in the current directory or on the boot drive. The dialog
shows the file it loaded. Use Load to select another file, including one
on another guest drive. Changes apply after restarting the guest; they do
not change a running VDI/AES installation.

Font folder changes the first configured fonts folder. Choose a font file
inside the desired folder. Additional fonts lines remain intact. Fonts
must follow the screen-driver line, as fVDI requires.

Smooth outline text enables antialiasing in a FreeType build. The updated
engine composites on supported packed 16/32-bit surfaces. Palette screens
and unrecognized component layouts use the normal monochrome renderer.

NVDI cookie controls the compatibility cookie. Enabling it adds $0501.
Existing cookie values are retained until that setting is changed. This
does not make fVDI a complete NVDI implementation; the implemented calls
are part of the engine, not independently switchable options.

Display settings follow the configured driver:

  ARANYM.SYS: resolution, depth (8/16/32), and refresh in its mode option.
  SAGA.SYS: selected supported 60 Hz modes, 16 bits only.
  16_BIT.SYS and BITPLANE.SYS on Atari hardware: show the current desktop
      mode. Select the boot video mode through the system's normal tools.
  Shinogi: on a compatible virtual-machine setup, Set up Shinogi display
      enables its launcher resolution controls. Existing SHINOGI.INI
      settings are preserved. Depth remains 16 bits.

The application does not replace the screen driver, change the machine's
boot resolution directly, or offer unsupported hardware mode combinations.
SAGA policy follows the driver tables; physical hardware remains untested.

Saving prepares and verifies a temporary file beside the original, checks
that the original has not changed since loading, then keeps it as an unused
.B00 through .B99 backup. Existing backups are never replaced. Failure to
prepare a file or reserve a backup leaves the original untouched. A failed
final replacement restores the original when possible and reports the
backup location otherwise. The final file is reopened and verified.
Keep backups until you have booted the new configuration successfully.

Unknown settings, additional font folders, driver options and comments are
preserved. Files over 64 KB, lines over 1023 bytes, and unsupported config
syntax are refused rather than truncated. The GEM selector uses traditional
128-byte paths. Multiple 01 screen-driver definitions require manual editing.

Shinogi's launcher file is saved separately from FVDI.SYS. If that second
save fails, the app reports which file was saved and retains the pending
resolution change for retrying.

Build: make -C fvdi/utility/settings
License: the fVDI GPL. Packages include the GNU GPL text.
