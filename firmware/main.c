/*
 * Boot switch firmware for a SparkFun-style Pro Micro (ATmega32U4, 5 V, 16 MHz).
 *
 * The board appears as two USB devices:
 *   - A read-only disk whose root folder holds LINUX.SW, WINDOWS.SW or MENU.SW,
 *     following the three-position switch. GRUB checks this at boot.
 *   - A serial port that reports the switches as text lines:
 *       BOOT=LINUX | BOOT=WINDOWS | BOOT=MENU
 *       BT=ON | BT=OFF
 *     Lines are sent when the port is opened, when a switch changes, and in reply to "?".
 *
 * Wiring (every switch common goes to GND; the chip's pull-ups are used):
 *   pin 4 (PD4)  boot switch, Linux side
 *   pin 5 (PC6)  boot switch, Windows side
 *   pin 6 (PD7)  Bluetooth switch. Open = ON, closed to GND = OFF, so an
 *                unconnected pin leaves Bluetooth on.
 *
 * Opening the serial port at 1200 baud and closing it restarts the board into its
 * bootloader for reflashing, the same as an Arduino.
 *
 * Based on the LUFA VirtualSerialMassStorage demo (Copyright 2021 Dean Camera, MIT-style licence).
 */
#include <avr/io.h>
#include <avr/wdt.h>
#include <avr/power.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdio.h>
#include <LUFA/Drivers/USB/USB.h>

#include "Descriptors.h"
#include "scsi.h"
#include "virtualdisk.h"

#define DEBOUNCE_MS   30

/* Pro Micro LEDs, both active low: RX on PB0, TX on PD5. */
#define LED_RX_ON()   (PORTB &= ~(1 << PB0))
#define LED_RX_OFF()  (PORTB |=  (1 << PB0))
#define LED_TX_ON()   (PORTD &= ~(1 << PD5))
#define LED_TX_OFF()  (PORTD |=  (1 << PD5))

USB_ClassInfo_CDC_Device_t VirtualSerial_CDC_Interface =
	{
		.Config =
			{
				.ControlInterfaceNumber = INTERFACE_ID_CDC_CCI,
				.DataINEndpoint         = { .Address = CDC_TX_EPADDR, .Size = CDC_TXRX_EPSIZE, .Banks = 1 },
				.DataOUTEndpoint        = { .Address = CDC_RX_EPADDR, .Size = CDC_TXRX_EPSIZE, .Banks = 1 },
				.NotificationEndpoint   = { .Address = CDC_NOTIFICATION_EPADDR, .Size = CDC_NOTIFICATION_EPSIZE, .Banks = 1 },
			},
	};

USB_ClassInfo_MS_Device_t Disk_MS_Interface =
	{
		.Config =
			{
				.InterfaceNumber = INTERFACE_ID_MassStorage,
				.DataINEndpoint  = { .Address = MASS_STORAGE_IN_EPADDR,  .Size = MASS_STORAGE_IO_EPSIZE, .Banks = 1 },
				.DataOUTEndpoint = { .Address = MASS_STORAGE_OUT_EPADDR, .Size = MASS_STORAGE_IO_EPSIZE, .Banks = 1 },
				.TotalLUNs       = TOTAL_LUNS,
			},
	};

static volatile bool HostListening;
static volatile bool JumpToBootloaderRequested;

/* Reads the boot switch directly, with no debounce. A boot only reads a settled switch. */
uint8_t BootSwitch_ReadChoice(void)
{
	bool LinuxSide   = !(PIND & (1 << PD4));
	bool WindowsSide = !(PINC & (1 << PC6));

	if (LinuxSide && !WindowsSide)
	  return BOOT_LINUX;
	if (WindowsSide && !LinuxSide)
	  return BOOT_WINDOWS;

	/* Middle position, or both sides grounded by a wiring fault. */
	return BOOT_MENU;
}

static bool Bluetooth_ReadOn(void)
{
	return (PIND & (1 << PD7)) != 0;
}

static void SetupHardware(void)
{
	MCUSR &= ~(1 << WDRF);
	wdt_disable();
	clock_prescale_set(clock_div_1);

	/* Switch inputs with pull-ups. */
	DDRD  &= ~((1 << PD4) | (1 << PD7));
	PORTD |=   (1 << PD4) | (1 << PD7);
	DDRC  &= ~(1 << PC6);
	PORTC |=  (1 << PC6);

	/* LEDs off. */
	DDRB |= (1 << PB0);
	DDRD |= (1 << PD5);
	LED_RX_OFF();
	LED_TX_OFF();

	USB_Init();
}

static void SendLine(const char* Line)
{
	if (HostListening)
	  CDC_Device_SendString(&VirtualSerial_CDC_Interface, Line);
}

