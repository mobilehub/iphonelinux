#include "openiboot.h"
#include "usb.h"
#include "power.h"
#include "util.h"
#include "hardware/power.h"
#include "hardware/usb.h"
#include "timer.h"
#include "clock.h"
#include "interrupt.h"

static void change_state(USBState new_state);

static Boolean usb_inited;

static USBState usb_state;
static USBDirection endpoint_directions[USB_NUM_ENDPOINTS];
static USBEndpointBidirHandlerInfo endpoint_handlers[USB_NUM_ENDPOINTS];

volatile USBEPRegisters* InEPRegs;
volatile USBEPRegisters* OutEPRegs;

static USBDeviceDescriptor deviceDescriptor;

static uint8_t numStringDescriptors;
static USBStringDescriptor** stringDescriptors;
static USBFirstStringDescriptor* firstStringDescriptor;

static USBConfiguration* configurations;

static uint8_t* inBuffer = NULL;
static uint8_t* outBuffer = NULL;

static void usbIRQHandler(uint32_t token);

static void initializeDescriptors();

static uint8_t addConfiguration(uint8_t bConfigurationValue, uint8_t iConfiguration, uint8_t selfPowered, uint8_t remoteWakeup, uint16_t maxPower);

static void endConfiguration(USBConfiguration* configuration);

static USBInterface* addInterfaceDescriptor(USBConfiguration* configuration, uint8_t interface, uint8_t bAlternateSetting, uint8_t bInterfaceClass, uint8_t bInterfaceSubClass, uint8_t bInterfaceProtocol, uint8_t iInterface);

static uint8_t addEndpointDescriptor(USBInterface* interface, uint8_t endpoint, USBDirection direction, USBTransferType transferType, USBSynchronisationType syncType, USBUsageType usageType, uint16_t wMaxPacketSize, uint8_t bInterval);

static void releaseConfigurations();

static uint8_t addStringDescriptor(const char* descriptorString);
static void releaseStringDescriptors();
static uint16_t packetsizeFromSpeed(uint8_t speed_id);

