/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <IOKit/assert.h>
#include <IOKit/IOLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hidsystem/IOHIDTypes.h>
#include <IOKit/hidsystem/IOLLEvent.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include "GenericPS2Keyboard.h"
#include "ApplePS2KeyboardDevice.h"
#include "ApplePS2ToADBMap.h"

// =============================================================================
// GenericPS2Keyboard Class Implementation
//

#define super IOHIKeyboard
#define APPLEPS2KEYBOARD_DEVICE_TYPE	0x1B

// I just made these up (after checking they weren't in the main map).
#define kSpecialPrevious 0xa1
#define kSpecialPlay 0xa2
#define kSpecialNext 0xa3

#define kRemappedDeadKey 0xa4

OSDefineMetaClassAndStructors(GenericPS2Keyboard, IOHIKeyboard);

UInt32 GenericPS2Keyboard::deviceType()  { return APPLEPS2KEYBOARD_DEVICE_TYPE; };
UInt32 GenericPS2Keyboard::interfaceID() { return NX_EVS_DEVICE_INTERFACE_ACE; };

UInt32 GenericPS2Keyboard::maxKeyCodes() { return KBV_NUM_KEYCODES; };

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool GenericPS2Keyboard::init(OSDictionary * properties)
{
    //
    // Initialize this object's minimal state.  This is invoked right after this
    // object is instantiated.
    //
    
    if (!super::init(properties))  return false;
    
    _device                    = 0;
    _extendCount               = 0;
    _interruptHandlerInstalled = false;
    _ledState                  = 0;
    
    for (int index = 0; index < KBV_NUNITS; index++)  _keyBitVector[index] = 0;
    
    return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

GenericPS2Keyboard * GenericPS2Keyboard::probe(IOService * provider, SInt32 * score)
{
    //
    // The driver has been instructed to verify the presence of the actual
    // hardware we represent. We are guaranteed by the controller that the
    // keyboard clock is enabled and the keyboard itself is disabled (thus
    // it won't send any asynchronous scan codes that may mess up the
    // responses expected by the commands we send it).  This is invoked
    // after the init.
    //
    
    ApplePS2KeyboardDevice * device  = (ApplePS2KeyboardDevice *)provider;
    PS2Request *             request = device->allocateRequest();
    bool                     success;
    
    if (!super::probe(provider, score))  return 0;
    
    //
    // Check to see if the keyboard responds to a basic diagnostic echo.
    //
    
    // (diagnostic echo command)
    request->commands[0].command = kPS2C_WriteDataPort;
    request->commands[0].inOrOut = kDP_TestKeyboardEcho;
    request->commands[1].command = kPS2C_ReadDataPortAndCompare;
    request->commands[1].inOrOut = 0xEE;
    request->commandsCount = 2;
    device->submitRequestAndBlock(request);
    
    // (free the request)
    success = (request->commandsCount == 2);
    device->freeRequest(request);
    
    return (success) ? this : 0;
}

IOReturn GenericPS2Keyboard::setProperties(OSObject * properties) {
    super::setProperties(properties);
    setProperty(kIOHIDVendorIDKey, OSNumber::withNumber((unsigned long long) 0, 16));
    setProperty(kIOHIDProductIDKey, OSNumber::withNumber((unsigned long long) 0, 16));
    setProperty(kIOHIDManufacturerKey, OSString::withCString("Generic"));
    setProperty(kIOHIDProductKey, OSString::withCString("Generic PS/2 Keyboard"));
    return kIOReturnSuccess;
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool GenericPS2Keyboard::start(IOService * provider)
{
    IOLog("GenericPS2keyboard is starting\n");
    //
    // The driver has been instructed to start.   This is called after a
    // successful attach.
    //
    
    if (!super::start(provider))  return false;
    
    //
    // Maintain a pointer to and retain the provider object.
    //
    
    _device = (ApplePS2KeyboardDevice *)provider;
    _device->retain();
    
    //
    // Fetch some re-mapping settings.
    //
    _capslockKeyCode = OSDynamicCast(OSNumber, getProperty("Map capslock to keycode"))->unsigned32BitValue();
    _windowsAltSwap = (kOSBooleanTrue == getProperty("Swap alt and windows key"));
    _remapFunctionKeys = (kOSBooleanTrue == getProperty("Remap function keys"));
    
    // Keep track of these to emulate 'fn' keys.
    _insertKeyDown = false;
    _applicationKeyDown = false;
    
    //
    // Install our driver's interrupt handler, for asynchronous data delivery.
    //
    _device->installInterruptAction(this,
                                    OSMemberFunctionCast(PS2InterruptAction, this, &GenericPS2Keyboard::interruptOccurred));
    _interruptHandlerInstalled = true;
    
    //
    // Initialize the keyboard LED state.
    //
    
    setLEDs(_ledState);
    
    //
    // Enable the keyboard clock (should already be so), the keyboard IRQ line,
    // and the keyboard Kscan -> scan code translation mode.
    //
    
    setCommandByte(kCB_EnableKeyboardIRQ | kCB_TranslateMode,
                   kCB_DisableKeyboardClock);
    
    //
    // Finally, we enable the keyboard itself, so that it may start reporting
    // key events.
    //
    setKeyboardEnable(true);
    
    //
    // Install our power control handler.
    //
    
    _device->installPowerControlAction( this,
                                       OSMemberFunctionCast(PS2PowerControlAction, this, &GenericPS2Keyboard::setDevicePowerState ));
    _powerControlHandlerInstalled = true;
    IOLog("GenericPS2Keyboard started.\n");
    return true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void GenericPS2Keyboard::stop(IOService * provider)
{
    IOLog("GenericPS2Keyboard stopping.\n");
    //
    // The driver has been instructed to stop.  Note that we must break all
    // connections to other service objects now (ie. no registered actions,
    // no pointers and retains to objects, etc), if any.
    //
    
    assert(_device == provider);
    
    //
    // Disable the keyboard itself, so that it may stop reporting key events.
    //
    
    setKeyboardEnable(false);
    
    //
    // Disable the keyboard clock and the keyboard IRQ line.
    //
    
    setCommandByte(kCB_DisableKeyboardClock, kCB_EnableKeyboardIRQ);
    
    //
    // Uninstall the interrupt handler.
    //
    
    if ( _interruptHandlerInstalled )  _device->uninstallInterruptAction();
    _interruptHandlerInstalled = false;
    
    //
    // Uninstall the power control handler.
    //
    
    if ( _powerControlHandlerInstalled ) _device->uninstallPowerControlAction();
    _powerControlHandlerInstalled = false;
    
    //
    // Release the pointer to the provider object.
    //
    
    _device->release();
    _device = 0;
    
    super::stop(provider);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void GenericPS2Keyboard::interruptOccurred(UInt8 scanCode)   // PS2InterruptAction
{
    //
    // This will be invoked automatically from our device when asynchronous
    // keyboard data needs to be delivered.  Process the keyboard data.  Do
    // NOT send any BLOCKING commands to our device in this context.
    //
    
    if (scanCode == kSC_Acknowledge)
        IOLog("%s: Unexpected acknowledge from PS/2 controller.\n", getName());
    else if (scanCode == kSC_Resend)
        IOLog("%s: Unexpected resend request from PS/2 controller.\n", getName());
    else
        dispatchKeyboardEventWithScancode(scanCode);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

bool GenericPS2Keyboard::dispatchKeyboardEventWithScancode(UInt8 scanCode)
{
    //
    // Parses the given scan code, updating all necessary internal state, and
    // should a new key be detected, the key event is dispatched.
    //
    // Returns true if a key event was indeed dispatched.
    //
    
    unsigned int keyCode;
    bool         goingDown;
    AbsoluteTime now;
    
    //
    // See if this scan code introduces an extended key sequence.  If so, note
    // it and then return.  Next time we get a key we'll finish the sequence.
    //
    
    if (scanCode == kSC_Extend)
    {
        _extendCount = 1;
        return false;
    }
    
    //
    // See if this scan code introduces an extended key sequence for the Pause
    // Key.  If so, note it and then return.  The next time we get a key, drop
    // it.  The next key we get after that finishes the Pause Key sequence.
    //
    // The sequence actually sent to us by the keyboard for the Pause Key is:
    //
    // 1. E1  Extended Sequence for Pause Key
    // 2. 1D  Useless Data, with Up Bit Cleared
    // 3. 45  Pause Key, with Up Bit Cleared
    // 4. E1  Extended Sequence for Pause Key
    // 5. 9D  Useless Data, with Up Bit Set
    // 6. C5  Pause Key, with Up Bit Set
    //
    // The reason items 4 through 6 are sent with the Pause Key is because the
    // keyboard hardware never generates a release code for the Pause Key and
    // the designers are being smart about it.  The sequence above translates
    // to this parser as two separate events, as it should be -- one down key
    // event and one up key event (for the Pause Key).
    //
    
    if (scanCode == kSC_Pause)
    {
        _extendCount = 2;
        return false;
    }
    
    //
    // Convert the scan code into a key code.
    //
    
    if (_extendCount == 0)
    {
        keyCode = scanCode & ~ kSC_UpBit;
        switch (keyCode) {
            case 0x38: if (_windowsAltSwap) { keyCode = 0x70; }; break; // left alt -> left command
            case 0x6a: if (_windowsAltSwap) { keyCode = 0x71; }; break; // right alt -> right command
        }
    }
    else
    {
        _extendCount--;
        if (_extendCount)  return false;
        
        //
        // Convert certain extended codes on the PC keyboard into single scancodes.
        // Refer to the conversion table in defaultKeymapOfLength.
        //
        switch (scanCode & ~kSC_UpBit)
        {
            case 0x1D: keyCode = 0x60; break;            // ctrl
            case 0x38: keyCode = 0x61; break;            // right alt (may become right command)
            case 0x1C: keyCode = 0x62; break;            // enter
            case 0x35: keyCode = 0x63; break;            // /
            case 0x48: keyCode = 0x64; break;            // up arrow
            case 0x50: keyCode = 0x65; break;            // down arrow
            case 0x4B: keyCode = 0x66; break;            // left arrow
            case 0x4D: keyCode = 0x67; break;            // right arrow
            case 0x52: keyCode = 0x68; break;            // insert
            case 0x53: keyCode = 0x69; break;            // delete
            case 0x49: keyCode = 0x6A; break;            // page up
            case 0x51: keyCode = 0x6B; break;            // page down
            case 0x47: keyCode = 0x6C; break;            // home
            case 0x4F: keyCode = 0x6D; break;            // end
            case 0x37: keyCode = 0x6E; break;            // PrintScreen
            case 0x45: keyCode = 0x6F; break;            // Pause
            case 0x5D: keyCode = 0x72; break;            // Application
            case 0x5B: // Left Windows/Command
                if (_windowsAltSwap)
                {
                    keyCode = 0x38; // left alt
                } else {
                    keyCode = 0x70; // left command
                }
                break;
            case 0x5c: // Right Windows/Command
                if (_windowsAltSwap)
                {
                    keyCode = 0x6a; // right alt
                } else {
                    keyCode = 0x71; // right command
                }
                break;
            // scancodes from running showkey -s (under Linux) for extra keys on keyboard
            case 0x30: keyCode = 0x7d; break;		     // E030 = volume up
            case 0x2e: keyCode = 0x7e; break;		     // E02E = volume down
            case 0x20: keyCode = 0x7f; break;		     // E020 = volume mute
            case 0x5e: keyCode = 0x7c; break;            // E05E = power
            case 0x5f:                                   // E05F = sleep
                keyCode = 0;
                if (!(scanCode & kSC_UpBit))
                {
                    IOPMrootDomain * rootDomain = getPMRootDomain();
                    if (rootDomain)
                        rootDomain->receivePowerNotification( kIOPMSleepNow );
                }
                break;

            case 0x2A:             // header or trailer for PrintScreen
            default: return false;
        }
    }
    
    if (keyCode == 0)  return false;
    
    //
    // Update our key bit vector, which maintains the up/down status of all keys.
    //
    
    goingDown = !(scanCode & kSC_UpBit);
    
    if (goingDown)
    {
        //
        // Verify that this is not an autorepeated key -- discard it if it is.
        //
        
        if (KBV_IS_KEYDOWN(keyCode, _keyBitVector))  return false;
        
        KBV_KEYDOWN(keyCode, _keyBitVector);
    }
    else
    {
        KBV_KEYUP(keyCode, _keyBitVector);
    }
    
    //
    // We have a valid key event -- dispatch it to our superclass.
    //
    
    clock_get_uptime(reinterpret_cast<UInt64*>(&now));
    
    UInt32 adbKeyCode = PS2ToADBMap[keyCode];
    if (adbKeyCode == 0x39) {
        adbKeyCode = _capslockKeyCode;
    }
    
    if (_remapFunctionKeys)
    {
        // Abuse the not-so-useful Insert and Application keys to be fn keys
        if (adbKeyCode == 0x72) // insert
        {
            _insertKeyDown = goingDown;
            return true;
        }
        if (adbKeyCode == 0x6e) // application
        {
            _applicationKeyDown = goingDown;
            return true;
        }
        
        if (!(_insertKeyDown || _applicationKeyDown))
        {
            adbKeyCode = remapFunctionKeys(adbKeyCode, goingDown);
            if (adbKeyCode == kRemappedDeadKey)
            {
                return true;
            }
        }
    }
    
    
    dispatchKeyboardEvent( adbKeyCode,
                          /*direction*/ goingDown,
                          /*timeStamp*/ now );
    
    return true;
}

UInt32 GenericPS2Keyboard::remapFunctionKeys(UInt32 adbKeyCode, bool goingDown)
{  
    switch(adbKeyCode)
    {
        case 0x7a: return  0x91; // F1 -> Brightness down
        case 0x78: return  0x90; // F2 -> Brightness up
        case 0x63: // F3 -> Mission Control
            // Eww....
            AbsoluteTime now;
            clock_get_uptime(reinterpret_cast<UInt64*>(&now));
            dispatchKeyboardEvent(0x3e, goingDown, now); // 1. right control
            dispatchKeyboardEvent(0x7e, goingDown, now); // 2. up arrow
            return kRemappedDeadKey;
        case 0x76: return 0x6f; // F4 -> F12 (== dashboard)
        /* case 0x60: */ // F5 -> F5 (or keyboard backlight down on an internal keyboard)
        /* case 0x61: */ // F6 -> F6 (keyboard backlight up)
        case 0x62: return kSpecialPrevious; // F7
        case 0x64: return kSpecialPlay; // F8
        case 0x65: return kSpecialNext; // F9
        case 0x6d: return 0x4a; // F10 -> Mute
        case 0x67: return 0x49; // F11 -> Volume Down
        case 0x6f: return 0x48; // F12 -> Volume Up
        default:
            return adbKeyCode;
    }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void GenericPS2Keyboard::setAlphaLockFeedback(bool locked)
{
    //
    // Set the keyboard LEDs to reflect the state of alpha (caps) lock.
    //
    // It is safe to issue this request from the interrupt/completion context.
    //
    
    _ledState = locked ? (_ledState | kLED_CapsLock):(_ledState & ~kLED_CapsLock);
    setLEDs(_ledState);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void GenericPS2Keyboard::setNumLockFeedback(bool locked)
{
    //
    // Set the keyboard LEDs to reflect the state of num lock.
    //
    // It is safe to issue this request from the interrupt/completion context.
    //
    
    _ledState = locked ? (_ledState | kLED_NumLock):(_ledState & ~kLED_NumLock);
    setLEDs(_ledState);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void GenericPS2Keyboard::setLEDs(UInt8 ledState)
{
    //
    // Asynchronously instructs the controller to set the keyboard LED state.
    //
    // It is safe to issue this request from the interrupt/completion context.
    //
    
    PS2Request * request = _device->allocateRequest();
    
    // (set LEDs command)
    request->commands[0].command = kPS2C_WriteDataPort;
    request->commands[0].inOrOut = kDP_SetKeyboardLEDs;
    request->commands[1].command = kPS2C_ReadDataPortAndCompare;
    request->commands[1].inOrOut = kSC_Acknowledge;
    request->commands[2].command = kPS2C_WriteDataPort;
    request->commands[2].inOrOut = ledState;
    request->commands[3].command = kPS2C_ReadDataPortAndCompare;
    request->commands[3].inOrOut = kSC_Acknowledge;
    request->commandsCount = 4;
    _device->submitRequest(request); // asynchronous, auto-free'd
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void GenericPS2Keyboard::setKeyboardEnable(bool enable)
{
    //
    // Instructs the keyboard to start or stop the reporting of key events.
    // Be aware that while the keyboard is enabled, asynchronous key events
    // may arrive in the middle of command sequences sent to the controller,
    // and may get confused for expected command responses.
    //
    // It is safe to issue this request from the interrupt/completion context.
    //
    
    PS2Request * request = _device->allocateRequest();
    
    // (keyboard enable/disable command)
    request->commands[0].command = kPS2C_WriteDataPort;
    request->commands[0].inOrOut = (enable)?kDP_Enable:kDP_SetDefaultsAndDisable;
    request->commands[1].command = kPS2C_ReadDataPortAndCompare;
    request->commands[1].inOrOut = kSC_Acknowledge;
    request->commandsCount = 2;
    _device->submitRequest(request); // asynchronous, auto-free'd
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void GenericPS2Keyboard::setCommandByte(UInt8 setBits, UInt8 clearBits)
{
    //
    // Sets the bits setBits and clears the bits clearBits "atomically" in the
    // controller's Command Byte.   Since the controller does not provide such
    // a read-modify-write primitive, we resort to a test-and-set try loop.
    //
    // Do NOT issue this request from the interrupt/completion context.
    //
    
    UInt8        commandByte;
    UInt8        commandByteNew;
    PS2Request * request = _device->allocateRequest();
    
    do
    {
        // (read command byte)
        request->commands[0].command = kPS2C_WriteCommandPort;
        request->commands[0].inOrOut = kCP_GetCommandByte;
        request->commands[1].command = kPS2C_ReadDataPort;
        request->commands[1].inOrOut = 0;
        request->commandsCount = 2;
        _device->submitRequestAndBlock(request);
        
        //
        // Modify the command byte as requested by caller.
        //
        
        commandByte    = request->commands[1].inOrOut;
        commandByteNew = (commandByte | setBits) & (~clearBits);
        
        // ("test-and-set" command byte)
        request->commands[0].command = kPS2C_WriteCommandPort;
        request->commands[0].inOrOut = kCP_GetCommandByte;
        request->commands[1].command = kPS2C_ReadDataPortAndCompare;
        request->commands[1].inOrOut = commandByte;
        request->commands[2].command = kPS2C_WriteCommandPort;
        request->commands[2].inOrOut = kCP_SetCommandByte;
        request->commands[3].command = kPS2C_WriteDataPort;
        request->commands[3].inOrOut = commandByteNew;
        request->commandsCount = 4;
        _device->submitRequestAndBlock(request);
        
        //
        // Repeat this loop if last command failed, that is, if the old command byte
        // was modified since we first read it.
        //
        
    } while (request->commandsCount != 4);  
    
    _device->freeRequest(request);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

const unsigned char * GenericPS2Keyboard::defaultKeymapOfLength(UInt32 * length)
{
	//
    // Keymap data borrowed from IOUSBFamily/AppleUSBKeyboard.
    //
    static const unsigned char appleUSAKeyMap[] = {
        0x00,0x00,
        
        // Modifier Defs
        0x0a,   //Number of modifier keys.  Was 7
        //0x00,0x01,0x39,  //CAPSLOCK, uses one byte.
        0x01,0x01,0x38,
        0x02,0x01,0x3b,
        0x03,0x01,0x3a,
        0x04,0x01,0x37,
        0x05,0x15,0x52,0x41,0x4c,0x53,0x54,0x55,0x45,0x58,0x57,0x56,0x5b,0x5c,
        0x43,0x4b,0x51,0x7b,0x7d,0x7e,0x7c,0x4e,0x59,
        0x06,0x01,0x72,
        0x09,0x01,0x3c, //Right shift
        0x0a,0x01,0x3e, //Right control
        0x0b,0x01,0x3d, //Right Option
        0x0c,0x01,0x36, //Right Command
        
        // key deffs
        0x7f,0x0d,0x00,0x61,
        0x00,0x41,0x00,0x01,0x00,0x01,0x00,0xca,0x00,0xc7,0x00,0x01,0x00,0x01,0x0d,0x00,
        0x73,0x00,0x53,0x00,0x13,0x00,0x13,0x00,0xfb,0x00,0xa7,0x00,0x13,0x00,0x13,0x0d,
        0x00,0x64,0x00,0x44,0x00,0x04,0x00,0x04,0x01,0x44,0x01,0xb6,0x00,0x04,0x00,0x04,
        0x0d,0x00,0x66,0x00,0x46,0x00,0x06,0x00,0x06,0x00,0xa6,0x01,0xac,0x00,0x06,0x00,
        0x06,0x0d,0x00,0x68,0x00,0x48,0x00,0x08,0x00,0x08,0x00,0xe3,0x00,0xeb,0x00,0x00,
        0x18,0x00,0x0d,0x00,0x67,0x00,0x47,0x00,0x07,0x00,0x07,0x00,0xf1,0x00,0xe1,0x00,
        0x07,0x00,0x07,0x0d,0x00,0x7a,0x00,0x5a,0x00,0x1a,0x00,0x1a,0x00,0xcf,0x01,0x57,
        0x00,0x1a,0x00,0x1a,0x0d,0x00,0x78,0x00,0x58,0x00,0x18,0x00,0x18,0x01,0xb4,0x01,
        0xce,0x00,0x18,0x00,0x18,0x0d,0x00,0x63,0x00,0x43,0x00,0x03,0x00,0x03,0x01,0xe3,
        0x01,0xd3,0x00,0x03,0x00,0x03,0x0d,0x00,0x76,0x00,0x56,0x00,0x16,0x00,0x16,0x01,
        0xd6,0x01,0xe0,0x00,0x16,0x00,0x16,0x02,0x00,0x3c,0x00,0x3e,0x0d,0x00,0x62,0x00,
        0x42,0x00,0x02,0x00,0x02,0x01,0xe5,0x01,0xf2,0x00,0x02,0x00,0x02,0x0d,0x00,0x71,
        0x00,0x51,0x00,0x11,0x00,0x11,0x00,0xfa,0x00,0xea,0x00,0x11,0x00,0x11,0x0d,0x00,
        0x77,0x00,0x57,0x00,0x17,0x00,0x17,0x01,0xc8,0x01,0xc7,0x00,0x17,0x00,0x17,0x0d,
        0x00,0x65,0x00,0x45,0x00,0x05,0x00,0x05,0x00,0xc2,0x00,0xc5,0x00,0x05,0x00,0x05,
        0x0d,0x00,0x72,0x00,0x52,0x00,0x12,0x00,0x12,0x01,0xe2,0x01,0xd2,0x00,0x12,0x00,
        0x12,0x0d,0x00,0x79,0x00,0x59,0x00,0x19,0x00,0x19,0x00,0xa5,0x01,0xdb,0x00,0x19,
        0x00,0x19,0x0d,0x00,0x74,0x00,0x54,0x00,0x14,0x00,0x14,0x01,0xe4,0x01,0xd4,0x00,
        0x14,0x00,0x14,0x0a,0x00,0x31,0x00,0x21,0x01,0xad,0x00,0xa1,0x0e,0x00,0x32,0x00,
        0x40,0x00,0x32,0x00,0x00,0x00,0xb2,0x00,0xb3,0x00,0x00,0x00,0x00,0x0a,0x00,0x33,
        0x00,0x23,0x00,0xa3,0x01,0xba,0x0a,0x00,0x34,0x00,0x24,0x00,0xa2,0x00,0xa8,0x0e,
        0x00,0x36,0x00,0x5e,0x00,0x36,0x00,0x1e,0x00,0xb6,0x00,0xc3,0x00,0x1e,0x00,0x1e,
        0x0a,0x00,0x35,0x00,0x25,0x01,0xa5,0x00,0xbd,0x0a,0x00,0x3d,0x00,0x2b,0x01,0xb9,
        0x01,0xb1,0x0a,0x00,0x39,0x00,0x28,0x00,0xac,0x00,0xab,0x0a,0x00,0x37,0x00,0x26,
        0x01,0xb0,0x01,0xab,0x0e,0x00,0x2d,0x00,0x5f,0x00,0x1f,0x00,0x1f,0x00,0xb1,0x00,
        0xd0,0x00,0x1f,0x00,0x1f,0x0a,0x00,0x38,0x00,0x2a,0x00,0xb7,0x00,0xb4,0x0a,0x00,
        0x30,0x00,0x29,0x00,0xad,0x00,0xbb,0x0e,0x00,0x5d,0x00,0x7d,0x00,0x1d,0x00,0x1d,
        0x00,0x27,0x00,0xba,0x00,0x1d,0x00,0x1d,0x0d,0x00,0x6f,0x00,0x4f,0x00,0x0f,0x00,
        0x0f,0x00,0xf9,0x00,0xe9,0x00,0x0f,0x00,0x0f,0x0d,0x00,0x75,0x00,0x55,0x00,0x15,
        0x00,0x15,0x00,0xc8,0x00,0xcd,0x00,0x15,0x00,0x15,0x0e,0x00,0x5b,0x00,0x7b,0x00,
        0x1b,0x00,0x1b,0x00,0x60,0x00,0xaa,0x00,0x1b,0x00,0x1b,0x0d,0x00,0x69,0x00,0x49,
        0x00,0x09,0x00,0x09,0x00,0xc1,0x00,0xf5,0x00,0x09,0x00,0x09,0x0d,0x00,0x70,0x00,
        0x50,0x00,0x10,0x00,0x10,0x01,0x70,0x01,0x50,0x00,0x10,0x00,0x10,0x10,0x00,0x0d,
        0x00,0x03,0x0d,0x00,0x6c,0x00,0x4c,0x00,0x0c,0x00,0x0c,0x00,0xf8,0x00,0xe8,0x00,
        0x0c,0x00,0x0c,0x0d,0x00,0x6a,0x00,0x4a,0x00,0x0a,0x00,0x0a,0x00,0xc6,0x00,0xae,
        0x00,0x0a,0x00,0x0a,0x0a,0x00,0x27,0x00,0x22,0x00,0xa9,0x01,0xae,0x0d,0x00,0x6b,
        0x00,0x4b,0x00,0x0b,0x00,0x0b,0x00,0xce,0x00,0xaf,0x00,0x0b,0x00,0x0b,0x0a,0x00,
        0x3b,0x00,0x3a,0x01,0xb2,0x01,0xa2,0x0e,0x00,0x5c,0x00,0x7c,0x00,0x1c,0x00,0x1c,
        0x00,0xe3,0x00,0xeb,0x00,0x1c,0x00,0x1c,0x0a,0x00,0x2c,0x00,0x3c,0x00,0xcb,0x01,
        0xa3,0x0a,0x00,0x2f,0x00,0x3f,0x01,0xb8,0x00,0xbf,0x0d,0x00,0x6e,0x00,0x4e,0x00,
        0x0e,0x00,0x0e,0x00,0xc4,0x01,0xaf,0x00,0x0e,0x00,0x0e,0x0d,0x00,0x6d,0x00,0x4d,
        0x00,0x0d,0x00,0x0d,0x01,0x6d,0x01,0xd8,0x00,0x0d,0x00,0x0d,0x0a,0x00,0x2e,0x00,
        0x3e,0x00,0xbc,0x01,0xb3,0x02,0x00,0x09,0x00,0x19,0x0c,0x00,0x20,0x00,0x00,0x00,
        0x80,0x00,0x00,0x0a,0x00,0x60,0x00,0x7e,0x00,0x60,0x01,0xbb,0x02,0x00,0x7f,0x00,
        0x08,0xff,0x02,0x00,0x1b,0x00,0x7e,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0x00,0x00,0x2e,0xff,0x00,0x00,
        0x2a,0xff,0x00,0x00,0x2b,0xff,0x00,0x00,0x1b,0xff,0xff,0xff,0x0e,0x00,0x2f,0x00,
        0x5c,0x00,0x2f,0x00,0x1c,0x00,0x2f,0x00,0x5c,0x00,0x00,0x0a,0x00,0x00,0x00,0x0d, //XX03
        0xff,0x00,0x00,0x2d,0xff,0xff,0x0e,0x00,0x3d,0x00,0x7c,0x00,0x3d,0x00,0x1c,0x00,
        0x3d,0x00,0x7c,0x00,0x00,0x18,0x46,0x00,0x00,0x30,0x00,0x00,0x31,0x00,0x00,0x32,
        0x00,0x00,0x33,0x00,0x00,0x34,0x00,0x00,0x35,0x00,0x00,0x36,0x00,0x00,0x37,0xff,
        0x00,0x00,0x38,0x00,0x00,0x39,0xff,0xff,0xff,0x00,0xfe,0x24,0x00,0xfe,0x25,0x00,
        0xfe,0x26,0x00,0xfe,0x22,0x00,0xfe,0x27,0x00,0xfe,0x28,0xff,0x00,0xfe,0x2a,0xff,
        0x00,0xfe,0x32,0x00,0xfe,0x35,0x00,0xfe,0x33,0xff,0x00,0xfe,0x29,0xff,0x00,0xfe,0x2b,0xff,
        0x00,0xfe,0x34,0xff,0x00,0xfe,0x2e,0x00,0xfe,0x30,0x00,0xfe,0x2d,0x00,0xfe,0x23,
        0x00,0xfe,0x2f,0x00,0xfe,0x21,0x00,0xfe,0x31,0x00,0xfe,0x20,
        0x00,0x01,0xac, //ADB=0x7b is left arrow
        0x00,0x01,0xae, //ADB = 0x7c is right arrow
        0x00,0x01,0xaf, //ADB=0x7d is down arrow.  
        0x00,0x01,0xad, //ADB=0x7e is up arrow	 
        0x0f,0x02,0xff,0x04,            
        0x00,0x31,0x02,0xff,0x04,0x00,0x32,0x02,0xff,0x04,0x00,0x33,0x02,0xff,0x04,0x00,
        0x34,0x02,0xff,0x04,0x00,0x35,0x02,0xff,0x04,0x00,0x36,0x02,0xff,0x04,0x00,0x37,
        0x02,0xff,0x04,0x00,0x38,0x02,0xff,0x04,0x00,0x39,0x02,0xff,0x04,0x00,0x30,0x02,
        0xff,0x04,0x00,0x2d,0x02,0xff,0x04,0x00,0x3d,0x02,0xff,0x04,0x00,0x70,0x02,0xff,
        0x04,0x00,0x5d,0x02,0xff,0x04,0x00,0x5b,
        0x0a, // number of following special keys
        0x04,0x39,  //caps lock
        0x05,0x72,  //NX_KEYTYPE_HELP is 5, ADB code is 0x72
        0x06,0x7f,  //NX_POWER_KEY is 6, ADB code is 0x7f
        0x07,0x4a,  //NX_KEYTYPE_MUTE is 7, ADB code is 0x4a
        0x00,0x48,  //NX_KEYTYPE_SOUND_UP is 0, ADB code is 0x48
        0x01,0x49,  //NX_KEYTYPE_SOUND_DOWN is 1, ADB code is 0x49
        NX_KEYTYPE_REWIND,kSpecialPrevious, // REWIND == previous track, no idea what NX_KEYTYPE_PREVIOUS is.
        NX_KEYTYPE_PLAY,kSpecialPlay,
        NX_KEYTYPE_FAST,kSpecialNext, // Similar. FAST == next track, NEXT == NFI.
        // remove arrow keys as special keys. They are generating double up/down scroll events
        // in both carbon and coco apps.
        //0x08,0x7e,  //NX_UP_ARROW_KEY is 8, ADB is 3e raw, 7e virtual (KMAP)
        //0x09,0x7d,  //NX_DOWN_ARROW_KEY is 9, ADB is 0x3d raw, 7d virtual
        0x0a,0x47   //NX_KEYTYPE_NUM_LOCK is 10, ADB combines with CLEAR key for numlock
    };
    
    *length = sizeof(appleUSAKeyMap);
    return appleUSAKeyMap;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

void GenericPS2Keyboard::setDevicePowerState( UInt32 whatToDo )
{
    IOLog("GenericPS2Keyboard::setDevicePowerState %d\n", whatToDo);
    switch ( whatToDo )
    {
        case kPS2C_DisableDevice:
            
            //
            // Disable keyboard.
            //
            
            setKeyboardEnable( false );
            
            break;
            
        case kPS2C_EnableDevice:
            
            //
            // Initialize the keyboard LED state.
            //
            
            setLEDs(_ledState);
            
            //
            // Enable the keyboard clock (should already be so), the keyboard
            // IRQ line, and the keyboard Kscan -> scan code translation mode.
            //
            
            setCommandByte( kCB_EnableKeyboardIRQ | kCB_TranslateMode,
                           kCB_DisableKeyboardClock );
            
            //
            // Finally, we enable the keyboard itself, so that it may start
            // reporting key events.
            //
            
            setKeyboardEnable( true );
            
            break;
    }
}