static void ReportBoot(uint8_t Choice)
{
	static const char* const Lines[] =
	{
		[BOOT_MENU]    = "BOOT=MENU\r\n",
		[BOOT_LINUX]   = "BOOT=LINUX\r\n",
		[BOOT_WINDOWS] = "BOOT=WINDOWS\r\n",
	};

	SendLine(Lines[Choice]);
}

static void ReportBluetooth(bool On)
{
	SendLine(On ? "BT=ON\r\n" : "BT=OFF\r\n");
}

/* Restarts into the Caterina bootloader, which stays active for about 8 seconds. */
static void JumpToBootloader(void)
{
	USB_Disable();
	cli();
	_delay_ms(50);

	/* Older Caterina builds look for the key at 0x0800, newer ones at the top of RAM. */
	*(volatile uint16_t*)0x0800 = 0x7777;
	*(volatile uint16_t*)(RAMEND - 1) = 0x7777;

	wdt_enable(WDTO_120MS);
	for (;;);
}

/* Tracks a switch input and reports it once it has been stable for DEBOUNCE_MS. */
typedef struct
{
	uint8_t Reported;
	uint8_t Candidate;
	uint8_t StableMs;
} Debounce_t;

static bool Debounce_Update(Debounce_t* d, uint8_t Value)
{
	if (Value != d->Candidate)
	{
		d->Candidate = Value;
		d->StableMs  = 0;
		return false;
	}

	if (d->StableMs < DEBOUNCE_MS)
	{
		d->StableMs++;
		return false;
	}

	if (Value != d->Reported)
	{
		d->Reported = Value;
		return true;
	}

	return false;
}

int main(void)
{
	Debounce_t BootState;
	Debounce_t BluetoothState;
	bool       WasListening = false;

	SetupHardware();

	BootState.Reported      = BootState.Candidate      = BootSwitch_ReadChoice();
	BluetoothState.Reported = BluetoothState.Candidate = Bluetooth_ReadOn();
	BootState.StableMs      = BluetoothState.StableMs  = 0;

	GlobalInterruptEnable();

	for (;;)
	{
		if (JumpToBootloaderRequested)
		  JumpToBootloader();

		/* Report everything when a program opens the port. */
		if (HostListening && !WasListening)
		{
			ReportBoot(BootState.Reported);
			ReportBluetooth(BluetoothState.Reported);
		}
		WasListening = HostListening;

		if (Debounce_Update(&BootState, BootSwitch_ReadChoice()))
		  ReportBoot(BootState.Reported);

		if (Debounce_Update(&BluetoothState, Bluetooth_ReadOn()))
		  ReportBluetooth(BluetoothState.Reported);

		int16_t Received = CDC_Device_ReceiveByte(&VirtualSerial_CDC_Interface);
		if (Received == '?')
		{
			ReportBoot(BootState.Reported);
			ReportBluetooth(BluetoothState.Reported);
		}

		CDC_Device_USBTask(&VirtualSerial_CDC_Interface);
		MS_Device_USBTask(&Disk_MS_Interface);
		USB_USBTask();

		_delay_ms(1);
	}
}

void EVENT_USB_Device_Connect(void)
{
	LED_RX_OFF();
}

void EVENT_USB_Device_Disconnect(void)
{
	HostListening = false;
	LED_RX_OFF();
}

void EVENT_USB_Device_ConfigurationChanged(void)
{
	bool ConfigSuccess = true;

	ConfigSuccess &= CDC_Device_ConfigureEndpoints(&VirtualSerial_CDC_Interface);
	ConfigSuccess &= MS_Device_ConfigureEndpoints(&Disk_MS_Interface);

	/* RX LED on means the PC has set up the board. */
	if (ConfigSuccess)
	  LED_RX_ON();
}

void EVENT_USB_Device_ControlRequest(void)
{
	CDC_Device_ProcessControlRequest(&VirtualSerial_CDC_Interface);
	MS_Device_ProcessControlRequest(&Disk_MS_Interface);
}

void EVENT_CDC_Device_ControLineStateChanged(USB_ClassInfo_CDC_Device_t* const CDCInterfaceInfo)
{
	bool DTR = (CDCInterfaceInfo->State.ControlLineStates.HostToDevice & CDC_CONTROL_LINE_OUT_DTR) != 0;

	HostListening = DTR;

	/* Arduino convention: closing the port after opening it at 1200 baud means "reflash". */
	if (!DTR && (CDCInterfaceInfo->State.LineEncoding.BaudRateBPS == 1200))
	  JumpToBootloaderRequested = true;
}

bool CALLBACK_MS_Device_SCSICommandReceived(USB_ClassInfo_MS_Device_t* const MSInterfaceInfo)
{
	bool CommandSuccess;

	LED_TX_ON();
	CommandSuccess = SCSI_DecodeSCSICommand(MSInterfaceInfo);
	LED_TX_OFF();

	return CommandSuccess;
}
