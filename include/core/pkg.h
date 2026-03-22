#pragma once

/* Package manager.
 *
 * Repository: a plain-text file at PKG_SERVER/PKG_INDEX_PATH.
 * Each non-empty line: <name> <version> <path>
 *   e.g.  hello  1.0  /pkg/hello.elf
 *
 * To host packages on your Windows machine:
 *   cd C:\packages && python3 -m http.server 8080
 * Then the VM reaches it at 10.0.2.2:8080 via VirtualBox NAT.
 */

#define PKG_SERVER      "10.0.2.2"
#define PKG_PORT        8080
#define PKG_INDEX_PATH  "/pkg/index.txt"
#define PKG_INSTALL_DIR "/bin"

void cmd_pkg(int argc, char **argv);