int usb_setup() {
	int i;

	if(usb_inited) {
		return 0;
	}

	InEPRegs = (USBEPRegisters*)(USB + USB_INREGS);
	OutEPRegs = (USBEPRegisters*)(USB + USB_OUTREGS);

	change_state(USBStart);

	// Power on hardware
	power_ctrl(POWER_USB, ON);
	udelay(USB_START_DELAYUS);

	// Initialize our data structures
	for(i = 0; i < USB_NUM_ENDPOINTS; i++) {
		switch(USB_EP_DIRECTION(i)) {
			case USB_ENDPOINT_DIRECTIONS_BIDIR:
				endpoint_directions[i] = USBBiDir;
				break;
			case USB_ENDPOINT_DIRECTIONS_IN:
				endpoint_directions[i] = USBIn;
				break;
			case USB_ENDPOINT_DIRECTIONS_OUT:
				endpoint_directions[i] = USBOut;
				break;
		}
	}

	memset(endpoint_handlers, 0, sizeof(endpoint_handlers));

	// Set up the hardware
	clock_gate_switch(USB_OTGCLOCKGATE, ON);
	clock_gate_switch(USB_PHYCLOCKGATE, ON);
	clock_gate_switch(EDRAM_CLOCKGATE, ON);

	// Generate a soft disconnect on host
	SET_REG(USB + DCTL, GET_REG(USB + DCTL) | DCTL_SFTDISCONNECT);
	udelay(USB_SFTDISCONNECT_DELAYUS);

	// power on OTG
	SET_REG(USB + USB_ONOFF, GET_REG(USB + USB_ONOFF) & (~USB_ONOFF_OFF));
	udelay(USB_ONOFFSTART_DELAYUS);

	// power on PHY
	SET_REG(USB_PHY + OPHYPWR, OPHYPWR_POWERON);
	udelay(USB_PHYPWRPOWERON_DELAYUS);

	// select clock
	SET_REG(USB_PHY + OPHYCLK, (GET_REG(USB_PHY + OPHYCLK) & OPHYCLK_CLKSEL_MASK) | OPHYCLK_CLKSEL_48MHZ);

	// reset phy
	SET_REG(USB_PHY + ORSTCON, GET_REG(USB_PHY + ORSTCON) | ORSTCON_PHYSWRESET);
	udelay(USB_RESET2_DELAYUS);
	SET_REG(USB_PHY + ORSTCON, GET_REG(USB_PHY + ORSTCON) & (~ORSTCON_PHYSWRESET));
	udelay(USB_RESET_DELAYUS);

	SET_REG(USB + GRSTCTL, GRSTCTL_CORESOFTRESET);

	// wait until reset takes
	while((GET_REG(USB + GRSTCTL) & GRSTCTL_CORESOFTRESET) == GRSTCTL_CORESOFTRESET);

	// wait until reset completes
	while(GET_REG(USB + GRSTCTL) >= 0);

	udelay(USB_RESETWAITFINISH_DELAYUS);

	// allow host to reconnect
	SET_REG(USB + DCTL, GET_REG(USB + DCTL) & (~DCTL_SFTDISCONNECT));
	udelay(USB_SFTCONNECT_DELAYUS);

	// flag all interrupts as positive, maybe to disable them

	// Set 7th EP? This is what iBoot does
	InEPRegs[USB_NUM_ENDPOINTS].interrupt = USB_EPINT_INEPNakEff | USB_EPINT_INTknEPMis | USB_EPINT_INTknTXFEmp
		| USB_EPINT_TimeOUT | USB_EPINT_AHBErr | USB_EPINT_EPDisbld | USB_EPINT_XferCompl;
	OutEPRegs[USB_NUM_ENDPOINTS].interrupt = USB_EPINT_OUTTknEPDis
		| USB_EPINT_SetUp | USB_EPINT_AHBErr | USB_EPINT_EPDisbld | USB_EPINT_XferCompl;

	for(i = 0; i < USB_NUM_ENDPOINTS; i++) {
		InEPRegs[i].interrupt = USB_EPINT_INEPNakEff | USB_EPINT_INTknEPMis | USB_EPINT_INTknTXFEmp
			| USB_EPINT_TimeOUT | USB_EPINT_AHBErr | USB_EPINT_EPDisbld | USB_EPINT_XferCompl;
		OutEPRegs[i].interrupt = USB_EPINT_OUTTknEPDis
			| USB_EPINT_SetUp | USB_EPINT_AHBErr | USB_EPINT_EPDisbld | USB_EPINT_XferCompl;
	}

	// disable all interrupts until endpoint descriptors and configuration structures have been setup
	SET_REG(USB + GINTMSK, GINTMSK_NONE);
	SET_REG(USB + DIEPMSK, DIEPMSK_NONE);
	SET_REG(USB + DOEPMSK, DOEPMSK_NONE);

	interrupt_install(USB_INTERRUPT, usbIRQHandler, 0);
	interrupt_enable(USB_INTERRUPT);

	// TODO: possibly initialize buffers
	// Install endpoint handlers

	initializeDescriptors();

	if(inBuffer == NULL)
		inBuffer = memalign(0x40, 0x80);

	if(outBuffer == NULL)
		outBuffer = memalign(0x40, 0x80);

	SET_REG(USB + GAHBCFG, GAHBCFG_DMAEN | GAHBCFG_BSTLEN_INCR8 | GAHBCFG_MASKINT);
	SET_REG(USB + USB_UNKNOWNREG1, USB_UNKNOWNREG1_START);
	SET_REG(USB + DCFG, DCFG_NZSTSOUTHSHK); // some random setting. See specs
	SET_REG(USB + DCFG, GET_REG(USB + DCFG) & ~(DCFG_DEVICEADDRMSK));
	InEPRegs[0].control = USB_EPCON_ACTIVE;
	OutEPRegs[0].control = USB_EPCON_ACTIVE;

	SET_REG(USB + GRXFSIZ, RX_FIFO_DEPTH);
	SET_REG(USB + GNPTXFSIZ, (TX_FIFO_DEPTH << 8) | TX_FIFO_STARTADDR);

	for(i = 0; i < USB_NUM_ENDPOINTS; i++) {
		InEPRegs[i].interrupt = USB_EPINT_INEPNakEff | USB_EPINT_INTknEPMis | USB_EPINT_INTknTXFEmp
			| USB_EPINT_TimeOUT | USB_EPINT_AHBErr | USB_EPINT_EPDisbld | USB_EPINT_XferCompl;
		OutEPRegs[i].interrupt = USB_EPINT_OUTTknEPDis
			| USB_EPINT_SetUp | USB_EPINT_AHBErr | USB_EPINT_EPDisbld | USB_EPINT_XferCompl;
	}

	SET_REG(USB + GINTMSK, GINTMSK_OTG | GINTMSK_SUSPEND | GINTMSK_RESET | GINTMSK_INEP | GINTMSK_OEP | GINTMSK_DISCONNECT);
	SET_REG(USB + DAINTMSK, DAINTMSK_ALL);
	SET_REG(USB + DOEPMSK, DOEPMSK_XFERCOMPL | DOEPMSK_SETUP | DOEPMSK_BACK2BACKSETUP);
	SET_REG(USB + DIEPMSK, DIEPMSK_XFERCOMPL | DIEPMSK_AHBERR | DIEPMSK_TIMEOUT);
	SET_REG(USB + DIEPMSK, DIEPMSK_XFERCOMPL | DIEPMSK_AHBERR | DIEPMSK_TIMEOUT);

	InEPRegs[0].interrupt = USB_EPINT_ALL;
	OutEPRegs[0].interrupt = USB_EPINT_ALL;

	SET_REG(USB + DCTL, DCTL_PROGRAMDONE + DCTL_CGOUTNAK + DCTL_CGNPINNAK);
	udelay(USB_PROGRAMDONE_DELAYUS);
	SET_REG(USB + GOTGCTL, GET_REG(USB + GOTGCTL) | GOTGCTL_SESSIONREQUEST);

	change_state(USBPowered);

	usb_inited = TRUE;

	return 0;
}

