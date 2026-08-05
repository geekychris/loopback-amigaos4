#ifndef VERSION_H
#define VERSION_H

/* Bumped on every meaningful behavior change. Roadshow reads this
 * via NSCMD_DEVICEQUERY.SubType so an incompatibility should force
 * the version up. */

#define DEVNAME       "loopback.device"
#define DEVMAJOR      1
#define DEVMINOR      0
#define DEVREVISION   0
#define DEVDATE       "5.8.2026"

#define DEVVERSIONSTRING "$VER: loopback.device 1.0 (" DEVDATE ")"

#endif
