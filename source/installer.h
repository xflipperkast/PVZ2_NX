#ifndef PVZ2_INSTALLER_H
#define PVZ2_INSTALLER_H

/* Installs the files needed by the port from DATA_DIR "/pvz2.xapk" when the
 * OBB or required ARM64 libraries are absent. Returns 0 on success. */
int installer_prepare_game_files(void);
const char *installer_last_error(void);

#endif