static void usbIRQHandler(uint32_t token) {

}

USBDeviceDescriptor* usb_get_device_descriptor() {
	if(configurations == NULL) {
		deviceDescriptor.bLength = sizeof(USBDeviceDescriptor);
		deviceDescriptor.bDescriptorType = USBDeviceDescriptorType;
		deviceDescriptor.bcdUSB = USB_2_0;
		deviceDescriptor.bDeviceClass = 0;
		deviceDescriptor.bDeviceSubClass = 0;
		deviceDescriptor.bDeviceProtocol = 0;
		deviceDescriptor.bMaxPacketSize = USB_MAX_PACKETSIZE;
		deviceDescriptor.idVendor = VENDOR_APPLE;
		deviceDescriptor.idProduct = PRODUCT_IPHONE;
		deviceDescriptor.bcdDevice = DEVICE_IPHONE;
		deviceDescriptor.iManufacturer = addStringDescriptor("Apple Inc.");
		deviceDescriptor.iProduct = addStringDescriptor("Apple Mobile Device (OpenIBoot Mode)");
		deviceDescriptor.iSerialNumber = addStringDescriptor("");
		deviceDescriptor.bNumConfigurations = 0;

		addConfiguration(1, addStringDescriptor("OpenIBoot Mode Configuration"), 0, 0, 500);
	}

	return &deviceDescriptor;
}

USBConfigurationDescriptor* usb_get_configuration_descriptor(int index, uint8_t speed_id) {
	if(index == 0 && configurations[0].interfaces == NULL) {
		USBInterface* interface = addInterfaceDescriptor(&configurations[0], 0, 0,
			OPENIBOOT_INTERFACE_CLASS, OPENIBOOT_INTERFACE_SUBCLASS, OPENIBOOT_INTERFACE_PROTOCOL, addStringDescriptor("IF0"));

		addEndpointDescriptor(interface, 1, USBIn, USBBulk, USBNoSynchronization, USBDataEndpoint, packetsizeFromSpeed(speed_id), 0);
		addEndpointDescriptor(interface, 1, USBOut, USBBulk, USBNoSynchronization, USBDataEndpoint, packetsizeFromSpeed(speed_id), 0);
		endConfiguration(&configurations[0]);
	}

	return &configurations[index].descriptor;
}

static void initializeDescriptors() {
	numStringDescriptors = 0;
	stringDescriptors = NULL;
	configurations = NULL;
}

static void releaseConfigurations() {
	if(configurations == NULL) {
		return;
	}

	int8_t i;
	for(i = 0; i < deviceDescriptor.bNumConfigurations; i++) {
		int8_t j;
		for(j = 0; j < configurations[i].descriptor.bNumInterfaces; j++) {
			free(configurations[i].interfaces[j].endpointDescriptors);
		}
		free(configurations[i].interfaces);
	}

	free(configurations);
	deviceDescriptor.bNumConfigurations = 0;
	configurations = NULL;
}

