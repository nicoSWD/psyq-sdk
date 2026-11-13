# Third-party code

`bin/cpppsx` and `bin/cc1psx` are built from the PsyQ-patched GNU GCC 2.8.1 source, which is licensed under the
GNU General Public License, version 2 or (at your option) any later version.

* Source: <https://github.com/nocato/homebrew-psyq>, commit `bdee891005a0aec6bf6fe40e504cf068ebf38caf`
  (directory `gcc-2.8.1_psyq-4.4/gcc`)
* Modifications: the patches applied by `src/cpppsx/build.sh` and `src/cc1psx/build.sh`, and the compatibility
  header `src/cc1psx/psx_compat.h`, all part of this repository. The complete corresponding source of the binaries
  is therefore that upstream commit plus this repository at the same version.
* GPL-2.0 text: <https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt>

The other five tools (`aspsx`, `asmpsx`, `psylink`, `psylink_overlay_fopen_mod`, `cpe2exe`) contain no third-party
code and are covered by the MIT licence in `LICENSE`.
