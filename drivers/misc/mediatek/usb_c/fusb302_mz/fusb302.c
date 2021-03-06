#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <typec.h>
#include "fusb302.h"

#define PD_SUPPORT

#ifdef PD_SUPPORT
#include "usbpd.h"
#endif

#define FUSB302_I2C_NAME "fusb302"
//#define CUST_EINT_CC_DECODER_NUM 95

int VBUS_5V_EN;
int VBUS_12V_EN;

#define PORT_UFP                0
#define PORT_DFP                1
#define PORT_DRP                2

#define HOST_CUR_NO     0
#define HOST_CUR_USB        1
#define HOST_CUR_1500       2
#define HOST_CUR_3000       3

#define CC_UNKNOW       4
#define CC_RD_3000      3
#define CC_RD_1500      2
#define CC_RD_DEFAULT       1
#define CC_RA           0

/* Configure Definition */
#define FUSB302_I2C_NUM     1
#define FUSB302_PORT_TYPE   USBTypeC_Source
#define FUSB302_HOST_CUR    HOST_CUR_USB
//#define FUSB302_PORT_TYPE USBTypeC_DRP
//#define FUDB302_PORT_TYPE USBTypeC_Sink

#define FUSB_MS_TO_NS(x) (x * 1000 * 1000)
#define Delay10us(x) udelay(x*10);

//#define FUSB302_DEBUG
bool fusb302_debug = true;

#define FUSB_LOG(fmt, args...)  do{ if(fusb302_debug) pr_info("[fusb302]" fmt, ##args); }while(0)

struct fusb302_i2c_data {
	struct i2c_client *client;
	struct task_struct *thread;

	spinlock_t lock;
	int gpio_pd_on;
	int gpio_int_ccd;
	int irq_ccd;
	struct device *dev;
	struct pinctrl *pinctrl;
	struct typec_switch_data *host_driver;
	struct typec_switch_data *device_driver;
	struct usbc_pin_ctrl *pin_cfg;
};

struct fusb302_i2c_data *fusb_i2c_data;

bool state_changed;
static DECLARE_COMPLETION(state_notifier);

static struct hrtimer state_hrtimer;
static struct hrtimer debounce_hrtimer1;
static struct hrtimer debounce_hrtimer2;
static struct hrtimer toggle_hrtimer;

extern PolicyState_t PolicyState;
extern ProtocolState_t ProtocolState;
extern PDTxStatus_t PDTxStatus;
//
/////////////////////////////////////////////////////////////////////////////
//      Variables accessible outside of the FUSB300 state machine
/////////////////////////////////////////////////////////////////////////////
FUSB300reg_t            Registers;          // Variable holding the current status of the FUSB300 registers
bool                    USBPDActive;        // Variable to indicate whether the USB PD state machine is active or not
bool                    USBPDEnabled;       // Variable to indicate whether USB PD is enabled (by the host)
uint32_t                  PRSwapTimer;        // Timer used to bail out of a PR_Swap from the Type-C side if necessary

USBTypeCPort            PortType;           // Variable indicating which type of port we are implementing
bool                    blnCCPinIsCC1;      // Flag to indicate if the CC1 pin has been detected as the CC pin
bool                    blnCCPinIsCC2;      // Flag to indicate if the CC2 pin has been detected as the CC pin
bool                    blnSMEnabled;       // Flag to indicate whether the FUSB300 state machine is enabled
ConnectionState         ConnState;          // Variable indicating the current connection state

/////////////////////////////////////////////////////////////////////////////
//      Variables accessible only inside FUSB300 state machine
/////////////////////////////////////////////////////////////////////////////
static bool             blnSrcPreferred;    // Flag to indicate whether we prefer the Src role when in DRP
static bool             blnAccSupport;      // Flag to indicate whether the port supports accessories
static bool             blnINTActive;       // Flag to indicate that an interrupt occurred that needs to be handled
static uint16_t           StateTimer;         // Timer used to validate proceeding to next state
static uint16_t           DebounceTimer1;     // Timer used for first level debouncing
static uint16_t           DebounceTimer2;     // Timer used for second level debouncing
static uint16_t           ToggleTimer;        // Timer used for CC swapping in the FUSB302
static CCTermType       CC1TermAct;         // Active CC1 termination value
static CCTermType       CC2TermAct;         // Active CC2 termination value
static CCTermType       CC1TermDeb;         // Debounced CC1 termination value
static CCTermType       CC2TermDeb;         // Debounced CC2 termination value
static USBTypeCCurrent  SinkCurrent;        // Variable to indicate the current capability we have received
static USBTypeCCurrent  SourceCurrent;      // Variable to indicate the current capability we are broadcasting

static int trigger_driver(struct fusb302_i2c_data *fusb, int type, int stat, int dir);

/* /////////////////////////////////////////////////////////////////////////// */
/* FUSB300 I2C Routines */
/* /////////////////////////////////////////////////////////////////////////// */
#if 0
void fusb300_i2c_w_reg8(struct i2c_client *client, u8 addr, u8 var)
{
	char buffer[2];

	buffer[0] = addr;
	buffer[1] = var;
	i2c_master_send(client, buffer, 2);
}

u8 fusb300_i2c_r_reg(struct i2c_client *client, u8 addr)
{
	u8 var;

	i2c_master_send(client, &addr, 1);
	i2c_master_recv(client, &var, 1);
	return var;
}
#endif

int FUSB300Write(unsigned char regAddr, unsigned char length, unsigned char *data)
{
#if 0
	int i;

	for (i = 0; i < length; i++)
		fusb300_i2c_w_reg8(fusb_i2c_data->client, regAddr + i, data[i]);

	return true;
#else
    return i2c_smbus_write_i2c_block_data(fusb_i2c_data->client, regAddr, length, data);
#endif
}

int FUSB300Read(unsigned char regAddr, unsigned char length, unsigned char *data)
{
#if 0
	int i;

	for (i = 0; i < length; i++)
		data[i] = fusb300_i2c_r_reg(fusb_i2c_data->client, regAddr + i);

	return true;
#else
    return i2c_smbus_read_i2c_block_data(fusb_i2c_data->client, regAddr, length, data);
#endif
}

/////////////////////////////////////////////////////////////////////////////
//                     Internal Routines
////////////////////////////////////////////////////////////////////////////
void SetStateDelayUnattached(void);
void StateMachineUnattached(void);
void StateMachineAttachWaitSnk(void);
void StateMachineAttachedSink(void);
void StateMachineAttachWaitSrc(void);
void StateMachineAttachedSource(void);
void StateMachineTrySrc(void);
void StateMachineTryWaitSnk(void);
void StateMachineAttachWaitAcc(void);
void StateMachineDelayUnattached(void);

void SetStateUnattached(void);
void SetStateAttachWaitSnk(void);
void SetStateAttachWaitAcc(void);
void SetStateAttachWaitSrc(void);
void SetStateTrySrc(void);
void SetStateAttachedSink(void);
void SetStateAttachedSrc(void);
void UpdateSinkCurrent(CCTermType Termination);
void SetStateTryWaitSnk(void);
void UpdateSourcePowerMode(void);

static inline int FUSB300Int_PIN_LVL(void)
{
    int ret = gpio_get_value(fusb_i2c_data->gpio_int_ccd);
    FUSB_LOG("gpio %d = %d\n", fusb_i2c_data->gpio_int_ccd, ret);
    return ret;
}
/*
static void SourceOutput(int vol, int current)
{
    return;
}
*/
void FUSB302_start_timer(struct hrtimer* timer, int ms)
{
    ktime_t ktime;
    ktime = ktime_set(0, FUSB_MS_TO_NS(ms)); 
    hrtimer_start(timer, ktime, HRTIMER_MODE_REL);  
}

enum hrtimer_restart func_hrtimer(struct hrtimer *timer)
{
    if (timer == &state_hrtimer)
        StateTimer = 0;
    else if (timer == &debounce_hrtimer1)
        DebounceTimer1 = 0;
    else if (timer == &debounce_hrtimer2)
        DebounceTimer2 = 0;
    else if (timer == &toggle_hrtimer )
        ToggleTimer = 0;
    
    state_changed = true;
    complete_all(&state_notifier);
    
    return HRTIMER_NORESTART;
}

void wake_up_statemachine(void)
{
    //FUSB_LOG("wake_up_statemachine.\n");
    state_changed = true;   
    complete_all(&state_notifier);
}

static void dump_reg(void)
{
/*
    int reg_addr[] = {regDeviceID, regSwitches0, regSwitches1, regMeasure, regSlice,
        regControl0, regControl1, regControl2, regControl3, regMask, regPower, regReset,
        regOCPreg, regMaska, regMaskb, regControl4, regStatus0a, regStatus1a, regInterrupta,
        regInterruptb, regStatus0, regStatus1, regInterrupt };

    char buf[1024];
    int i, len = 0;
    uint8_t byte = 0;
    for (i=0; i< sizeof(reg_addr)/sizeof(reg_addr[0]); i++)
    {
        FUSB300Read(reg_addr[i], 1, &byte);
        len += sprintf(buf+len, "%02xH:%02x ", reg_addr[i], byte);
        if (((i+1)%6)==0)
            len += sprintf(buf+len, "\n");
    }
    FUSB_LOG("%s\n", buf);
*/
    char buf[1024];
    int i, len = 0;
    //uint8_t byte = 0;
    for (i=0; i<7; i++)
    {
        len += sprintf(buf+len, "%02xH:%02x ", i+regStatus0a, Registers.Status.byte[i]);
    }
    FUSB_LOG("%s\n", buf);
}

/*******************************************************************************
 * Function:        InitializeFUSB300Variables
 * Input:           None
 * Return:          None
 * Description:     Initializes the FUSB300 state machine variables
 ******************************************************************************/