static uint8_t addConfiguration(uint8_t bConfigurationValue, uint8_t iConfiguration, uint8_t selfPowered, uint8_t remoteWakeup, uint16_t maxPower) {
	uint8_t newIndex = deviceDescriptor.bNumConfigurations;
	deviceDescriptor.bNumConfigurations++;

	configurations = (USBConfiguration*) realloc(configurations, sizeof(USBConfiguration) * deviceDescriptor.bNumConfigurations);
	configurations[newIndex].descriptor.bLength = sizeof(USBConfigurationDescriptor);
	configurations[newIndex].descriptor.bDescriptorType = USBConfigurationDescriptorType;
	configurations[newIndex].descriptor.wTotalLength = 0;
	configurations[newIndex].descriptor.bNumInterfaces = 0;
	configurations[newIndex].descriptor.bConfigurationValue = bConfigurationValue;
	configurations[newIndex].descriptor.iConfiguration = iConfiguration;
	configurations[newIndex].descriptor.bmAttributes = ((0x1) << 7) | ((selfPowered & 0x1) << 6) | ((remoteWakeup & 0x1) << 5);
	configurations[newIndex].descriptor.bMaxPower = maxPower / 2;

	return newIndex;
}

static void endConfiguration(USBConfiguration* configuration) {
	configuration->descriptor.wTotalLength = sizeof(USBConfigurationDescriptor);

	int i;
	for(i = 0; i < configurations->descriptor.bNumInterfaces; i++) {
		configuration->descriptor.wTotalLength += sizeof(USBInterfaceDescriptor) + (configuration->interfaces[i].descriptor.bNumEndpoints * sizeof(USBEndpointDescriptor));
	}
}

static USBInterface* addInterfaceDescriptor(USBConfiguration* configuration, uint8_t bInterfaceNumber, uint8_t bAlternateSetting, uint8_t bInterfaceClass, uint8_t bInterfaceSubClass, uint8_t bInterfaceProtocol, uint8_t iInterface) {
	uint8_t newIndex = configuration->descriptor.bNumInterfaces;
	configuration->descriptor.bNumInterfaces++;

	configuration->interfaces = (USBInterface*) realloc(configuration->interfaces, sizeof(USBInterface) * configuration->descriptor.bNumInterfaces);
	configuration->interfaces[newIndex].descriptor.bLength = sizeof(USBInterfaceDescriptor);
	configuration->interfaces[newIndex].descriptor.bDescriptorType = USBInterfaceDescriptorType;
	configuration->interfaces[newIndex].descriptor.bInterfaceNumber = bInterfaceNumber;
	configuration->interfaces[newIndex].descriptor.bAlternateSetting = bAlternateSetting;
	configuration->interfaces[newIndex].descriptor.bInterfaceClass = bInterfaceClass;
	configuration->interfaces[newIndex].descriptor.bInterfaceSubClass = bInterfaceSubClass;
	configuration->interfaces[newIndex].descriptor.bInterfaceProtocol = bInterfaceProtocol;
	configuration->interfaces[newIndex].descriptor.iInterface = iInterface;
	configuration->interfaces[newIndex].descriptor.bNumEndpoints = 0;
	configuration->interfaces[newIndex].endpointDescriptors = NULL;

	return &configuration->interfaces[newIndex];
}

static uint8_t addEndpointDescriptor(USBInterface* interface, uint8_t endpoint, USBDirection direction, USBTransferType transferType, USBSynchronisationType syncType, USBUsageType usageType, uint16_t wMaxPacketSize, uint8_t bInterval) {
	if(direction > 2)
		return -1;

	uint8_t newIndex = interface->descriptor.bNumEndpoints;
	interface->descriptor.bNumEndpoints++;

	interface->endpointDescriptors = (USBEndpointDescriptor*) realloc(interface->endpointDescriptors, sizeof(USBEndpointDescriptor) * interface->descriptor.bNumEndpoints);
	interface->endpointDescriptors[newIndex].bLength = sizeof(USBEndpointDescriptor);
	interface->endpointDescriptors[newIndex].bDescriptorType = USBEndpointDescriptorType;
	interface->endpointDescriptors[newIndex].bEndpointAddress = (endpoint & 0x3) | ((direction & 0x1) << 7);	// see USB specs for the bitfield spec
	interface->endpointDescriptors[newIndex].bmAttributes = (transferType & 0x3) | ((syncType & 0x3) << 2) | ((usageType & 0x3) << 4);
	interface->endpointDescriptors[newIndex].wMaxPacketSize = wMaxPacketSize;
	interface->endpointDescriptors[newIndex].bInterval = bInterval;

	return newIndex;
}

