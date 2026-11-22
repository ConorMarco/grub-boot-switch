/*
 * SCSI commands for the read-only virtual disk.
 * Based on the LUFA VirtualSerialMassStorage demo (Copyright 2021 Dean Camera, MIT-style licence).
 */
#include "scsi.h"
#include "virtualdisk.h"

#define SCSI_CMD_READ_FORMAT_CAPACITIES   0x23
#define SCSI_CMD_SYNCHRONIZE_CACHE_10     0x35

static const SCSI_Inquiry_Response_t InquiryData =
	{
		.DeviceType          = 0x00,   /* direct-access block device */
		.PeripheralQualifier = 0,
		.Removable           = true,
		.Version             = 0,
		.ResponseDataFormat  = 2,
		.AdditionalLength    = 0x1F,
		.VendorID            = {'c','m','a','r','c','o',' ',' '},
		.ProductID           = {'B','o','o','t',' ','S','w','i','t','c','h',' ',' ',' ',' ',' '},
		.RevisionID          = {'1','.','0','0'},
	};

static SCSI_Request_Sense_Response_t SenseData =
	{
		.ResponseCode        = 0x70,
		.AdditionalLength    = 0x0A,
	};

static uint8_t BlockBuffer[VDISK_BLOCK_SIZE];

static void SetSense(uint8_t key, uint8_t asc)
{
	SenseData.SenseKey                 = key;
	SenseData.AdditionalSenseCode      = asc;
	SenseData.AdditionalSenseQualifier = 0;
}