void InitializeFUSB300Variables(void)
{
    blnSMEnabled = true;                // Disable the FUSB300 state machine by default
    blnAccSupport = false;              // Disable accessory support by default
    blnSrcPreferred = false;            // Clear the source preferred flag by default
    PortType = USBTypeC_DRP;            // Initialize to a dual-role port by default
//    PortType = USBTypeC_Sink;            // Initialize to a dual-role port by default
    ConnState = Disabled;               // Initialize to the disabled state?
    blnINTActive = false;               // Clear the handle interrupt flag
    blnCCPinIsCC1 = false;              // Clear the flag to indicate CC1 is CC
    blnCCPinIsCC2 = false;              // Clear the flag to indicate CC2 is CC
 
    StateTimer = USHRT_MAX;             // Disable the state timer
    DebounceTimer1 = USHRT_MAX;         // Disable the 1st debounce timer
    DebounceTimer2 = USHRT_MAX;         // Disable the 2nd debounce timer
    ToggleTimer = USHRT_MAX;            // Disable the toggle timer
    CC1TermDeb = CCTypeNone;            // Set the CC1 termination type to none initially
    CC2TermDeb = CCTypeNone;            // Set the CC2 termination type to none initially
    CC1TermAct = CC1TermDeb;            // Initialize the active CC1 value
    CC2TermAct = CC2TermDeb;            // Initialize the active CC2 value
    SinkCurrent = utccNone;             // Clear the current advertisement initially
    SourceCurrent = utccDefault;        // Set the current advertisement to the default
    Registers.DeviceID.byte = 0x00;     // Clear
    Registers.Switches.byte[0] = 0x03;  // Only enable the device pull-downs by default
    Registers.Switches.byte[1] = 0x00;  // Disable the BMC transmit drivers
    Registers.Measure.byte = 0x00;      // Clear
    Registers.Slice.byte = SDAC_DEFAULT;// Set the SDAC threshold to ~0.544V by default (from FUSB302)
    Registers.Control.byte[0] = 0x20;   // Set to mask all interrupts by default (from FUSB302)
    Registers.Control.byte[1] = 0x00;   // Clear
    Registers.Control.byte[2] = 0x02;   //
    Registers.Control.byte[3] = 0x06;   //
    Registers.Mask.byte = 0x00;         // Clear
    Registers.Power.byte = 0x07;        // Initialize to everything enabled except oscillator
    Registers.Status.Status = 0;        // Clear status bytes
    Registers.Status.StatusAdv = 0;     // Clear the advanced status bytes
    Registers.Status.Interrupt1 = 0;    // Clear the interrupt1 byte
    Registers.Status.InterruptAdv = 0;  // Clear the advanced interrupt bytes
    USBPDActive = false;                // Clear the USB PD active flag
    USBPDEnabled = true;                // Clear the USB PD enabled flag until enabled by the host
    PRSwapTimer = 0;                    // Clear the PR Swap timer

    //
    state_changed = false;
    hrtimer_init(&state_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    state_hrtimer.function = func_hrtimer;  

    hrtimer_init(&debounce_hrtimer1, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    debounce_hrtimer1.function = func_hrtimer;  

    hrtimer_init(&debounce_hrtimer2, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    debounce_hrtimer2.function = func_hrtimer;  

    hrtimer_init(&toggle_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    toggle_hrtimer.function = func_hrtimer;  
}

void InitializeFUSB300(void)
{
    FUSB_LOG("enter:%s", __func__);
    FUSB300Read(regDeviceID, 2, &Registers.DeviceID.byte);          // Read the device ID
    FUSB300Read(regSlice, 1, &Registers.Slice.byte);                // Read the slice
    Registers.Mask.byte = 0x44;                                     // Mask I_ACTIVITY and I_WAKE to reduce system load
    FUSB300Write(regMask, 1, &Registers.Mask.byte);                 // Clear all interrupt masks (we want to do something with them)
    // Update the control and power values since they will be written in either the Unattached.UFP or Unattached.DFP states
    Registers.Control.dword = 0x06220004;                           // Reset all back to default, but clear the INT_MASK bit
    switch (PortType)
    {
        case USBTypeC_Sink:
            Registers.Control.MODE = 0b10;
            break;
        case USBTypeC_Source:
            Registers.Control.MODE = 0b11;
            break;
        default:
            Registers.Control.MODE = 0b01;
            break;
    }
    FUSB300Write(regControl2, 2, &Registers.Control.byte[2]);       // Update the control registers for toggling
    Registers.Control4.TOG_USRC_EXIT = 1;                           // Stop toggling with Ra/Ra
    FUSB300Write(regControl4, 1, &Registers.Control4.byte);         // Commit to the device
    Registers.Power.byte = 0x01;                                    // Initialize such that only the bandgap and wake circuit are enabled by default
    FUSB300Read(regStatus0a, 2, &Registers.Status.byte[0]);         // Read the advanced status registers to make sure we are in sync with the device
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the standard status registers to make sure we are in sync with the device
    // Do not read any of the interrupt registers, let the state machine handle those
    SetStateDelayUnattached();
}

void DisableFUSB300StateMachine(void)
{
    blnSMEnabled = false;
    SetStateDisabled();
    //InitializeFUSB300Timer(false);
}

void EnableFUSB300StateMachine(void)
{
    InitializeFUSB300();
//    InitializeFUSB300Timer(true);
    blnSMEnabled = true;
}

/*******************************************************************************
 * Function:        StateMachineFUSB300
 * Input:           None
 * Return:          None
 * Description:     This is the state machine for the entire USB PD
 *                  This handles all the communication between the master and
 *                  slave.  This function is called by the Timer3 ISR on a
 *                  sub-interval of the 1/4 UI in order to meet timing
 *                  requirements.
 ******************************************************************************/
void StateMachineFUSB300(void)
{
    FUSB_LOG("connstate=%d, policyState=%d, protocolState=%d, PDTxStatus=%d\n", 
        ConnState, PolicyState, ProtocolState, PDTxStatus);
    //if (!blnSMEnabled)
    //    return;
    if (!FUSB300Int_PIN_LVL())
    {
        FUSB300Read(regStatus0a, 7, &Registers.Status.byte[0]);     // Read the interrupta, interruptb, status0, status1 and interrupt registers
        dump_reg();
    }
#ifdef PD_SUPPORT
    if (USBPDActive)                                                // Only call the USB PD routines if we have enabled the block
    {
        USBPDProtocol();                                            // Call the protocol state machine to handle any timing critical operations
        USBPDPolicyEngine();                                        // Once we have handled any Type-C and protocol events, call the USB PD Policy Engine
    }
#endif

    switch (ConnState)
    {
        case Disabled:
            StateMachineDisabled();
            break;
        case ErrorRecovery:
            StateMachineErrorRecovery();
            break;
        case Unattached:
            StateMachineUnattached();
            break;
        case AttachWaitSink:
            StateMachineAttachWaitSnk();
            break;            
        case AttachedSink:
            StateMachineAttachedSink();
            break;
        case AttachWaitSource:
            StateMachineAttachWaitSrc();
            break;
        case AttachedSource:
            StateMachineAttachedSource();
            break;
        case TrySource:
            StateMachineTrySrc();
            break;
        case TryWaitSink:
            StateMachineTryWaitSnk();
            break;
        case AudioAccessory:
            StateMachineAudioAccessory();
            break;
        case DebugAccessory:
            StateMachineDebugAccessory();
            break;
        case AttachWaitAccessory:
            StateMachineAttachWaitAcc();
            break;
        case PoweredAccessory:
            StateMachinePoweredAccessory();
            break;
        case UnsupportedAccessory:
            StateMachineUnsupportedAccessory();
            break;
        case DelayUnattached:
            StateMachineDelayUnattached();
            break;
        default:                                                    
            SetStateDelayUnattached();                                          // We shouldn't get here, so go to the unattached state just in case
            break;
    }
    Registers.Status.Interrupt1 = 0;            // Clear the interrupt register once we've gone through the state machines
    Registers.Status.InterruptAdv = 0;          // Clear the advanced interrupt registers once we've gone through the state machines
}

void StateMachineDisabled(void)
{
    // Do nothing until directed to go to some other state...
}

void StateMachineErrorRecovery(void)
{
    if (StateTimer == 0)
    {
        SetStateDelayUnattached();
    }
}

void StateMachineDelayUnattached(void)
{
    if (StateTimer == 0)
    {
        SetStateUnattached();
    }
}

void StateMachineUnattached(void)
{
    if (Registers.Status.I_TOGDONE)
    {
       /* restore I_BC_LVL */
       Registers.Mask.M_BC_LVL = 0;
       Registers.Mask.M_COMP_CHNG = 0;
       FUSB300Write(regMask, 1, &Registers.Mask.byte);
       /* end */
        switch (Registers.Status.TOGSS)
        {
            case 0b101: // Rp detected on CC1
                blnCCPinIsCC1 = true;
                blnCCPinIsCC2 = false;
                SetStateAttachWaitSnk();                                        // Go to the AttachWaitSnk state
                break;
            case 0b110: // Rp detected on CC2
                blnCCPinIsCC1 = false;
                blnCCPinIsCC2 = true;
                SetStateAttachWaitSnk();                                        // Go to the AttachWaitSnk state
                break;
            case 0b001: // Rd detected on CC1
                blnCCPinIsCC1 = true;
                blnCCPinIsCC2 = false;
                if ((PortType == USBTypeC_Sink) && (blnAccSupport))             // If we are configured as a sink and support accessories...
                    SetStateAttachWaitAcc();                                    // Go to the AttachWaitAcc state
                else                                                            // Otherwise we must be configured as a source or DRP
                    SetStateAttachWaitSrc();                                    // So go to the AttachWaitSnk state
                break;
            case 0b010: // Rd detected on CC2
                blnCCPinIsCC1 = false;
                blnCCPinIsCC2 = true;
                if ((PortType == USBTypeC_Sink) && (blnAccSupport))             // If we are configured as a sink and support accessories...
                    SetStateAttachWaitAcc();                                    // Go to the AttachWaitAcc state
                else                                                            // Otherwise we must be configured as a source or DRP
                    SetStateAttachWaitSrc();                                    // So go to the AttachWaitSnk state
                break;
            case 0b111: // Ra detected on both CC1 and CC2
                blnCCPinIsCC1 = false;
                blnCCPinIsCC2 = false;
                if ((PortType == USBTypeC_Sink) && (blnAccSupport))             // If we are configured as a sink and support accessories...
                    SetStateAttachWaitAcc();                                    // Go to the AttachWaitAcc state
                else                                                            // Otherwise we must be configured as a source or DRP
                    SetStateAttachWaitSrc();                                    // So go to the AttachWaitSnk state
                break;
            default:    // Shouldn't get here, but just in case reset everything...
                Registers.Control.TOGGLE = 0;                                   // Disable the toggle in order to clear...
                FUSB300Write(regControl2, 1, &Registers.Control.byte[2]);       // Commit the control state
                Delay10us(1);
	       /* MASK I_BC_LVL in toggle process */
	       Registers.Mask.M_BC_LVL = 1;
	       Registers.Mask.M_COMP_CHNG = 1;
	       FUSB300Write(regMask, 1, &Registers.Mask.byte);
	       /* end */

                
                Registers.Control.TOGGLE = 1;                                   // Re-enable the toggle state machine... (allows us to get another I_TOGDONE interrupt)
                FUSB300Write(regControl2, 1, &Registers.Control.byte[2]);       // Commit the control state
                break;
        }
    }
  //  rand();
}

void StateMachineAttachWaitSnk(void)
{
    // If the both CC lines has been open for tPDDebounce, go to the unattached state
    // If VBUS and the we've been Rd on exactly one pin for 100ms... go to the attachsnk state
    CCTermType CCValue = DecodeCCTermination();                                 // Grab the latest CC termination value
    if (Registers.Switches.MEAS_CC1)                                            // If we are looking at CC1
    {
        if (CC1TermAct != CCValue)                                              // Check to see if the value has changed...
        {
            CC1TermAct = CCValue;                                               // If it has, update the value
            DebounceTimer1 = tPDDebounceMin;                                     // Restart the debounce timer with tPDDebounce (wait 10ms before detach)
            FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);        
        }
    }
    else                                                                        // Otherwise we are looking at CC2
    {
        if (CC2TermAct != CCValue)                                              // Check to see if the value has changed...
        {
            CC2TermAct = CCValue;                                               // If it has, update the value
            DebounceTimer1 = tPDDebounceMin;                                    // Restart the debounce timer with tPDDebounce (wait 10ms before detach)
            FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
        }
    }
    if (DebounceTimer1 == 0)                                                     // Check to see if our debounce timer has expired...
    {
        DebounceTimer1 = USHRT_MAX;                                             // If it has, disable it so we don't come back in here until we have debounced a change in state
        if ((CC1TermDeb != CC1TermAct) || (CC2TermDeb != CC2TermAct)) {
            DebounceTimer2 = tCCDebounceMin - tPDDebounceMin;                   // Once the CC state is known, start the tCCDebounce timer to validate 
            FUSB302_start_timer(&debounce_hrtimer2,DebounceTimer2);
        } 
        CC1TermDeb = CC1TermAct;                                                // Update the CC1 debounced value
        CC2TermDeb = CC2TermAct;                                                // Update the CC2 debounced value
    }
    if (ToggleTimer == 0)                                                       // If are toggle timer has expired, it's time to swap detection
    {
        if (Registers.Switches.MEAS_CC1)                                        // If we are currently on the CC1 pin...
            ToggleMeasureCC2();                                                 // Toggle over to look at CC2
        else                                                                    // Otherwise assume we are using the CC2...
            ToggleMeasureCC1();                                                 // So toggle over to look at CC1
        ToggleTimer = tFUSB302Toggle;                                          // Reset the toggle timer to our default toggling (<tPDDebounce to avoid disconnecting the other side when we remove pull-ups)
        FUSB302_start_timer(&toggle_hrtimer, ToggleTimer);
    }
    if ((CC1TermDeb == CCTypeRa) && (CC2TermDeb == CCTypeRa))                   // If we have detected SNK.Open for atleast tPDDebounce on both pins...
        SetStateDelayUnattached();                                              // Go to the unattached state
    else if (Registers.Status.VBUSOK && (DebounceTimer2 == 0))                  // If we have detected VBUS and we have detected an Rp for >tCCDebounce...
    {
        if ((CC1TermDeb > CCTypeRa) && (CC2TermDeb == CCTypeRa))                // If Rp is detected on CC1
        {
            if ((PortType == USBTypeC_DRP) && blnSrcPreferred)                  // If we are configured as a DRP and prefer the source role...
                SetStateTrySrc();                                               // Go to the Try.Src state
            else                                                                // Otherwise we are free to attach as a sink
            {
		trigger_driver(fusb_i2c_data, DEVICE_TYPE, ENABLE, UP_SIDE);
                blnCCPinIsCC1 = true;                                           // Set the CC pin to CC1
                blnCCPinIsCC2 = false;                                          //
                SetStateAttachedSink();                                         // Go to the Attached.Snk state               
            }
        }
        else if ((CC1TermDeb == CCTypeRa) && (CC2TermDeb > CCTypeRa))           // If Rp is detected on CC2
        {
            if ((PortType == USBTypeC_DRP) && blnSrcPreferred)                  // If we are configured as a DRP and prefer the source role...
                SetStateTrySrc();                                               // Go to the Try.Src state
            else                                                                // Otherwise we are free to attach as a sink
            {
		trigger_driver(fusb_i2c_data, DEVICE_TYPE, ENABLE, DOWN_SIDE);
                blnCCPinIsCC1 = false;                                          //
                blnCCPinIsCC2 = true;                                           // Set the CC pin to CC2
                SetStateAttachedSink();                                         // Go to the Attached.Snk State
            }
        }
    }    
}

void StateMachineAttachWaitSrc(void)
{
    CCTermType CCValue = DecodeCCTermination();                                 // Grab the latest CC termination value
    if (Registers.Switches.MEAS_CC1)                                            // If we are looking at CC1
    {
        if (CC1TermAct != CCValue)                                              // Check to see if the value has changed...
        {
            CC1TermAct = CCValue;                                               // If it has, update the value
            DebounceTimer1 = tPDDebounceMin;                                    // Restart the debounce timer (tPDDebounce)
            FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
        }
    }
    else                                                                        // Otherwise we are looking at CC2
    {
        if (CC2TermAct != CCValue)                                              // Check to see if the value has changed...
        {
            CC2TermAct = CCValue;                                               // If it has, update the value
            DebounceTimer1 = tPDDebounceMin;                                     // Restart the debounce timer (tPDDebounce)
            FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
        }
    }
    if (DebounceTimer1 == 0)                                                     // Check to see if our debounce timer has expired...
    {
        DebounceTimer1 = USHRT_MAX;                                             // If it has, disable it so we don't come back in here until we have debounced a change in state
        if ((CC1TermDeb != CC1TermAct) || (CC2TermDeb != CC2TermAct)) {
            DebounceTimer2 = tCCDebounceMin;                                    // Once the CC state is known, start the tCCDebounce timer to validate
            FUSB302_start_timer(&debounce_hrtimer2,DebounceTimer2);
        }
        CC1TermDeb = CC1TermAct;                                                // Update the CC1 debounced value
        CC2TermDeb = CC2TermAct;                                                // Update the CC2 debounced value
    }
    if (ToggleTimer == 0)                                                       // If are toggle timer has expired, it's time to swap detection
    {
        if (Registers.Switches.MEAS_CC1)                                        // If we are currently on the CC1 pin...
            ToggleMeasureCC2();                                                 // Toggle over to look at CC2
        else                                                                    // Otherwise assume we are using the CC2...
            ToggleMeasureCC1();                                                 // So toggle over to look at CC1
        ToggleTimer = tFUSB302Toggle;                                         // Reset the toggle timer to our default toggling (<tPDDebounce to avoid disconnecting the other side when we remove pull-ups)
        FUSB302_start_timer(&toggle_hrtimer, ToggleTimer);
    }
    if ((CC1TermDeb == CCTypeNone) && (CC2TermDeb == CCTypeNone))               // If our debounced signals are both open, go to the unattached state
    {
        SetStateDelayUnattached();
    }
    else if ((CC1TermDeb == CCTypeNone) && (CC2TermDeb == CCTypeRa))            // If exactly one pin is open and the other is Ra, go to the unattached state
    {
        SetStateDelayUnattached();
    }
    else if ((CC1TermDeb == CCTypeRa) && (CC2TermDeb == CCTypeNone))            // If exactly one pin is open and the other is Ra, go to the unattached state
    {
        SetStateDelayUnattached();
    }
    else if (DebounceTimer2 == 0)                                               // Otherwise, we are checking to see if we have had a solid state for tCCDebounce
    {
        if ((CC1TermDeb == CCTypeRa) && (CC2TermDeb == CCTypeRa))               // If both pins are Ra, it's an audio accessory
            SetStateAudioAccessory();
        else if ((CC1TermDeb > CCTypeRa) && (CC2TermDeb > CCTypeRa))            // If both pins are Rd, it's a debug accessory
            SetStateDebugAccessory();
        else if (CC1TermDeb > CCTypeRa)                                         // If CC1 is Rd and CC2 is not...
        {
	    trigger_driver(fusb_i2c_data, HOST_TYPE, ENABLE, UP_SIDE);
            blnCCPinIsCC1 = true;                                               // Set the CC pin to CC1
            blnCCPinIsCC2 = false;
            SetStateAttachedSrc();                                              // Go to the Attached.Src state
        }
        else if (CC2TermDeb > CCTypeRa)                                         // If CC2 is Rd and CC1 is not...
        {
	    trigger_driver(fusb_i2c_data, HOST_TYPE, ENABLE, DOWN_SIDE);
            blnCCPinIsCC1 = false;
            blnCCPinIsCC2 = true;                                               // Set the CC pin to CC2
            SetStateAttachedSrc();                                              // Go to the Attached.Src state
        }
    }
}

void StateMachineAttachWaitAcc(void)
{
    CCTermType CCValue = DecodeCCTermination();                                 // Grab the latest CC termination value
    if (Registers.Switches.MEAS_CC1)                                            // If we are looking at CC1
    {
        if (CC1TermAct != CCValue)                                              // Check to see if the value has changed...
        {
            CC1TermAct = CCValue;                                               // If it has, update the value
            DebounceTimer1 = tCCDebounceNom;                                    // Restart the debounce timer (tCCDebounce)
            FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
        }
    }
    else                                                                        // Otherwise we are looking at CC2
    {
        if (CC2TermAct != CCValue)                                              // Check to see if the value has changed...
        {
            CC2TermAct = CCValue;                                               // If it has, update the value
            DebounceTimer1 = tCCDebounceNom;                                    // Restart the debounce timer (tCCDebounce)
            FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
        }
    }
    if (ToggleTimer == 0)                                                       // If are toggle timer has expired, it's time to swap detection
    {
        if (Registers.Switches.MEAS_CC1)                                        // If we are currently on the CC1 pin...
            ToggleMeasureCC2();                                                 // Toggle over to look at CC2
        else                                                                    // Otherwise assume we are using the CC2...
            ToggleMeasureCC1();                                                 // So toggle over to look at CC1
        ToggleTimer = tFUSB302Toggle;                                           // Reset the toggle timer to our default toggling (<tPDDebounce to avoid disconnecting the other side when we remove pull-ups)
        FUSB302_start_timer(&toggle_hrtimer,ToggleTimer);
    }
    if (DebounceTimer1 == 0)                                                    // Check to see if the signals have been stable for tCCDebounce
    {
        if ((CC1TermDeb == CCTypeRa) && (CC2TermDeb == CCTypeRa))               // If they are both Ra, it's an audio accessory
            SetStateAudioAccessory();
        else if ((CC1TermDeb > CCTypeRa) && (CC2TermDeb > CCTypeRa))            // If they are both Rd, it's a debug accessory
            SetStateDebugAccessory();
        else if ((CC1TermDeb == CCTypeNone) || (CC2TermDeb == CCTypeNone))      // If either pin is open, it's considered a detach
            SetStateDelayUnattached();
        else if ((CC1TermDeb > CCTypeRa) && (CC2TermDeb == CCTypeRa))           // If CC1 is Rd and CC2 is Ra, it's a powered accessory (CC1 is CC)
        {
            blnCCPinIsCC1 = true;
            blnCCPinIsCC2 = false;
            SetStatePoweredAccessory();
        }
        else if ((CC1TermDeb == CCTypeRa) && (CC2TermDeb > CCTypeRa))           // If CC1 is Ra and CC2 is Rd, it's a powered accessory (CC2 is CC)
        {
            blnCCPinIsCC1 = true;
            blnCCPinIsCC2 = false;
            SetStatePoweredAccessory();
        }
    }
}

void StateMachineAttachedSink(void)
{
    CCTermType CCValue = DecodeCCTermination();                                 // Grab the latest CC termination value
    //FUSB_LOG("vbus=%d, prswaptimer=%d\n", Registers.Status.VBUSOK, PRSwapTimer);
    if ((Registers.Status.VBUSOK == false) && (!PRSwapTimer)) {                  // If VBUS is removed and we are not in the middle of a power role swap...
        SetStateDelayUnattached();                                              // Go to the unattached state
	trigger_driver(fusb_i2c_data, DEVICE_TYPE, DISABLE, DONT_CARE);
    } else {
        if (Registers.Switches.MEAS_CC1)                                        // If we are looking at CC1
        {
            if (CCValue != CC1TermAct)                                          // If the CC voltage has changed...
            {
                CC1TermAct = CCValue;                                           // Store the updated value
                DebounceTimer1 = tPDDebounceMin;                                // Reset the debounce timer to the minimum tPDdebounce
                FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
            }
            else if (DebounceTimer1 == 0)                                       // If the signal has been debounced
            {
                DebounceTimer1 = USHRT_MAX;                                     // Disable the debounce timer until we get a change
                CC1TermDeb = CC1TermAct;                                        // Store the debounced termination for CC1
                UpdateSinkCurrent(CC1TermDeb);                                  // Update the advertised current
            }
        }
        else
        {
            if (CCValue != CC2TermAct)                                          // If the CC voltage has changed...
            {
                CC2TermAct = CCValue;                                           // Store the updated value
                DebounceTimer1 = tPDDebounceMin;                                 // Reset the debounce timer to the minimum tPDdebounce
                FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
            }
            else if (DebounceTimer1 == 0)                                        // If the signal has been debounced
            {
                DebounceTimer1 = USHRT_MAX;                                     // Disable the debounce timer until we get a change
                CC2TermDeb = CC2TermAct;                                        // Store the debounced termination for CC2
                UpdateSinkCurrent(CC2TermDeb);                                  // Update the advertised current
            }
        }
    }
}

void StateMachineAttachedSource(void)
{
    CCTermType CCValue = DecodeCCTermination();                                 // Grab the latest CC termination value
    if (Registers.Switches.MEAS_CC1)                                            // Did we detect CC1 as the CC pin?
    {
        if (CC1TermAct != CCValue)                                              // If the CC voltage has changed...
        {
            CC1TermAct = CCValue;                                               // Store the updated value
            DebounceTimer1 = tPDDebounceMin;                                    // Reset the debounce timer to the minimum tPDdebounce
            FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
         }
        else if (DebounceTimer1 == 0)                                           // If the signal has been debounced
        {
            DebounceTimer1 = USHRT_MAX;                                         // Disable the debounce timer until we get a change
            CC1TermDeb = CC1TermAct;                                            // Store the debounced termination for CC1
        }
        if ((CC1TermDeb == CCTypeNone) && (!PRSwapTimer))                       // If the debounced CC pin is detected as open and we aren't in the middle of a PR_Swap
        {
            if ((PortType == USBTypeC_DRP) && blnSrcPreferred)                  // Check to see if we need to go to the TryWait.SNK state...
                SetStateTryWaitSnk();
            else                                                                // Otherwise we are going to the unattached state
                SetStateDelayUnattached();
		trigger_driver(fusb_i2c_data, HOST_TYPE, DISABLE, DONT_CARE);
        }
    }
    else                                                                        // We must have detected CC2 as the CC pin
    {
        if (CC2TermAct != CCValue)                                              // If the CC voltage has changed...
        {
            CC2TermAct = CCValue;                                               // Store the updated value
            DebounceTimer1 = tPDDebounceMin;                                     // Reset the debounce timer to the minimum tPDdebounce
            FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
        }
        else if (DebounceTimer1 == 0)                                            // If the signal has been debounced
        {
            DebounceTimer1 = USHRT_MAX;                                         // Disable the debounce timer until we get a change
            CC2TermDeb = CC2TermAct;                                            // Store the debounced termination for CC1
        }
        if ((CC2TermDeb == CCTypeNone) && (!PRSwapTimer))                       // If the debounced CC pin is detected as open and we aren't in the middle of a PR_Swap
        {
            if ((PortType == USBTypeC_DRP) && blnSrcPreferred)                  // Check to see if we need to go to the TryWait.SNK state...
                SetStateTryWaitSnk();
            else {                                                              // Otherwise we are going to the unattached state
                SetStateDelayUnattached();
		trigger_driver(fusb_i2c_data, HOST_TYPE, DISABLE, DONT_CARE);
		}
        }
    }
}

void StateMachineTryWaitSnk(void)
{
    CCTermType CCValue = DecodeCCTermination();                                 // Grab the latest CC termination value
    if (Registers.Switches.MEAS_CC1)                                            // If we are looking at CC1
    {
        if (CC1TermAct != CCValue)                                              // Check to see if the value has changed...
        {
            CC1TermAct = CCValue;                                               // If it has, update the value
            DebounceTimer1 = tPDDebounceMin;                                     // Restart the debounce timer with tPDDebounce (wait 10ms before detach)
            FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
        }
    }
    else                                                                        // Otherwise we are looking at CC2
    {
        if (CC2TermAct != CCValue)                                              // Check to see if the value has changed...
        {
            CC2TermAct = CCValue;                                               // If it has, update the value
            DebounceTimer1 = tPDDebounceMin;                                    // Restart the debounce timer with tPDDebounce (wait 10ms before detach)
            FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
        }
    }
    if (DebounceTimer1 == 0)                                                     // Check to see if our debounce timer has expired...
    {
        DebounceTimer1 = USHRT_MAX;                                             // If it has, disable it so we don't come back in here until we have debounced a change in state
        if ((CC1TermDeb != CC1TermAct) || (CC2TermDeb != CC2TermAct))
        {
            DebounceTimer2 = tCCDebounceMin - tPDDebounceMin;                   // Once the CC state is known, start the tCCDebounce timer to validate 
            //FIXME  handle timer restart.
            FUSB302_start_timer(&debounce_hrtimer2,DebounceTimer2);
        }
        CC1TermDeb = CC1TermAct;                                                // Update the CC1 debounced value
        CC2TermDeb = CC2TermAct;                                                // Update the CC2 debounced value
    }
    if (ToggleTimer == 0)                                                       // If are toggle timer has expired, it's time to swap detection
    {
        if (Registers.Switches.MEAS_CC1)                                        // If we are currently on the CC1 pin...
            ToggleMeasureCC2();                                                 // Toggle over to look at CC2
        else                                                                    // Otherwise assume we are using the CC2...
            ToggleMeasureCC1();                                                 // So toggle over to look at CC1
        ToggleTimer = tFUSB302Toggle;                                           // Reset the toggle timer to our default toggling (<tPDDebounce to avoid disconnecting the other side when we remove pull-ups)
        FUSB302_start_timer(&toggle_hrtimer,ToggleTimer);
    }
    if ((StateTimer == 0) && (CC1TermDeb == CCTypeRa) && (CC2TermDeb == CCTypeRa))  // If tDRPTryWait has expired and we detected open on both pins...
        SetStateDelayUnattached();                                              // Go to the unattached state
    else if (Registers.Status.VBUSOK && (DebounceTimer2 == 0))                  // If we have detected VBUS and we have detected an Rp for >tCCDebounce...
    {
        if ((CC1TermDeb > CCTypeRa) && (CC2TermDeb == CCTypeRa))                // If Rp is detected on CC1
        {
            blnCCPinIsCC1 = true;                                               // Set the CC pin to CC1
            blnCCPinIsCC2 = false;                                              //
            SetStateAttachedSink();                                             // Go to the Attached.Snk state
        }
        else if ((CC1TermDeb == CCTypeRa) && (CC2TermDeb > CCTypeRa))           // If Rp is detected on CC2
        {
            blnCCPinIsCC1 = false;                                              //
            blnCCPinIsCC2 = true;                                               // Set the CC pin to CC2
            SetStateAttachedSink();                                             // Go to the Attached.Snk State
        }
    }
}

void StateMachineTrySrc(void)
{
    CCTermType CCValue = DecodeCCTermination();                                 // Grab the latest CC termination value
    if (Registers.Switches.MEAS_CC1)                                            // If we are looking at CC1
    {
        if (CC1TermAct != CCValue)                                              // Check to see if the value has changed...
        {
            CC1TermAct = CCValue;                                               // If it has, update the value
            DebounceTimer1 = tPDDebounceMin;                                    // Restart the debounce timer (tPDDebounce)
            FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
        }
    }
    else                                                                        // Otherwise we are looking at CC2
    {
        if (CC2TermAct != CCValue)                                              // Check to see if the value has changed...
        {
            CC2TermAct = CCValue;                                               // If it has, update the value
            DebounceTimer1 = tPDDebounceMin;                                    // Restart the debounce timer (tPDDebounce)
            FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
        }
    }
    if (DebounceTimer1 == 0)                                                     // Check to see if our debounce timer has expired...
    {
        DebounceTimer1 = USHRT_MAX;                                             // If it has, disable it so we don't come back in here until a new debounce value is ready
        CC1TermDeb = CC1TermAct;                                                // Update the CC1 debounced value
        CC2TermDeb = CC2TermAct;                                                // Update the CC2 debounced value
    }
    if (ToggleTimer == 0)                                                       // If are toggle timer has expired, it's time to swap detection
    {
        if (Registers.Switches.MEAS_CC1)                                        // If we are currently on the CC1 pin...
            ToggleMeasureCC2();                                                 // Toggle over to look at CC2
        else                                                                    // Otherwise assume we are using the CC2...
            ToggleMeasureCC1();                                                 // So toggle over to look at CC1
        ToggleTimer = tPDDebounceMax;                                           // Reset the toggle timer to the max tPDDebounce to ensure the other side sees the pull-up for the min tPDDebounce
        FUSB302_start_timer(&toggle_hrtimer,ToggleTimer);
    }
    if ((CC1TermDeb > CCTypeRa) && ((CC2TermDeb == CCTypeNone) || (CC2TermDeb == CCTypeRa)))    // If the CC1 pin is Rd for atleast tPDDebounce...
    {
        blnCCPinIsCC1 = true;                                                   // The CC pin is CC1
        blnCCPinIsCC2 = false;
        SetStateAttachedSrc();                                                  // Go to the Attached.Src state
    }
    else if ((CC2TermDeb > CCTypeRa) && ((CC1TermDeb == CCTypeNone) || (CC1TermDeb == CCTypeRa)))   // If the CC2 pin is Rd for atleast tPDDebounce...
    {
        blnCCPinIsCC1 = false;                                                  // The CC pin is CC2
        blnCCPinIsCC2 = true;
        SetStateAttachedSrc();                                                  // Go to the Attached.Src state
    }
    else if (StateTimer == 0)                                                   // If we haven't detected Rd on exactly one of the pins and we have waited for tDRPTry...
        SetStateTryWaitSnk();                                                   // Move onto the TryWait.Snk state to not get stuck in here
}

void StateMachineDebugAccessory(void)
{
    CCTermType CCValue = DecodeCCTermination();                                 // Grab the latest CC termination value
    if (CC1TermAct != CCValue)                                                  // If the CC voltage has changed...
    {
        CC1TermAct = CCValue;                                                   // Store the updated value
        DebounceTimer1 = tCCDebounceMin;                                        // Reset the debounce timer to the minimum tCCDebounce
        FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    }
    else if (DebounceTimer1 == 0)                                               // If the signal has been debounced
    {
        DebounceTimer1 = USHRT_MAX;                                             // Disable the debounce timer until we get a change
        CC1TermDeb = CC1TermAct;                                                // Store the debounced termination for CC1
    }
    if (CC1TermDeb == CCTypeNone)                                               // If we have detected an open for > tCCDebounce
        SetStateDelayUnattached();                                              // Go to the unattached state

}

void StateMachineAudioAccessory(void)
{
    CCTermType CCValue = DecodeCCTermination();                                 // Grab the latest CC termination value
    if (CC1TermAct != CCValue)                                                  // If the CC voltage has changed...
    {
        CC1TermAct = CCValue;                                                   // Store the updated value
        DebounceTimer1 = tCCDebounceMin;                                        // Reset the debounce timer to the minimum tCCDebounce
        FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    }
    else if (DebounceTimer1 == 0)                                               // If the signal has been debounced
    {
        DebounceTimer1 = USHRT_MAX;                                             // Disable the debounce timer until we get a change
        CC1TermDeb = CC1TermAct;                                                // Store the debounced termination for CC1
    }
    if (CC1TermDeb == CCTypeNone)                                               // If we have detected an open for > tCCDebounce
        SetStateDelayUnattached();                                              // Go to the unattached state
}

void StateMachinePoweredAccessory(void)
{
    CCTermType CCValue = DecodeCCTermination();                                 // Grab the latest CC termination value
    if (CC1TermAct != CCValue)                                                  // If the CC voltage has changed...
    {
        CC1TermAct = CCValue;                                                   // Store the updated value
        DebounceTimer1 = tPDDebounceMin;                                        // Reset the debounce timer to the minimum tPDdebounce
        FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    }
    else if (DebounceTimer1 == 0)                                               // If the signal has been debounced
    {
        DebounceTimer1 = USHRT_MAX;                                             // Disable the debounce timer until we get a change
        CC1TermDeb = CC1TermAct;                                                // Store the debounced termination for CC1
    }
    if (CC1TermDeb == CCTypeNone)                                               // If we have detected an open for > tCCDebounce
        SetStateDelayUnattached();                                              // Go to the unattached state
    else if (StateTimer == 0)                                                   // If we have timed out (tAMETimeout) and haven't entered an alternate mode...
        SetStateUnsupportedAccessory();                                         // Go to the Unsupported.Accessory state
}

void StateMachineUnsupportedAccessory(void)
{
    CCTermType CCValue = DecodeCCTermination();                                 // Grab the latest CC termination value
    if (CC1TermAct != CCValue)                                                  // If the CC voltage has changed...
    {
        CC1TermAct = CCValue;                                                   // Store the updated value
        DebounceTimer1 = tPDDebounceMin;                                        // Reset the debounce timer to the minimum tPDDebounce
        FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    }
    else if (DebounceTimer1 == 0)                                               // If the signal has been debounced
    {
        DebounceTimer1 = USHRT_MAX;                                             // Disable the debounce timer until we get a change
        CC1TermDeb = CC1TermAct;                                                // Store the debounced termination for CC1
    }
    if (CC1TermDeb == CCTypeNone)                                               // If we have detected an open for > tCCDebounce
        SetStateDelayUnattached();                                              // Go to the unattached state
}

/////////////////////////////////////////////////////////////////////////////
//                      State Machine Configuration
/////////////////////////////////////////////////////////////////////////////

void SetStateDisabled(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    Registers.Power.PWR = 0x01;                                     // Enter low power state
    Registers.Control.TOGGLE = 0;                                   // Disable the toggle state machine
    Registers.Control.HOST_CUR = 0x00;                              // Disable the currents for the pull-ups (not used for UFP)
    Registers.Switches.word = 0x0000;                               // Disable all pull-ups and pull-downs on the CC pins and disable the BMC transmitters
    FUSB300Write(regPower, 1, &Registers.Power.byte);               // Commit the power state
    FUSB300Write(regControl0, 3, &Registers.Control.byte[0]);       // Commit the control state
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
#ifdef PD_SUPPORT
    USBPDDisable(false);                                            // Disable the USB PD state machine (no need to write FUSB300 again since we are doing it here)
#endif
    CC1TermDeb = CCTypeNone;                                        // Clear the debounced CC1 state
    CC2TermDeb = CCTypeNone;                                        // Clear the debounced CC2 state
    CC1TermAct = CC1TermDeb;                                        // Clear the active CC1 state
    CC2TermAct = CC2TermDeb;                                        // Clear the active CC2 state
    blnCCPinIsCC1 = false;                                          // Clear the CC1 pin flag
    blnCCPinIsCC2 = false;                                          // Clear the CC2 pin flag
    ConnState = Disabled;                                           // Set the state machine variable to Disabled
    StateTimer = USHRT_MAX;                                         // Disable the state timer (not used in this state)
    DebounceTimer1 = USHRT_MAX;                                     // Disable the 1st level debounce timer
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debounce timer
    ToggleTimer = USHRT_MAX;                                        // Disable the toggle timer
    //wake_up_statemachine();
}

void SetStateErrorRecovery(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    Registers.Power.PWR = 0x01;                                     // Enter low power state
    Registers.Control.TOGGLE = 0;                                   // Disable the toggle state machine
    Registers.Control.HOST_CUR = 0x00;                              // Disable the currents for the pull-ups (not used for UFP)
    Registers.Switches.word = 0x0000;                               // Disable all pull-ups and pull-downs on the CC pins and disable the BMC transmitters
    FUSB300Write(regPower, 1, &Registers.Power.byte);               // Commit the power state
    FUSB300Write(regControl0, 3, &Registers.Control.byte[0]);       // Commit the control state
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
#ifdef PD_SUPPORT
    USBPDDisable(false);                                            // Disable the USB PD state machine (no need to write FUSB300 again since we are doing it here)
#endif
    CC1TermDeb = CCTypeNone;                                        // Clear the debounced CC1 state
    CC2TermDeb = CCTypeNone;                                        // Clear the debounced CC2 state
    CC1TermAct = CC1TermDeb;                                        // Clear the active CC1 state
    CC2TermAct = CC2TermDeb;                                        // Clear the active CC2 state
    blnCCPinIsCC1 = false;                                          // Clear the CC1 pin flag
    blnCCPinIsCC2 = false;                                          // Clear the CC2 pin flag
    ConnState = ErrorRecovery;                                      // Set the state machine variable to ErrorRecovery
    StateTimer = tErrorRecovery;                                    // Load the tErrorRecovery duration into the state transition timer
    FUSB302_start_timer(&state_hrtimer, StateTimer);
    DebounceTimer1 = USHRT_MAX;                                     // Disable the 1st level debounce timer
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debounce timer
    ToggleTimer = USHRT_MAX;                                        // Disable the toggle timer
    wake_up_statemachine();
}

void SetStateDelayUnattached(void)
{
    // This state is only here because of the precision timing source we have with the FPGA
    // We are trying to avoid having the toggle state machines in sync with each other
    // Causing the tDRPAdvert period to overlap causing the devices to not attach for a period of time
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    Registers.Power.PWR = 0x01;                                     // Enter low power state
    Registers.Control.TOGGLE = 0;                                   // Disable the toggle state machine
    Registers.Control.HOST_CUR = 0x00;                              // Disable the currents for the pull-ups (not used for UFP)
    Registers.Switches.word = 0x0000;                               // Disable all pull-ups and pull-downs on the CC pins and disable the BMC transmitters
    FUSB300Write(regPower, 1, &Registers.Power.byte);               // Commit the power state
    FUSB300Write(regControl0, 3, &Registers.Control.byte[0]);       // Commit the control state
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
#ifdef PD_SUPPORT
    USBPDDisable(false);                                            // Disable the USB PD state machine (no need to write FUSB300 again since we are doing it here)
#endif
    CC1TermDeb = CCTypeNone;                                        // Clear the debounced CC1 state
    CC2TermDeb = CCTypeNone;                                        // Clear the debounced CC2 state
    CC1TermAct = CC1TermDeb;                                        // Clear the active CC1 state
    CC2TermAct = CC2TermDeb;                                        // Clear the active CC2 state
    blnCCPinIsCC1 = false;                                          // Clear the CC1 pin flag
    blnCCPinIsCC2 = false;                                          // Clear the CC2 pin flag
    ConnState = DelayUnattached;                                    // Set the state machine variable to delayed unattached
    StateTimer = 10;                                       // Set the state timer to a random value to not synchronize the toggle start (use a multiple of RAND_MAX+1 as the modulus operator)
    FUSB302_start_timer(&state_hrtimer, StateTimer);
    DebounceTimer1 = USHRT_MAX;                                     // Disable the 1st level debounce timer
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debounce timer
    ToggleTimer = USHRT_MAX;                                        // Disable the toggle timer
    wake_up_statemachine();
}

void SetStateUnattached(void)
{
    // This function configures the Toggle state machine in the FUSB302 to handle all of the unattached states.
    // This allows for the MCU to be placed in a low power mode until the FUSB302 wakes it up upon detecting something
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    Registers.Control.HOST_CUR = 0x01;                              // Enable the defauult host current for the pull-ups (regardless of mode)
    Registers.Control.TOGGLE = 1;                                   // Enable the toggle
    if ((PortType == USBTypeC_DRP) || (blnAccSupport))              // If we are a DRP or supporting accessories
        Registers.Control.MODE = 0b01;                              // We need to enable the toggling functionality for Rp/Rd
    else if (PortType == USBTypeC_Source)                           // If we are strictly a Source
        Registers.Control.MODE = 0b11;                              // We just need to look for Rd
    else                                                            // Otherwise we are a UFP
        Registers.Control.MODE = 0b10;                              // So we need to only look for Rp
    Registers.Switches.word = 0x0003;                               // Enable the pull-downs on the CC pins, toggle overrides anyway
    Registers.Power.PWR = 0x07;                                     // Enable everything except internal oscillator
    Registers.Measure.MDAC = MDAC_2P05V;                            // Set up DAC threshold to 2.05V

    /* mask I_BC_LVL/COMP_CHNG to avoid C2A Cable generate INT*/
    Registers.Mask.M_BC_LVL = 1;
    Registers.Mask.M_COMP_CHNG = 1;
    FUSB300Write(regMask, 1, &Registers.Mask.byte);
    /*end*/

    FUSB300Write(regPower, 1, &Registers.Power.byte);               // Commit the power state
    FUSB300Write(regControl0, 3, &Registers.Control.byte[0]);       // Commit the control state
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
    FUSB300Write(regMeasure, 1, &Registers.Measure.byte);           // Commit the DAC threshold
#ifdef PD_SUPPORT
    USBPDDisable(false);                                            // Disable the USB PD state machine (no need to write FUSB300 again since we are doing it here)
#endif
    ConnState = Unattached;                                         // Set the state machine variable to unattached

    SinkCurrent = utccNone;
    CC1TermDeb = CCTypeNone;                                        // Clear the termination for this state
    CC2TermDeb = CCTypeNone;                                        // Clear the termination for this state
    CC1TermAct = CC1TermDeb;
    CC2TermAct = CC2TermDeb;
    blnCCPinIsCC1 = false;                                          // Clear the CC1 pin flag 
    blnCCPinIsCC2 = false;                                          // Clear the CC2 pin flag
    StateTimer = USHRT_MAX;                                         // Disable the state timer, not used in this state
    DebounceTimer1 = USHRT_MAX;                                     // Disable the 1st level debounce timer, not used in this state
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debounce timer, not used in this state
    ToggleTimer = USHRT_MAX;                                        // Disable the toggle timer, not used in this state
    wake_up_statemachine();
}

void SetStateAttachWaitSnk(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    Registers.Power.PWR = 0x07;                                     // Enable everything except internal oscillator
    Registers.Switches.word = 0x0003;                               // Enable the pull-downs on the CC pins
    if (blnCCPinIsCC1)
        Registers.Switches.MEAS_CC1 = 1;
    else
        Registers.Switches.MEAS_CC2 = 1;
    Registers.Measure.MDAC = MDAC_2P05V;                            // Set up DAC threshold to 2.05V
    Registers.Control.HOST_CUR = 0x00;                              // Disable the host current
    Registers.Control.TOGGLE = 0;                                   // Disable the toggle
    FUSB300Write(regPower, 1, &Registers.Power.byte);               // Commit the power state
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
    FUSB300Write(regMeasure, 1, &Registers.Measure.byte);           // Commit the DAC threshold
    FUSB300Write(regControl0, 3, &Registers.Control.byte[0]);       // Commit the host current
    Delay10us(25);                                                  // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the current state of the BC_LVL and COMP
    ConnState = AttachWaitSink;                                     // Set the state machine variable to AttachWait.Snk
    SinkCurrent = utccNone;                                         // Set the current advertisment variable to none until we determine what the current is
    if (Registers.Switches.MEAS_CC1)                                // If CC1 is what initially got us into the wait state...
    {
        CC1TermAct = DecodeCCTermination();                         // Determine what is initially on CC1
        CC2TermAct = CCTypeNone;                                    // Put something that we shouldn't see on the CC2 to force a debouncing
    }
    else
    {
        CC1TermAct = CCTypeNone;                                    // Put something that we shouldn't see on the CC1 to force a debouncing
        CC2TermAct = DecodeCCTermination();                         // Determine what is initially on CC2
    }
    CC1TermDeb = CCTypeNone;                                        // Initially set to invalid
    CC2TermDeb = CCTypeNone;                                        // Initially set to invalid
    StateTimer = USHRT_MAX;                                         // Disable the state timer, not used in this state
    DebounceTimer1 = tPDDebounceMax;                                // Set the tPDDebounce for validating signals to transition to
    FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debouncing until the first level has been debounced
    ToggleTimer = tFUSB302Toggle;                                   // Set the toggle timer to look at each pin for tFUSB302Toggle duration
    FUSB302_start_timer(&toggle_hrtimer,ToggleTimer);
    wake_up_statemachine();
}

void SetStateAttachWaitSrc(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    Registers.Power.PWR = 0x07;                                     // Enable everything except internal oscillator
    Registers.Switches.word = 0x0000;                               // Clear the register for the case below
    if (blnCCPinIsCC1)                                              // If we detected CC1 as an Rd
        Registers.Switches.word = 0x0044;                           // Enable CC1 pull-up and measure
    else
        Registers.Switches.word = 0x0088;                           // Enable CC2 pull-up and measure
    SourceCurrent = utccDefault;                                    // Set the default current level
    UpdateSourcePowerMode();                                        // Update the settings for the FUSB302
    Registers.Control.TOGGLE = 0;                                   // Disable the toggle
    FUSB300Write(regPower, 1, &Registers.Power.byte);               // Commit the power state
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
    FUSB300Write(regMeasure, 1, &Registers.Measure.byte);           // Commit the DAC threshold
    FUSB300Write(regControl2, 1, &Registers.Control.byte[2]);       // Commit the toggle
    Delay10us(25);                                                  // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the current state of the BC_LVL and COMP
    ConnState = AttachWaitSource;                                   // Set the state machine variable to AttachWait.Src
    SinkCurrent = utccNone;                                         // Not used in Src
    if (Registers.Switches.MEAS_CC1)                                // If CC1 is what initially got us into the wait state...
    {
        CC1TermAct = DecodeCCTermination();                         // Determine what is initially on CC1
        CC2TermAct = CCTypeNone;                                    // Assume that the initial value on CC2 is open
    }
    else
    {
        CC1TermAct = CCTypeNone;                                    // Assume that the initial value on CC1 is open
        CC2TermAct = DecodeCCTermination();                         // Determine what is initially on CC2
    }
    CC1TermDeb = CCTypeRa;                                          // Initially set both the debounced values to Ra to force the 2nd level debouncing
    CC2TermDeb = CCTypeRa;                                          // Initially set both the debounced values to Ra to force the 2nd level debouncing
    StateTimer = USHRT_MAX;                                         // Disable the state timer, not used in this state
    DebounceTimer1 = tPDDebounceMin;                                // Only debounce the lines for tPDDebounce so that we can debounce a detach condition
    FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debouncing initially to force completion of a 1st level debouncing
    ToggleTimer = tDRP;                                             // Set the initial toggle time to tDRP to ensure the other end sees the Rp
    FUSB302_start_timer(&toggle_hrtimer,ToggleTimer);
    wake_up_statemachine();
}

void SetStateAttachWaitAcc(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    Registers.Power.PWR = 0x07;                                     // Enable everything except internal oscillator
    Registers.Switches.word = 0x0044;                               // Enable CC1 pull-up and measure
    UpdateSourcePowerMode();
    Registers.Control.TOGGLE = 0;                                   // Disable the toggle
    FUSB300Write(regPower, 1, &Registers.Power.byte);               // Commit the power state
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
    FUSB300Write(regMeasure, 1, &Registers.Measure.byte);           // Commit the DAC threshold
    FUSB300Write(regControl2, 1, &Registers.Control.byte[2]);       // Commit the toggle
    Delay10us(25);                                                  // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the current state of the BC_LVL and COMP
    ConnState = AttachWaitAccessory;                                // Set the state machine variable to AttachWait.Accessory
    SinkCurrent = utccNone;                                         // Not used in accessories
    CC1TermAct = DecodeCCTermination();                             // Determine what is initially on CC1
    CC2TermAct = CCTypeNone;                                        // Assume that the initial value on CC2 is open
    CC1TermDeb = CCTypeNone;                                        // Initialize to open
    CC2TermDeb = CCTypeNone;                                        // Initialize to open
    StateTimer = USHRT_MAX;                                         // Disable the state timer, not used in this state
    DebounceTimer1 = tCCDebounceNom;                                // Once in this state, we are waiting for the lines to be stable for tCCDebounce before changing states
    FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debouncing initially to force completion of a 1st level debouncing
    ToggleTimer = tFUSB302Toggle;                                   // We're looking for the status of both lines of an accessory, no need to keep the line pull-ups on for tPDDebounce
    FUSB302_start_timer(&toggle_hrtimer,ToggleTimer);
    wake_up_statemachine();
}

void SetStateAttachedSrc(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 1;                                                 // Enable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    SourceCurrent = utccDefault;                                    // Reset the current to the default advertisement
    UpdateSourcePowerMode();                                        // Update the source power mode
    if (blnCCPinIsCC1 == true)                                      // If CC1 is detected as the CC pin...
        Registers.Switches.word = 0x0064;                           // Configure VCONN on CC2, pull-up on CC1, measure CC1
    else                                                            // Otherwise we are assuming CC2 is CC
        Registers.Switches.word = 0x0098;                           // Configure VCONN on CC1, pull-up on CC2, measure CC2
    Registers.Power.PWR = 0x07;                                     // Enable everything except internal oscillator
#ifdef PD_SUPPORT
    USBPDEnable(false, true);                                       // Enable the USB PD state machine if applicable (no need to write to FUSB300 again), set as DFP
#endif
    FUSB300Write(regPower, 1, &Registers.Power.byte);               // Commit the power state
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
    Delay10us(25);                                                  // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the current state of the BC_LVL and COMP
    // Maintain the existing CC term values from the wait state
    ConnState = AttachedSource;                                     // Set the state machine variable to Attached.Src
    SinkCurrent = utccNone;                                         // Set the Sink current to none (not used in source)
    StateTimer = USHRT_MAX;                                         // Disable the state timer, not used in this state
    DebounceTimer1 = tPDDebounceMin;                                // Set the debounce timer to tPDDebounceMin for detecting a detach
    FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debouncing, not needed in this state
    ToggleTimer = USHRT_MAX;                                        // Disable the toggle timer, not used in this state
    wake_up_statemachine();
}

void SetStateAttachedSink(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    Registers.Power.PWR = 0x07;                                     // Enable everything except internal oscillator
    Registers.Control.HOST_CUR = 0x00;                              // Disable the host current
    Registers.Measure.MDAC = MDAC_2P05V;                            // Set up DAC threshold to 2.05V
//    Registers.Switches.word = 0x0007;                               // Enable the pull-downs on the CC pins, measure CC1 and disable the BMC transmitters
    Registers.Switches.word = 0x0003;                               // Enable the pull-downs on the CC pins
    if (blnCCPinIsCC1)
        Registers.Switches.MEAS_CC1 = 1;
    else
        Registers.Switches.MEAS_CC2 = 1;
#ifdef PD_SUPPORT
    USBPDEnable(false, false);                                      // Enable the USB PD state machine (no need to write FUSB300 again since we are doing it here)
#endif
    FUSB300Write(regPower, 1, &Registers.Power.byte);               // Commit the power state
    FUSB300Write(regControl0, 1, &Registers.Control.byte[0]);       // Commit the host current
    FUSB300Write(regMeasure, 1, &Registers.Measure.byte);           // Commit the DAC threshold
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
    Delay10us(25);                                                  // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the current state of the BC_LVL and COMP
    ConnState = AttachedSink;                                       // Set the state machine variable to Attached.Sink
    SinkCurrent = utccDefault;                                      // Set the current advertisment variable to the default until we detect something different
    // Maintain the existing CC term values from the wait state
    StateTimer = USHRT_MAX;                                         // Disable the state timer, not used in this state
    DebounceTimer1 = tPDDebounceMin;                                // Set the debounce timer to tPDDebounceMin for detecting changes in advertised current
    FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debounce timer, not used in this state
    ToggleTimer = USHRT_MAX;                                        // Disable the toggle timer, not used in this state
    wake_up_statemachine();
}

void RoleSwapToAttachedSink(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    Registers.Control.HOST_CUR = 0x00;                              // Disable the host current
    Registers.Measure.MDAC = MDAC_2P05V;                            // Set up DAC threshold to 2.05V
    if (blnCCPinIsCC1)                                              // If the CC pin is CC1...
    {
        Registers.Switches.PU_EN1 = 0;                              // Disable the pull-up on CC1
        Registers.Switches.PDWN1 = 1;                               // Enable the pull-down on CC1
        // No change for CC2, it may be used as VCONN
        CC1TermAct = CCTypeRa;                                      // Initialize the CC term as open
        CC1TermDeb = CCTypeRa;                                      // Initialize the CC term as open
    }
    else
    {
        Registers.Switches.PU_EN2 = 0;                              // Disable the pull-up on CC2
        Registers.Switches.PDWN2 = 1;                               // Enable the pull-down on CC2
        // No change for CC1, it may be used as VCONN
        CC2TermAct = CCTypeRa;                                      // Initialize the CC term as open
        CC2TermDeb = CCTypeRa;                                      // Initialize the CC term as open
    }
    FUSB300Write(regControl0, 1, &Registers.Control.byte[0]);       // Commit the host current
    FUSB300Write(regMeasure, 1, &Registers.Measure.byte);           // Commit the DAC threshold
    FUSB300Write(regSwitches0, 1, &Registers.Switches.byte[0]);     // Commit the switch state
    Delay10us(25);                                                  // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the current state of the BC_LVL and COMP
    ConnState = AttachedSink;                                       // Set the state machine variable to Attached.Sink
    SinkCurrent = utccNone;                                         // Set the current advertisment variable to none until we determine what the current is
    StateTimer = USHRT_MAX;                                         // Disable the state timer, not used in this state
    DebounceTimer1 = tPDDebounceMin;                                // Set the debounce timer to tPDDebounceMin for detecting changes in advertised current
    FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debounce timer, not used in this state
    ToggleTimer = USHRT_MAX;                                        // Disable the toggle timer, not used in this state
    wake_up_statemachine();
}

void RoleSwapToAttachedSource(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 1;                                                 // Enable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    UpdateSourcePowerMode();                                        // Update the pull-up currents and measure block
    if (blnCCPinIsCC1)                                              // If the CC pin is CC1...
    {
        Registers.Switches.PU_EN1 = 1;                              // Enable the pull-up on CC1
        Registers.Switches.PDWN1 = 0;                               // Disable the pull-down on CC1
        // No change for CC2, it may be used as VCONN
        CC1TermAct = CCTypeNone;                                    // Initialize the CC term as open
        CC1TermDeb = CCTypeNone;                                    // Initialize the CC term as open
    }
    else
    {
        Registers.Switches.PU_EN2 = 1;                              // Enable the pull-up on CC2
        Registers.Switches.PDWN2 = 0;                               // Disable the pull-down on CC2
        // No change for CC1, it may be used as VCONN
        CC2TermAct = CCTypeNone;                                    // Initialize the CC term as open
        CC2TermDeb = CCTypeNone;                                    // Initialize the CC term as open
    }
    FUSB300Write(regSwitches0, 1, &Registers.Switches.byte[0]);     // Commit the switch state
    Delay10us(25);                                                  // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the current state of the BC_LVL and COMP
    ConnState = AttachedSource;                                     // Set the state machine variable to Attached.Src
    SinkCurrent = utccNone;                                         // Set the Sink current to none (not used in Src)
    StateTimer = USHRT_MAX;                                         // Disable the state timer, not used in this state
    DebounceTimer1 = tPDDebounceMin;                                // Set the debounce timer to tPDDebounceMin for detecting a detach
    FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debouncing, not needed in this state
    ToggleTimer = USHRT_MAX;                                        // Disable the toggle timer, not used in this state
    wake_up_statemachine();
}

void SetStateTryWaitSnk(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    Registers.Switches.word = 0x0007;                               // Enable the pull-downs on the CC pins and measure on CC1
    Registers.Power.PWR = 0x07;                                     // Enable everything except internal oscillator
    Registers.Measure.MDAC = MDAC_2P05V;                            // Set up DAC threshold to 2.05V
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
    FUSB300Write(regPower, 1, &Registers.Power.byte);               // Commit the power state
    FUSB300Write(regMeasure, 1, &Registers.Measure.byte);           // Commit the DAC threshold
    Delay10us(25);                                                  // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the current state of the BC_LVL and COMP
    ConnState = TryWaitSink;                                        // Set the state machine variable to TryWait.Snk
    SinkCurrent = utccNone;                                         // Set the current advertisment variable to none until we determine what the current is
    if (Registers.Switches.MEAS_CC1)
    {
        CC1TermAct = DecodeCCTermination();                         // Determine what is initially on CC1
        CC2TermAct = CCTypeNone;                                    // Assume that the initial value on CC2 is open
   }
    else
    {
        CC1TermAct = CCTypeNone ;                                   // Assume that the initial value on CC1 is open
        CC2TermAct = DecodeCCTermination();                         // Determine what is initially on CC2
    }
    CC1TermDeb = CCTypeNone;                                        // Initially set the debounced value to none so we don't immediately detach
    CC2TermDeb = CCTypeNone;                                        // Initially set the debounced value to none so we don't immediately detach
    StateTimer = tDRPTryWait;                                       // Set the state timer to tDRPTryWait to timeout if Rp isn't detected
    FUSB302_start_timer(&state_hrtimer,StateTimer);
    DebounceTimer1 = tPDDebounceMin;                                // The 1st level debouncing is based upon tPDDebounce
    FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debouncing initially until we validate the 1st level
    ToggleTimer = tFUSB302Toggle;                                   // Toggle the measure quickly (tFUSB302Toggle) to see if we detect an Rp on either
    FUSB302_start_timer(&toggle_hrtimer,ToggleTimer);
    wake_up_statemachine();
}

void SetStateTrySrc(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    SourceCurrent = utccDefault;                                    // Reset the current to the default advertisement
    Registers.Power.PWR = 0x07;                                     // Enable everything except internal oscillator
    Registers.Switches.word = 0x0000;                               // Disable everything (toggle overrides anyway)
    if (blnCCPinIsCC1)                                              // If we detected CC1 as an Rd
    {
        Registers.Switches.PU_EN1 = 1;                              // Enable the pull-up on CC1
        Registers.Switches.MEAS_CC1 = 1;                            // Measure on CC1
    }
    else
    {
        Registers.Switches.PU_EN2 = 1;                              // Enable the pull-up on CC1\2
        Registers.Switches.MEAS_CC2 = 1;                            // Measure on CC2
    }
    UpdateSourcePowerMode();
    FUSB300Write(regPower, 1, &Registers.Power.byte);               // Commit the power state
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
    Delay10us(25);                                                  // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the current state of the BC_LVL and COMP
    ConnState = TrySource;                                          // Set the state machine variable to Try.Src
    SinkCurrent = utccNone;                                         // Not used in Try.Src
    blnCCPinIsCC1 = false;                                          // Clear the CC1 is CC flag (don't know)
    blnCCPinIsCC2 = false;                                          // Clear the CC2 is CC flag (don't know)
    if (Registers.Switches.MEAS_CC1)
    {
        CC1TermAct = DecodeCCTermination();                         // Determine what is initially on CC1
        CC2TermAct = CCTypeNone;                                    // Assume that the initial value on CC2 is open
   }
    else
    {
        CC1TermAct = CCTypeNone;                                    // Assume that the initial value on CC1 is open
        CC2TermAct = DecodeCCTermination();                         // Determine what is initially on CC2
    }
    CC1TermDeb = CCTypeNone;                                        // Initially set the debounced value as open until we actually debounce the signal
    CC2TermDeb = CCTypeNone;                                        // Initially set both the active and debounce the same
    StateTimer = tDRPTry;                                           // Set the state timer to tDRPTry to timeout if Rd isn't detected
    FUSB302_start_timer(&state_hrtimer,StateTimer);
    DebounceTimer1 = tPDDebounceMin;                                // Debouncing is based soley off of tPDDebounce
    FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level since it's not needed
    ToggleTimer = tPDDebounceMax;                                   // Keep the pull-ups on for the max tPDDebounce to ensure that the other side acknowledges the pull-up
    FUSB302_start_timer(&toggle_hrtimer,ToggleTimer);
    wake_up_statemachine();
}

void SetStateDebugAccessory(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    Registers.Power.PWR = 0x07;                                     // Enable everything except internal oscillator
    Registers.Switches.word = 0x0044;                               // Enable CC1 pull-up and measure
    UpdateSourcePowerMode();
    FUSB300Write(regPower, 1, &Registers.Power.byte);               // Commit the power state
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
    FUSB300Write(regMeasure, 1, &Registers.Measure.byte);           // Commit the DAC threshold
    Delay10us(25);                                                  // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the current state of the BC_LVL and COMP
    ConnState = DebugAccessory;                                     // Set the state machine variable to Debug.Accessory
    SinkCurrent = utccNone;                                         // Not used in accessories
    // Maintain the existing CC term values from the wait state
    StateTimer = USHRT_MAX;                                         // Disable the state timer, not used in this state
    DebounceTimer1 = tCCDebounceNom;                                // Once in this state, we are waiting for the lines to be stable for tCCDebounce before changing states
    FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debouncing initially to force completion of a 1st level debouncing
    ToggleTimer = USHRT_MAX;                                        // Once we are in the debug.accessory state, we are going to stop toggling and only monitor CC1
    wake_up_statemachine();
}

void SetStateAudioAccessory(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    Registers.Power.PWR = 0x07;                                     // Enable everything except internal oscillator
    Registers.Switches.word = 0x0044;                               // Enable CC1 pull-up and measure
    UpdateSourcePowerMode();
    FUSB300Write(regPower, 1, &Registers.Power.byte);               // Commit the power state
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
    FUSB300Write(regMeasure, 1, &Registers.Measure.byte);           // Commit the DAC threshold
    Delay10us(25);                                                  // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the current state of the BC_LVL and COMP
    ConnState = AudioAccessory;                                     // Set the state machine variable to Audio.Accessory
    SinkCurrent = utccNone;                                         // Not used in accessories
    // Maintain the existing CC term values from the wait state
    StateTimer = USHRT_MAX;                                         // Disable the state timer, not used in this state
    DebounceTimer1 = tCCDebounceNom;                                // Once in this state, we are waiting for the lines to be stable for tCCDebounce before changing states
    FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debouncing initially to force completion of a 1st level debouncing
    ToggleTimer = USHRT_MAX;                                        // Once we are in the audio.accessory state, we are going to stop toggling and only monitor CC1
    wake_up_statemachine();
}

void SetStatePoweredAccessory(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    SourceCurrent = utcc1p5A;                                       // Have the option of 1.5A/3.0A for powered accessories, choosing 1.5A advert
    UpdateSourcePowerMode();                                        // Update the Source Power mode
    if (blnCCPinIsCC1 == true)                                      // If CC1 is detected as the CC pin...
        Registers.Switches.word = 0x0064;                           // Configure VCONN on CC2, pull-up on CC1, measure CC1
    else                                                            // Otherwise we are assuming CC2 is CC
        Registers.Switches.word = 0x0098;                           // Configure VCONN on CC1, pull-up on CC2, measure CC2
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
    Delay10us(25);                                                  // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the current state of the BC_LVL and COMP
    // Maintain the existing CC term values from the wait state
    // TODO: The line below will be uncommented once we have full support for VDM's and can enter an alternate mode as needed for Powered.Accessories
    // USBPDEnable(true, true);  
    ConnState = PoweredAccessory;                                   // Set the state machine variable to powered.accessory
    SinkCurrent = utccNone;                                         // Set the Sink current to none (not used in source)
    StateTimer = tAMETimeout;                                       // Set the state timer to tAMETimeout (need to enter alternate mode by this time)
    FUSB302_start_timer(&state_hrtimer,StateTimer);
    DebounceTimer1 = tPDDebounceMin;                                // Set the debounce timer to the minimum tPDDebounce to check for detaches
    FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debounce timer, not used in this state
    ToggleTimer = USHRT_MAX;                                        // Disable the toggle timer, only looking at the actual CC line
    wake_up_statemachine();
}

void SetStateUnsupportedAccessory(void)
{
    FUSB_LOG("enter:%s\n", __func__);	
    VBUS_5V_EN = 0;                                                 // Disable the 5V output...
    VBUS_12V_EN = 0;                                                // Disable the 12V output
    SourceCurrent = utccDefault;                                    // Reset the current to the default advertisement for this state
    UpdateSourcePowerMode();                                        // Update the Source Power mode
    Registers.Switches.VCONN_CC1 = 0;                               // Make sure VCONN is turned off
    Registers.Switches.VCONN_CC2 = 0;                               // Make sure VCONN is turned off
    FUSB300Write(regSwitches0, 2, &Registers.Switches.byte[0]);     // Commit the switch state
    Delay10us(25);                                                  // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);          // Read the current state of the BC_LVL and COMP
    ConnState = UnsupportedAccessory;                               // Set the state machine variable to unsupported.accessory
    SinkCurrent = utccNone;                                         // Set the Sink current to none (not used in source)
    StateTimer = USHRT_MAX;                                         // Disable the state timer, not used in this state
    DebounceTimer1 = tPDDebounceMin;                                // Set the debounce timer to the minimum tPDDebounce to check for detaches
    FUSB302_start_timer(&debounce_hrtimer1,DebounceTimer1);
    DebounceTimer2 = USHRT_MAX;                                     // Disable the 2nd level debounce timer, not used in this state
    ToggleTimer = USHRT_MAX;                                        // Disable the toggle timer, only looking at the actual CC line
    wake_up_statemachine();
}

void UpdateSourcePowerMode(void)
{
    switch(SourceCurrent)
    {
        case utccDefault:
            Registers.Measure.MDAC = MDAC_1P6V;                     // Set up DAC threshold to 1.6V (default USB current advertisement)
            Registers.Control.HOST_CUR = 0x01;                      // Set the host current to reflect the default USB power
            break;
        case utcc1p5A:
            Registers.Measure.MDAC = MDAC_1P6V;                     // Set up DAC threshold to 1.6V
            Registers.Control.HOST_CUR = 0x02;                      // Set the host current to reflect 1.5A
            break;
        case utcc3p0A:
            Registers.Measure.MDAC = MDAC_2P6V;                     // Set up DAC threshold to 2.6V
            Registers.Control.HOST_CUR = 0x03;                      // Set the host current to reflect 3.0A
            break;
        default:                                                    // This assumes that there is no current being advertised
            Registers.Measure.MDAC = MDAC_1P6V;                     // Set up DAC threshold to 1.6V (default USB current advertisement)
            Registers.Control.HOST_CUR = 0x00;                      // Set the host current to disabled
            break;
    }
    FUSB300Write(regMeasure, 1, &Registers.Measure.byte);           // Commit the DAC threshold
    FUSB300Write(regControl0, 1, &Registers.Control.byte[0]);       // Commit the host current
}

/////////////////////////////////////////////////////////////////////////////
//                        Type C Support Routines
/////////////////////////////////////////////////////////////////////////////

void ToggleMeasureCC1(void)
{
    Registers.Switches.PU_EN1 = Registers.Switches.PU_EN2;                  // If the pull-up was enabled on CC2, enable it for CC1
    Registers.Switches.PU_EN2 = 0;                                          // Disable the pull-up on CC2 regardless, since we aren't measuring CC2 (prevent short)
    Registers.Switches.MEAS_CC1 = 1;                                        // Set CC1 to measure
    Registers.Switches.MEAS_CC2 = 0;                                        // Clear CC2 from measuring
    FUSB300Write(regSwitches0, 1, &Registers.Switches.byte[0]);             // Set the switch to measure
    Delay10us(25);                                                          // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);                  // Read back the status to get the current COMP and BC_LVL
}

void ToggleMeasureCC2(void)
{
    Registers.Switches.PU_EN2 = Registers.Switches.PU_EN1;                  // If the pull-up was enabled on CC1, enable it for CC2
    Registers.Switches.PU_EN1 = 0;                                          // Disable the pull-up on CC1 regardless, since we aren't measuring CC1 (prevent short)
    Registers.Switches.MEAS_CC1 = 0;                                        // Clear CC1 from measuring
    Registers.Switches.MEAS_CC2 = 1;                                        // Set CC2 to measure
    FUSB300Write(regSwitches0, 1, &Registers.Switches.byte[0]);             // Set the switch to measure
    Delay10us(25);                                                          // Delay the reading of the COMP and BC_LVL to allow time for settling
    FUSB300Read(regStatus0, 2, &Registers.Status.byte[4]);                  // Read back the status to get the current COMP and BC_LVL
 }

CCTermType DecodeCCTermination(void)
{
    CCTermType Termination = CCTypeNone;            // By default set it to nothing
    if (Registers.Status.COMP == 0)                 // If COMP is high, the BC_LVL's don't matter
    {
        switch (Registers.Status.BC_LVL)            // Determine which level
        {
            case 0b00:                              // If BC_LVL is lowest... it's an vRa
                Termination = CCTypeRa;
                break;
            case 0b01:                              // If BC_LVL is 1, it's default
                Termination = CCTypeRdUSB;
                break;
            case 0b10:                              // If BC_LVL is 2, it's vRd1p5
                Termination = CCTypeRd1p5;
                break;
            default:                                // Otherwise it's vRd3p0
                Termination = CCTypeRd3p0;
                break;
        }
    }
    return Termination;                             // Return the termination type
}

void UpdateSinkCurrent(CCTermType Termination)
{
    switch (Termination)
    {
        case CCTypeRdUSB:                       // If we detect the default...
        case CCTypeRa:                          // Or detect an accessory (vRa)
            SinkCurrent = utccDefault;
            break;
        case CCTypeRd1p5:                       // If we detect 1.5A
            SinkCurrent = utcc1p5A;
            break;
        case CCTypeRd3p0:                       // If we detect 3.0A
            SinkCurrent = utcc3p0A;
            break;
        default:
            SinkCurrent = utccNone;
            break;
    }
}

/////////////////////////////////////////////////////////////////////////////
//                     Externally Accessible Routines
/////////////////////////////////////////////////////////////////////////////

void ConfigurePortType(unsigned char Control)
{
    unsigned char value;
    DisableFUSB300StateMachine();
    value = Control & 0x03;
    switch (value)
    {
        case 1:
            PortType = USBTypeC_Source;
            break;
        case 2:
            PortType = USBTypeC_DRP;
            break;
        default:
            PortType = USBTypeC_Sink;
            break;
    }
    if (Control & 0x04)
        blnAccSupport = true;
    else
        blnAccSupport = false;
    if (Control & 0x08)
        blnSrcPreferred = true;
    else
        blnSrcPreferred = false;
    value = (Control & 0x30) >> 4;
    switch (value)
    {
        case 1:
            SourceCurrent = utccDefault;
            break;
        case 2:
            SourceCurrent = utcc1p5A;
            break;
        case 3:
            SourceCurrent = utcc3p0A;
            break;
        default:
            SourceCurrent = utccNone;
            break;
    }
    if (Control & 0x80)
        EnableFUSB300StateMachine();
}

void UpdateCurrentAdvert(unsigned char Current)
{
    switch (Current)
    {
        case 1:
            SourceCurrent = utccDefault;
            break;
        case 2:
            SourceCurrent = utcc1p5A;
            break;
        case 3:
            SourceCurrent = utcc3p0A;
            break;
        default:
            SourceCurrent = utccNone;
            break;
    }
    if (ConnState == AttachedSource)
        UpdateSourcePowerMode();
}

void GetFUSB300TypeCStatus(unsigned char abytData[])
{
    int intIndex = 0;
    abytData[intIndex++] = GetTypeCSMControl();     // Grab a snapshot of the top level control
    abytData[intIndex++] = ConnState & 0xFF;        // Get the current state
    abytData[intIndex++] = GetCCTermination();      // Get the current CC termination
    abytData[intIndex++] = SinkCurrent;             // Set the sink current capability detected
}

unsigned char GetTypeCSMControl(void)
{
    unsigned char status = 0;
    status |= (PortType & 0x03);            // Set the type of port that we are configured as
    switch(PortType)                        // Set the port type that we are configured as
    {
        case USBTypeC_Source:
            status |= 0x01;                 // Set Source type
            break;
        case USBTypeC_DRP:
            status |= 0x02;                 // Set DRP type
            break;
        default:                            // If we are not DRP or Source, we are Sink which is a value of zero as initialized
            break;
    }
    if (blnAccSupport)                      // Set the flag if we support accessories 
        status |= 0x04;
    if (blnSrcPreferred)                    // Set the flag if we prefer Source mode (as a DRP)
        status |= 0x08;
    status |= (SourceCurrent << 4);
    if (blnSMEnabled)                       // Set the flag if the state machine is enabled
        status |= 0x80;
    return status;
}

unsigned char GetCCTermination(void)
{
    unsigned char status = 0;
    status |= (CC1TermDeb & 0x07);          // Set the current CC1 termination
//    if (blnCC1Debounced)                    // Set the flag if the CC1 pin has been debounced
//        status |= 0x08;
    status |= ((CC2TermDeb & 0x07) << 4);   // Set the current CC2 termination
//    if (blnCC2Debounced)                    // Set the flag if the CC2 pin has been debounced
//        status |= 0x80;
    return status;
}

/* /////////////////////////////////////////////////////////////////////////////// */
int register_typec_switch_callback(struct typec_switch_data *new_driver)
{
	FUSB_LOG("Register driver %s %d\n", new_driver->name, new_driver->type);

	if (new_driver->type == DEVICE_TYPE) {
		fusb_i2c_data->device_driver = new_driver;
		fusb_i2c_data->device_driver->on = 0;
		return 0;
	}

	if (new_driver->type == HOST_TYPE) {
		fusb_i2c_data->host_driver = new_driver;
		fusb_i2c_data->host_driver->on = 0;
		if (ConnState == AttachedSource)
			trigger_driver(fusb_i2c_data, HOST_TYPE, ENABLE, DONT_CARE);
		return 0;
	}

	return -1;
}
EXPORT_SYMBOL_GPL(register_typec_switch_callback);

int unregister_typec_switch_callback(struct typec_switch_data *new_driver)
{
	FUSB_LOG("Unregister driver %s %d\n", new_driver->name, new_driver->type);

	if ((new_driver->type == DEVICE_TYPE) && (fusb_i2c_data->device_driver == new_driver))
		fusb_i2c_data->device_driver = NULL;

	if ((new_driver->type == HOST_TYPE) && (fusb_i2c_data->host_driver == new_driver))
		fusb_i2c_data->host_driver = NULL;

	return 0;
}
EXPORT_SYMBOL_GPL(unregister_typec_switch_callback);

static irqreturn_t cc_eint_interrupt_handler(int irq, void *data)
{
    FUSB_LOG("%s\n", __func__);
    wake_up_statemachine();

    return IRQ_HANDLED;
}

static bool check_i2c_bus_suspended(void)
{
	struct i2c_adapter *adapter = fusb_i2c_data->client->adapter;
	struct device *dev = &adapter->dev;

	return dev->power.is_suspended;
}

int fusb302_state_kthread(void *x)
{
    long rc = 0;

    FUSB_LOG("***********enter fusb302 state thread!!1 ********************\n");

    while (!kthread_should_stop()) {
	// check adapter active
    	if (check_i2c_bus_suspended()) {
		pr_info_ratelimited("%s: waitting i2c core resume...\n", __func__);
		msleep(10);
		continue;
	}

    	// maybe we will miss state change or irq falling
    	if (state_changed) {
		state_changed = false;
		StateMachineFUSB300();
		continue;
	}
    	if (!FUSB300Int_PIN_LVL()) {
		StateMachineFUSB300();
		continue;
	}

	rc = wait_for_completion_interruptible_timeout(&state_notifier, 5*HZ);
	reinit_completion(&state_notifier);
	if (rc <= 0 || state_changed == false)
		continue;
	state_changed = false;

        StateMachineFUSB300();
    }
    return 0;
}
    
static void fusb302_device_check(void)
{
    FUSB300Read(regDeviceID, 2, &Registers.DeviceID.byte);
    FUSB_LOG("device id:%2x\n", Registers.DeviceID.byte);
}

static ssize_t fusb302_reg_dump(struct device *dev,struct device_attribute *attr, char *buf)
{
    int reg_addr[] = {regDeviceID, regSwitches0, regSwitches1, regMeasure, regSlice,
        regControl0, regControl1, regControl2, regControl3, regMask, regPower, regReset, 
        regOCPreg, regMaska, regMaskb, regControl4, regStatus0a, regStatus1a, regInterrupta,
        regInterruptb, regStatus0, regStatus1, regInterrupt };

    int i, len = 0; 
    uint8_t byte = 0;
    for (i=0; i< sizeof(reg_addr)/sizeof(reg_addr[0]); i++)
    {
        FUSB300Read(reg_addr[i], 1, &byte);
        len += sprintf(buf+len, "R%02xH:%02x ", reg_addr[i], byte);
        if (((i+1)%6)==0)
            len += sprintf(buf+len, "\n");
    }
    return len;
}

static ssize_t fusb302_reg_set(struct device *dev,struct device_attribute *attr, const char *buf, size_t size)
{
    return size;
}

static DEVICE_ATTR(reg_dump, 0660, fusb302_reg_dump, fusb302_reg_set);


static ssize_t fusb302_state(struct device *dev, struct device_attribute *attr, char *buf)
{
     char data[5];
     GetFUSB300TypeCStatus(data);
     return sprintf(buf, "SMC=%2x, connState=%2d, cc=%2x, current=%d, debug=%d\n", data[0], data[1], data[2], data[3], fusb302_debug);
}

static ssize_t fusb302_set_debug(struct device *dev,struct device_attribute *attr, const char *buf, size_t size)
{

	if (strstr(buf, "disable"))
		fusb302_debug = false;
	else
		fusb302_debug = true;

	return size;
}

static DEVICE_ATTR(state, 0660, fusb302_state, fusb302_set_debug);


extern uint32_t SinkRequestMaxVoltage;
extern void set_policy_state(PolicyState_t st);

void swith_req_voltage(uint32_t vol)
{
	SinkRequestMaxVoltage = vol;
	set_policy_state(peSinkEvaluateCaps);
	wake_up_statemachine();
}

static ssize_t fusb302_get_max_vol(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "SinkRequestMaxVoltage:%d\n", SinkRequestMaxVoltage);
}

static ssize_t fusb302_set_max_vol(struct device *dev,struct device_attribute *attr, const char *buf, size_t size)
{
	int max_vol = 0;

	if (strstr(buf, "5"))
		max_vol = 100; //(5V)
	else if (strstr(buf, "12"))
		max_vol = 240; //(12)

	if (!max_vol)
		swith_req_voltage(max_vol);

	return size;
}

static DEVICE_ATTR(max_vol, 0660, fusb302_get_max_vol, fusb302_set_max_vol);

/*(sel=0)=SW1=(gpio=0), (sel=1)=SW2=(gpio=1)*/
static int usb3_switch_sel(struct fusb302_i2c_data *fusb, int sel)
{
	int retval = 0;

	if (!fusb->pinctrl || !fusb->pin_cfg) {
		FUSB_LOG("%s not init\n", __func__);
		goto end;
	}

	FUSB_LOG("%s on=%d\n", __func__, sel);

	if (sel == UP_SIDE) /*select SW1 */
		pinctrl_select_state(fusb->pinctrl,
				fusb->pin_cfg->fusb340_sel_low);
	else if (sel == DOWN_SIDE) /*select SW2 */
		pinctrl_select_state(fusb->pinctrl,
				fusb->pin_cfg->fusb340_sel_high);

end:
	return retval;
}

static int trigger_driver(struct fusb302_i2c_data *fusb, int type, int stat, int dir)
{
	FUSB_LOG("trigger_driver: type:%d, stat:%d, dir%d\n", type, stat, dir);

	/*
	 * In order to save power, we disable usb 3.0 redriver & switch by
	 * default.
	 */
	if (stat == ENABLE) {
		usb3_switch_sel(fusb, dir);
		if (type == DEVICE_TYPE)
			pinctrl_select_state(fusb->pinctrl,
					fusb->pin_cfg->fusb340_usb18_high);
	} else {
		/*
		 * TUSB542 have some current backflow if we disable usb18
		 * directly, and the switch gpio should pull down at the same
		 * time.
		 */
		usb3_switch_sel(fusb, UP_SIDE);
		pinctrl_select_state(fusb->pinctrl,
				fusb->pin_cfg->fusb340_usb18_low);
	}

	if (type == DEVICE_TYPE && fusb->device_driver) {
		if ((stat == DISABLE) && (fusb->device_driver->disable)
		    && (fusb->device_driver->on == ENABLE)) {
			fusb->device_driver->disable(fusb->device_driver->priv_data);
			fusb->device_driver->on = DISABLE;

			FUSB_LOG("trigger_driver: disable dev drv\n");
		} else if ((stat == ENABLE) && (fusb->device_driver->enable)
			   && (fusb->device_driver->on == DISABLE)) {
			fusb->device_driver->enable(fusb->device_driver->priv_data);
			fusb->device_driver->on = ENABLE;

			FUSB_LOG("trigger_driver: enable dev drv\n");
		} else {
			FUSB_LOG("%s No device driver to enable\n", __func__);
		}
	} else if (type == HOST_TYPE && fusb->host_driver) {
		if ((stat == DISABLE) && (fusb->host_driver->disable)
		    && (fusb->host_driver->on == ENABLE)) {
			fusb->host_driver->disable(fusb->host_driver->priv_data);
			fusb->host_driver->on = DISABLE;

			FUSB_LOG("trigger_driver: disable host drv\n");
		} else if ((stat == ENABLE) &&
			   (fusb->host_driver->enable) && (fusb->host_driver->on == DISABLE)) {
			fusb->host_driver->enable(fusb->host_driver->priv_data);
			fusb->host_driver->on = ENABLE;

			FUSB_LOG("trigger_driver: enable host drv\n");
		} else {
			FUSB_LOG("%s No device driver to enable\n", __func__);
		}
	} else {
		FUSB_LOG("trigger_driver: no callback func\n");
	}

	return 0;
}

static int fusb302_probe(struct i2c_client *i2c, const struct i2c_device_id *id)
{
    int ret = 0;
    struct fusb302_i2c_data *fusb;
    int ret_device_file = 0;
    unsigned char reset = 0x01;
    u32 ints[2] = { 0, 0 };
    struct device_node *node;
    unsigned int debounce, gpiopin;

    FUSB_LOG("enter probe\n");

    if (!fusb_i2c_data)
	fusb_i2c_data = kzalloc(sizeof(struct fusb302_i2c_data), GFP_KERNEL);
    if (!fusb_i2c_data) {
        dev_err(&i2c->dev, "private data alloc fail\n");
        goto exit;
    }
    fusb = fusb_i2c_data;

    InitializeFUSB300Variables();
#ifdef PD_SUPPORT
    InitializeUSBPDVariables();
#endif
    
    i2c_set_clientdata(i2c, fusb);
    fusb->client = i2c;
    fusb->dev = &i2c->dev;

    ret_device_file = device_create_file(&(i2c->dev), &dev_attr_reg_dump);
    ret_device_file = device_create_file(&(i2c->dev), &dev_attr_state);
    ret_device_file = device_create_file(&(i2c->dev), &dev_attr_max_vol);

    fusb302_device_check();
    //Initialize FUSB302 
    FUSB300Write(regReset, 1, &reset);
    InitializeFUSB300();

    node = of_find_compatible_node(NULL, NULL, "mediatek,fusb300-eint");
    if (node) {
	    of_property_read_u32_array(node, "debounce", ints, ARRAY_SIZE(ints));
	    debounce = ints[1];
	    gpiopin = ints[0];

	    gpio_set_debounce(gpiopin, debounce);

	    fusb->gpio_int_ccd = gpiopin;
	    fusb->gpio_pd_on = 1;
	    fusb->irq_ccd = irq_of_parse_and_map(node, 0);

	    ret = request_irq(fusb->irq_ccd, cc_eint_interrupt_handler,
			    IRQF_TRIGGER_FALLING, "typec-fusb302", fusb);
	    enable_irq_wake(fusb->irq_ccd);
    } else {
	    FUSB_LOG("can not find DT data, abort\n");
	    goto exit;
    }

    //create kernel run
    fusb->thread = kthread_run(fusb302_state_kthread, NULL, "fusb302_state_kthread");

    device_init_wakeup(fusb->dev,true);
    
    FUSB_LOG("probe successfully!\n");
    return 0;

exit:
    return -1;
}

static int fusb302_remove(struct i2c_client *i2c)
{
//        i2c_unregister_device(i2c);
        kfree(i2c_get_clientdata(i2c));
        return 0;
}

static int fusb302_suspend(struct device *dev)
{
        //wait to do something
        return 0;
}
static int fusb302_resume(struct device *dev)
{
        //wait to do something
        return 0;
}

int fusb300_get_cable_capacity(void)
{
	u8 val;

#define STATUS0_CABLE_BC_LVL	3
	FUSB300Read(regStatus0, 1, &val);

	return val & STATUS0_CABLE_BC_LVL;
}
EXPORT_SYMBOL(fusb300_get_cable_capacity);

static int usbc_pinctrl_probe(struct platform_device *pdev)
{
	int retval = 0;
	struct fusb302_i2c_data *fusb;

	if (!fusb_i2c_data)
		fusb_i2c_data = kzalloc(sizeof(struct fusb302_i2c_data), GFP_KERNEL);

	fusb = fusb_i2c_data;

	fusb->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(fusb->pinctrl)) {
		FUSB_LOG("Cannot find usb pinctrl!\n");
	} else {
		fusb->pin_cfg = kzalloc(sizeof(struct usbc_pin_ctrl), GFP_KERNEL);

		FUSB_LOG("pinctrl=%p\n", fusb->pinctrl);

		fusb->pin_cfg->fusb340_sel_init = pinctrl_lookup_state(fusb->pinctrl, "fusb340_sel_init");
		if (IS_ERR(fusb->pin_cfg->fusb340_sel_init))
			FUSB_LOG("Can *NOT* find fusb340_sel_init\n");
		else
			FUSB_LOG("Find fusb340_sel_init\n");

		fusb->pin_cfg->fusb340_sel_low = pinctrl_lookup_state(fusb->pinctrl, "fusb340_sel_low");
		if (IS_ERR(fusb->pin_cfg->fusb340_sel_low))
			FUSB_LOG("Can *NOT* find fusb340_sel_low\n");
		else
			FUSB_LOG("Find fusb340_sel_low\n");

		fusb->pin_cfg->fusb340_sel_high = pinctrl_lookup_state(fusb->pinctrl, "fusb340_sel_high");
		if (IS_ERR(fusb->pin_cfg->fusb340_sel_high))
			FUSB_LOG("Can *NOT* find fusb340_sel_high\n");
		else
			FUSB_LOG("Find fusb340_sel_high\n");
		/********************************************************/
		fusb->pin_cfg->fusb340_usb18_init = pinctrl_lookup_state(fusb->pinctrl, "power_en_usb18_init");
		if (IS_ERR(fusb->pin_cfg->fusb340_usb18_init))
			FUSB_LOG("Can *NOT* find power_en_usb18_init\n");
		else
			FUSB_LOG("Find power_en_usb18_init\n");

		fusb->pin_cfg->fusb340_usb18_low = pinctrl_lookup_state(fusb->pinctrl, "power_en_usb18_low");
		if (IS_ERR(fusb->pin_cfg->fusb340_usb18_low))
			FUSB_LOG("Can *NOT* find power_en_usb18_low\n");
		else
			FUSB_LOG("Find power_en_usb18_low\n");

		fusb->pin_cfg->fusb340_usb18_high = pinctrl_lookup_state(fusb->pinctrl, "power_en_usb18_high");
		if (IS_ERR(fusb->pin_cfg->fusb340_usb18_high))
			FUSB_LOG("Can *NOT* find power_en_usb18_init\n");
		else
			FUSB_LOG("Find power_en_usb18_init\n");
		/********************************************************/

		// disable usb18 by default
		pinctrl_select_state(fusb->pinctrl,
				fusb->pin_cfg->fusb340_usb18_low);
		/*dir selection */
		pinctrl_select_state(fusb->pinctrl, fusb->pin_cfg->fusb340_sel_init);
		FUSB_LOG("Finish parsing pinctrl\n");
	}

	return retval;
}

static int usbc_pinctrl_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct dev_pm_ops fusb302_pm_ops = {
	.suspend	= fusb302_suspend,
	.resume		= fusb302_resume,
};

static const struct of_device_id usb_pinctrl_ids[] = {
	{.compatible = "mediatek,usb_c_pinctrl",},
	{},
};

static struct platform_driver usbc_pinctrl_driver = {
	.probe = usbc_pinctrl_probe,
	.remove = usbc_pinctrl_remove,
	.driver = {
		.name = "usbc_pinctrl",
#ifdef CONFIG_OF
		.of_match_table = usb_pinctrl_ids,
#endif
	},
};

#define FUSB302_NAME "FUSB302"

static const struct i2c_device_id usb_i2c_id[] = {
		{FUSB302_NAME, 0},
		{}
	};

#ifdef CONFIG_OF
static const struct of_device_id fusb302_of_match[] = {
		{.compatible = "mediatek,usb_type_c"},
		{},
	};
#endif

struct i2c_driver usb_i2c_driver = {
	.probe = fusb302_probe,
	.remove = fusb302_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = FUSB302_NAME,
		.pm = &fusb302_pm_ops,
#ifdef CONFIG_OF
		.of_match_table = fusb302_of_match,
#endif
	},
	.id_table = usb_i2c_id,
};

static int __init fusb300_init(void)
{
	int ret = 0;

	if (i2c_add_driver(&usb_i2c_driver) != 0) {
		FUSB_LOG("fusb300_init initialization failed!!\n");
		ret = -1;
	} else {
		FUSB_LOG("fusb300_init initialization succeed!!\n");
		if (!platform_driver_register(&usbc_pinctrl_driver))
			FUSB_LOG("register usbc pinctrl succeed!!\n");
		else {
			FUSB_LOG("register usbc pinctrl fail!!\n");
			ret = -1;
		}
	}
	return ret;
}

static void __exit fusb300_exit(void)
{

}
fs_initcall(fusb300_init);
/* module_exit(fusb300_exit); */
