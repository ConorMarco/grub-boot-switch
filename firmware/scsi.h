#ifndef SCSI_H
#define SCSI_H

#include <stdbool.h>
#include <LUFA/Drivers/USB/USB.h>

/* Handles one SCSI command from the host. Returns true if it succeeded. */
bool SCSI_DecodeSCSICommand(USB_ClassInfo_MS_Device_t* const MSInterfaceInfo);

/* Implemented in main.c: the switch position used for the root folder. */
uint8_t BootSwitch_ReadChoice(void);

#endif