static uint16_t ReadBE16(const uint8_t* p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t ReadBE32(const uint8_t* p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Sends up to AllocationLength bytes of Data, padding with zeros, and records the bytes sent. */
static void SendResponse(USB_ClassInfo_MS_Device_t* const MSInterfaceInfo,
                         const void* Data, uint16_t DataLength, uint16_t AllocationLength)
{
	uint16_t Sent = MIN(DataLength, AllocationLength);

	Endpoint_Write_Stream_LE(Data, Sent, NULL);
	Endpoint_ClearIN();
	MSInterfaceInfo->State.CommandBlock.DataTransferLength -= Sent;
}

static bool Command_Inquiry(USB_ClassInfo_MS_Device_t* const MSInterfaceInfo)
{
	const uint8_t* cdb = MSInterfaceInfo->State.CommandBlock.SCSICommandData;

	/* Only the standard INQUIRY page is supported. */
	if ((cdb[1] & 0x03) || cdb[2])
	{
		SetSense(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_ASENSE_INVALID_FIELD_IN_CDB);
		return false;
	}

	SendResponse(MSInterfaceInfo, &InquiryData, sizeof(InquiryData), ReadBE16(&cdb[3]));
	return true;
}

static bool Command_RequestSense(USB_ClassInfo_MS_Device_t* const MSInterfaceInfo)
{
	const uint8_t* cdb = MSInterfaceInfo->State.CommandBlock.SCSICommandData;

	SendResponse(MSInterfaceInfo, &SenseData, sizeof(SenseData), cdb[4]);
	return true;
}

static bool Command_ReadCapacity10(USB_ClassInfo_MS_Device_t* const MSInterfaceInfo)
{
	uint32_t LastBlock = VDISK_BLOCKS - 1;
	uint32_t BlockSize = VDISK_BLOCK_SIZE;

	Endpoint_Write_Stream_BE(&LastBlock, sizeof(LastBlock), NULL);
	Endpoint_Write_Stream_BE(&BlockSize, sizeof(BlockSize), NULL);
	Endpoint_ClearIN();
	MSInterfaceInfo->State.CommandBlock.DataTransferLength -= 8;
	return true;
}

static bool Command_ReadFormatCapacities(USB_ClassInfo_MS_Device_t* const MSInterfaceInfo)
{
	const uint8_t* cdb = MSInterfaceInfo->State.CommandBlock.SCSICommandData;
	const uint8_t Response[12] =
		{
			0x00, 0x00, 0x00, 0x08,                       /* capacity list length */
			(uint8_t)((uint32_t)VDISK_BLOCKS >> 24), (uint8_t)((uint32_t)VDISK_BLOCKS >> 16),
			(uint8_t)(VDISK_BLOCKS >> 8),  (uint8_t)VDISK_BLOCKS,
			0x02,                                         /* formatted media */
			0x00, (uint8_t)(VDISK_BLOCK_SIZE >> 8), (uint8_t)VDISK_BLOCK_SIZE,
		};

	SendResponse(MSInterfaceInfo, Response, sizeof(Response), ReadBE16(&cdb[7]));
	return true;
}

/* Mode parameter headers with no pages; the write-protect bit is always set. */
static bool Command_ModeSense6(USB_ClassInfo_MS_Device_t* const MSInterfaceInfo)
{
	const uint8_t* cdb = MSInterfaceInfo->State.CommandBlock.SCSICommandData;
	const uint8_t Header[4] = { 0x03, 0x00, 0x80, 0x00 };

	SendResponse(MSInterfaceInfo, Header, sizeof(Header), cdb[4]);
	return true;
}

static bool Command_ModeSense10(USB_ClassInfo_MS_Device_t* const MSInterfaceInfo)
{
	const uint8_t* cdb = MSInterfaceInfo->State.CommandBlock.SCSICommandData;
	const uint8_t Header[8] = { 0x00, 0x06, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00 };

	SendResponse(MSInterfaceInfo, Header, sizeof(Header), ReadBE16(&cdb[7]));
	return true;
}

static bool Command_Read10(USB_ClassInfo_MS_Device_t* const MSInterfaceInfo)
{
	const uint8_t* cdb = MSInterfaceInfo->State.CommandBlock.SCSICommandData;
	uint32_t BlockAddress = ReadBE32(&cdb[2]);
	uint16_t TotalBlocks  = ReadBE16(&cdb[7]);
	BootChoice_t Choice   = (BootChoice_t)BootSwitch_ReadChoice();

	if ((BlockAddress >= VDISK_BLOCKS) || (TotalBlocks > (VDISK_BLOCKS - BlockAddress)))
	{
		SetSense(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_ASENSE_LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE);
		return false;
	}

	Endpoint_SelectEndpoint(MSInterfaceInfo->Config.DataINEndpoint.Address);

	for (uint16_t i = 0; i < TotalBlocks; i++)
	{
		VirtualDisk_ReadBlock(BlockAddress + i, Choice, BlockBuffer);

		if (Endpoint_Write_Stream_LE(BlockBuffer, VDISK_BLOCK_SIZE, NULL) != ENDPOINT_RWSTREAM_NoError)
		  return false;

		if (MSInterfaceInfo->State.IsMassStoreReset)
		  return false;

		MSInterfaceInfo->State.CommandBlock.DataTransferLength -= VDISK_BLOCK_SIZE;
	}

	Endpoint_ClearIN();
	return true;
}

bool SCSI_DecodeSCSICommand(USB_ClassInfo_MS_Device_t* const MSInterfaceInfo)
{
	bool CommandSuccess = false;

	switch (MSInterfaceInfo->State.CommandBlock.SCSICommandData[0])
	{
		case SCSI_CMD_INQUIRY:
			CommandSuccess = Command_Inquiry(MSInterfaceInfo);
			break;
		case SCSI_CMD_REQUEST_SENSE:
			CommandSuccess = Command_RequestSense(MSInterfaceInfo);
			break;
		case SCSI_CMD_READ_CAPACITY_10:
			CommandSuccess = Command_ReadCapacity10(MSInterfaceInfo);
			break;
		case SCSI_CMD_READ_FORMAT_CAPACITIES:
			CommandSuccess = Command_ReadFormatCapacities(MSInterfaceInfo);
			break;
		case SCSI_CMD_MODE_SENSE_6:
			CommandSuccess = Command_ModeSense6(MSInterfaceInfo);
			break;
		case SCSI_CMD_MODE_SENSE_10:
			CommandSuccess = Command_ModeSense10(MSInterfaceInfo);
			break;
		case SCSI_CMD_READ_10:
			CommandSuccess = Command_Read10(MSInterfaceInfo);
			break;
		case SCSI_CMD_WRITE_10:
		case SCSI_CMD_WRITE_6:
			SetSense(SCSI_SENSE_KEY_DATA_PROTECT, SCSI_ASENSE_WRITE_PROTECTED);
			break;
		case SCSI_CMD_TEST_UNIT_READY:
		case SCSI_CMD_START_STOP_UNIT:
		case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
		case SCSI_CMD_VERIFY_10:
		case SCSI_CMD_SYNCHRONIZE_CACHE_10:
			CommandSuccess = true;
			MSInterfaceInfo->State.CommandBlock.DataTransferLength = 0;
			break;
		default:
			SetSense(SCSI_SENSE_KEY_ILLEGAL_REQUEST, SCSI_ASENSE_INVALID_COMMAND);
			break;
	}

	if (CommandSuccess)
	  SetSense(SCSI_SENSE_KEY_GOOD, SCSI_ASENSE_NO_ADDITIONAL_INFORMATION);

	return CommandSuccess;
}