static uint8_t addStringDescriptor(const char* descriptorString) {
	uint8_t newIndex = numStringDescriptors;
	numStringDescriptors++;

	stringDescriptors = (USBStringDescriptor**) realloc(stringDescriptors, sizeof(USBStringDescriptor*) * numStringDescriptors);

	int sLen = strlen(descriptorString);
	stringDescriptors[newIndex] = (USBStringDescriptor*) malloc(sizeof(USBStringDescriptor) + sLen);
	stringDescriptors[newIndex]->bLength = sizeof(USBStringDescriptor) + sLen;
	stringDescriptors[newIndex]->bDescriptorType = USBStringDescriptorType;
	memcpy(stringDescriptors[newIndex]->bString, descriptorString, sLen);

	firstStringDescriptor = (USBFirstStringDescriptor*) realloc(firstStringDescriptor, sizeof(USBFirstStringDescriptor) + (sizeof(uint16_t) * numStringDescriptors));
	firstStringDescriptor->wLANGID[newIndex] = USB_LANGID_ENGLISH_US;

	return (newIndex + 1);
}

USBStringDescriptor* usb_get_string_descriptor(int index) {
	if(index == 0) {
		return (USBStringDescriptor*) firstStringDescriptor;
	} else {
		return stringDescriptors[index - 1];
	}
}

static void releaseStringDescriptors() {
	int8_t i;

	if(stringDescriptors == NULL) {
		return;
	}

	for(i = 0; i < numStringDescriptors; i++) {
		free(stringDescriptors[i]);
	}

	free(stringDescriptors);

	numStringDescriptors = 0;
	stringDescriptors = NULL;
}

static uint16_t packetsizeFromSpeed(uint8_t speed_id) {
	switch(speed_id) {
		case USB_HIGHSPEED:
			return 512;
		case USB_FULLSPEED:
		case USB_FULLSPEED_48_MHZ:
			return 64;
		case USB_LOWSPEED:
			return 32;
		default:
			return -1;
	}
}

int usb_install_ep_handler(int endpoint, USBDirection direction, USBEndpointHandler handler, uint32_t token) {
	if(endpoint >= USB_NUM_ENDPOINTS) {
		return -1;
	}

	if(endpoint_directions[endpoint] != direction && endpoint_directions[endpoint] != USBBiDir) {
		return -1; // that endpoint can't handle this kind of direction
	}

	if(direction == USBIn) {
		endpoint_handlers[endpoint].in.handler = handler;
		endpoint_handlers[endpoint].in.token = token;
	} else if(direction == USBOut) {
		endpoint_handlers[endpoint].out.handler = handler;
		endpoint_handlers[endpoint].out.token = token;
	} else {
		return -1; // can only register IN or OUt directions
	}

	return 0;
}

int usb_shutdown() {
	power_ctrl(POWER_USB, ON);
	clock_gate_switch(USB_OTGCLOCKGATE, ON);
	clock_gate_switch(USB_PHYCLOCKGATE, ON);

	SET_REG(USB + USB_ONOFF, GET_REG(USB + USB_ONOFF) | USB_ONOFF_OFF); // reset link
	SET_REG(USB_PHY + OPHYPWR, OPHYPWR_FORCESUSPEND | OPHYPWR_PLLPOWERDOWN
		| OPHYPWR_XOPOWERDOWN | OPHYPWR_ANALOGPOWERDOWN | OPHYPWR_UNKNOWNPOWERDOWN); // power down phy

	SET_REG(USB_PHY + ORSTCON, ORSTCON_PHYSWRESET | ORSTCON_LINKSWRESET | ORSTCON_PHYLINKSWRESET); // reset phy/link

	udelay(USB_RESET_DELAYUS);	// wait a millisecond for the changes to stick

	clock_gate_switch(USB_OTGCLOCKGATE, OFF);
	clock_gate_switch(USB_PHYCLOCKGATE, OFF);
	power_ctrl(POWER_USB, OFF);

	releaseConfigurations();
	releaseStringDescriptors();

	return 0;
}

static void change_state(USBState new_state) {
	usb_state = new_state;
	if(usb_state == USBConfigured) {
		// TODO: set to host powered
	}
}

