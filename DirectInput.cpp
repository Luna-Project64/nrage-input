/*	
	N-Rage`s Dinput8 Plugin
    (C) 2002, 2006  Norbert Wladyka

	Author`s Email: norbert.wladyka@chello.at
	Website: http://go.to/nrage


    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/


#include "commonIncludes.h"
#include <dinput.h>
#include "NRagePluginV2.h"
#include "PakIO.h"
#include "DirectInput.h"
#include "XInputController.h"
#include "Luna.h"
#include "FileAccess.h"
#include <math.h>
#include <dbt.h>

// ProtoTypes //
HRESULT AcquireDevice( LPDIRECTINPUTDEVICE8 lpDirectInputDevice );
BOOL CALLBACK EnumSetObjectsAxis( LPCDIDEVICEOBJECTINSTANCE lpddoi, LPVOID pvRef );
static void SetDeviceBufferSize( LPDIRECTINPUTDEVICE8 lpDirectInputDevice );
static void PollDeviceHotplug();

// global Variables //
LPDIRECTINPUT8  g_pDIHandle = NULL;													// Base DirectInput8-Handle
bool g_bSubFrameTaps = true;														// see LatchSubFrameTaps(); "SubFrameTaps" in the INI turns it off

//LPDIRECTINPUTDEVICE8 g_apInputDevice[6] = { NULL, NULL, NULL, NULL, NULL, NULL };	// array of Handles for devices
																					// 0:Keyboard, 1:Mouse, the rest are FF gamepads

// BYTE g_acKeystate[256];		// use g_sysKeyboard.stateAs.rgbButtons instead
// DIMOUSESTATE2 g_msMouseState = { 0, 0, 0 };											// Store our mouse state data between reads (because every time we read data, it resets the device data)
		// moved to g_sysMouse.stateAs...

// Is this a keyboard or a mouse rather than a gamepad/joystick?
// dwDevType is a packed DWORD (type in the low byte, subtype above it, plus DIDEVTYPE_HID),
// so it has to be masked before it's compared to anything -- comparing the whole DWORD is
// always unequal for real hardware.
inline bool IsGamePadDevType( DWORD dwDevType )
{
	BYTE bType = LOBYTE( dwDevType );
	return ( bType != DI8DEVTYPE_KEYBOARD ) && ( bType != DI8DEVTYPE_MOUSE );
}

// Force a device's cached state to "nothing is pressed".
// A zeroed DIJOYSTATE is NOT neutral: rgdwPOV of 0 means the hat is being held straight up,
// so POVs have to be set to -1 by hand or an absent gamepad reads as holding D-pad up.
void NeutralizeDeviceState( DEVICE &device )
{
	ZeroMemory( &device.stateAs, sizeof(DEVICE::INPUTSTATE) );
	ZeroMemory( device.abLatchedPress, sizeof(device.abLatchedPress) );

	if( IsGamePadDevType( device.dwDevType ))
		FillMemory( device.stateAs.joyState.rgdwPOV, sizeof(device.stateAs.joyState.rgdwPOV), 0xFF );
}

// Map a queued event's data offset onto the index BUTTON::bOffset uses for this device type.
// -1 for anything that isn't a button or key: axes and POVs report a position rather than an
// event, and we already have the current one.
static int LatchIndexFromOffset( const DEVICE &device, DWORD dwOfs )
{
	switch( LOBYTE( device.dwDevType ))
	{
	case DI8DEVTYPE_KEYBOARD:
		// c_dfDIKeyboard data offsets are the DIK scancodes themselves
		return ( dwOfs < 256 ) ? (int)dwOfs : -1;

	case DI8DEVTYPE_MOUSE:
		return -1;		// nothing bound to a mouse needs sub-frame precision

	default:
		if( dwOfs >= DIJOFS_BUTTON( 0 ) && dwOfs < DIJOFS_BUTTON( 0 ) + ARRAYSIZE( device.stateAs.joyState.rgbButtons ))
			return (int)( dwOfs - DIJOFS_BUTTON( 0 ));
		return -1;
	}
}

// Drain the device's event queue and remember any button that went down since the last read,
// so a press too short to be caught by the immediate state still reaches the game -- for
// exactly one frame, because the latch is rebuilt from scratch on every read.
static void LatchSubFrameTaps( DEVICE &device )
{
	ZeroMemory( device.abLatchedPress, sizeof(device.abLatchedPress) );

	if( !g_bSubFrameTaps )
		return;

	DIDEVICEOBJECTDATA rgod[DEVICE_BUFFER_SIZE];
	DWORD dwItems = ARRAYSIZE( rgod );

	// DI_BUFFEROVERFLOW is a success code and only means we lost the oldest events; the
	// immediate state we already read is still correct, so take what's here and carry on.
	if( FAILED( device.didHandle->GetDeviceData( sizeof(DIDEVICEOBJECTDATA), rgod, &dwItems, 0 )))
		return;

	DWORD dwNow = GetTickCount();

	for( DWORD n = 0; n < dwItems; ++n )
	{
		if( !( rgod[n].dwData & 0x80 ))
			continue;		// a release, not a press

		if( (LONG)( dwNow - rgod[n].dwTimeStamp ) > LATCH_MAX_AGE_MS )
			continue;		// stale: queued while nothing was reading this device

		int iLatch = LatchIndexFromOffset( device, rgod[n].dwOfs );

		if( iLatch >= 0 )
			device.abLatchedPress[iLatch] = 1;
	}
}

// Try to get a lost device back.
// A controller that has just come back over USB or Bluetooth is, as far as the HID stack is
// concerned, a new device, and the first state DirectInput hands us afterwards can be a
// leftover or an axis reported in the device's own default range instead of the -32768..32767
// we asked for. Either one reads as a stick shoved into a corner for a frame or two, which is
// exactly long enough to end a run. So: re-apply the axis ranges, then hold the device at
// neutral until it has had a moment to settle.
static bool ReacquireDevice( DEVICE &device )
{
	DWORD dwNow = GetTickCount();

	// A device that isn't there yet fails to acquire every single frame, from inside the
	// emulator's input path. Back off instead of retrying 60 times a second.
	if( device.fAcquireThrottled && (LONG)( dwNow - device.dwNextAcquire ) < 0 )
		return false;

	if( FAILED( AcquireDevice( device.didHandle )))
	{
		device.dwNextAcquire = dwNow + REACQUIRE_INTERVAL_MS;
		device.fAcquireThrottled = true;
		return false;
	}

	device.fAcquireThrottled = false;

	if( IsGamePadDevType( device.dwDevType ))
	{
		// SetProperty needs the device unacquired, so give up the acquisition we just took.
		device.didHandle->Unacquire();
		device.didHandle->EnumObjects( EnumSetObjectsAxis, device.didHandle, DIDFT_AXIS );
		SetDeviceBufferSize( device.didHandle );

		if( FAILED( AcquireDevice( device.didHandle )))
		{
			device.dwNextAcquire = dwNow + REACQUIRE_INTERVAL_MS;
			device.fAcquireThrottled = true;
			return false;
		}

		device.dwSettleTime = dwNow + DEVICE_SETTLE_MS;
		device.fSettling = true;
	}

	DebugWrite( _T("Acquired device %s\n"), device.szProductName );
	return true;
}

// Poll one device and refresh its cached state.
// Every failure path ends at neutral rather than at "whatever it said last time", so a
// controller that dies while a direction is held doesn't leave that direction stuck on.
static void ReadDeviceState( DEVICE &device )
{
	if( !device.didHandle )
	{
		device.fAcquired = false;
		NeutralizeDeviceState( device );
		return;
	}

	if( !device.fInUse )		// nothing reads this device; don't spend a frame's time on it
		return;

	HRESULT hr = device.didHandle->Poll();

	if( FAILED( hr ))
	{
		device.fAcquired = false;
		if( !ReacquireDevice( device ))
		{
			NeutralizeDeviceState( device );
			return;
		}
	}

	switch( LOBYTE( device.dwDevType ))
	{
	case DI8DEVTYPE_KEYBOARD:
		hr = device.didHandle->GetDeviceState( sizeof(device.stateAs.rgbButtons), device.stateAs.rgbButtons );
		break;
	case DI8DEVTYPE_MOUSE:
		hr = device.didHandle->GetDeviceState( sizeof(device.stateAs.mouseState), &device.stateAs.mouseState );
		break;
	default:
		hr = device.didHandle->GetDeviceState( sizeof(device.stateAs.joyState), &device.stateAs.joyState );
	}

	// DIERR_INPUTLOST and DIERR_UNPLUGGED land here too, not just DIERR_NOTACQUIRED; the old
	// code only zeroed on the last of those and kept serving stale input for the others.
	if( FAILED( hr ))
	{
		device.fAcquired = false;
		NeutralizeDeviceState( device );
		return;
	}

	device.fAcquired = true;

	LatchSubFrameTaps( device );

	if( device.fSettling )
	{
		if( (LONG)( GetTickCount() - device.dwSettleTime ) < 0 )
			NeutralizeDeviceState( device );		// also drops the latches
		else
			device.fSettling = false;
	}
}

// Update device data tables (so we only have to poll and read the devices once).  This is called by GetKeys and ReadController.
void GetDeviceDatas()
{
	// Pick up controllers plugged in after the ROM was loaded. Costs nothing unless the
	// hotplug watcher actually saw a device come or go.
	PollDeviceHotplug();

	if( g_sysMouse.didHandle )
		ReadDeviceState( g_sysMouse );

	// need to just poll every damn device we're using
	for( int i = 0; i < g_nDevices; i++ )
		ReadDeviceState( g_devList[i] );
}

// hacked up piece of shit, but it works
inline bool GetJoyPadPOV( PDWORD dwDegree, BYTE AxeId )
// TRUE if specified Direction is Pressed
{
	if( LOWORD( *dwDegree ) == 0xFFFF )
		return false;

	bool bPressed;

	switch( AxeId )
	{
	case AI_POV_DOWN:
		bPressed = (( *dwDegree >= 18000 - POVANGLETHRESH ) && (*dwDegree <= 18000 + POVANGLETHRESH ));
		break;
	case AI_POV_LEFT:
		bPressed = (( *dwDegree >= 27000 - POVANGLETHRESH ) && (*dwDegree <= 27000 + POVANGLETHRESH ));
		break;
	case AI_POV_RIGHT:
		bPressed = (( *dwDegree >= 9000 - POVANGLETHRESH ) && (*dwDegree <= 9000 + POVANGLETHRESH ));
		break;
	case AI_POV_UP:
		bPressed = (( *dwDegree >= 36000 - POVANGLETHRESH ) || ( *dwDegree <= 0 + POVANGLETHRESH ));
		break;
	default:
		bPressed = false;
	}
	
	return bPressed;
}

// Fill in button states and axis states for controller indexController, into the struct pdwData.
// pdwData is a pointer to a 4 byte BUTTONS union, if anyone cares
bool GetNControllerInput ( const int indexController, LPDWORD pdwData )
{
	*pdwData = 0;
	WORD w_Buttons = 0;
	// WORD w_Axes = 0;

	LPCONTROLLER pcController = &g_pcControllers[indexController]; // still needs to be here, but not as important --rabid

	bool b_Value;
	long l_Value = 0;

	long lAxisValueX = ZEROVALUE;
	long lAxisValueY = ZEROVALUE;

	// take this info from the N64 controller struct, regardless of input devices
	float d_ModifierX = (float)pcController->bStickRange / 100.0f;
	float d_ModifierY = (float)pcController->bStickRange / 100.0f;

	int i;

	// do N64-Buttons / modifiers
	for (i = 0; i < pcController->nModifiers; i++ )
	{
		BUTTON btnButton = pcController->pModifiers[i].btnButton;

		b_Value = IsBtnPressed( pcController, btnButton );

		bool fChangeMod = false;

		if( pcController->pModifiers[i].bModType == MDT_CONFIG )
		{ // Config-Type
			if( pcController->pModifiers[i].fToggle )
			{
				if( b_Value && !btnButton.fPrevPressed)
				{
					pcController->pModifiers[i].fStatus = !pcController->pModifiers[i].fStatus;
					fChangeMod = true;
				}
			}
			else
			{
				if(	b_Value != (bool)(btnButton.fPrevPressed))
					fChangeMod = true;
			}
		}
		else if (pcController->pModifiers[i].bModType == MDT_LUNA)
		{
			if (b_Value != (bool)(btnButton.fPrevPressed))
				fChangeMod = true;
		}
		else
		{ // Move / Macro Type
			if( pcController->pModifiers[i].fToggle )
			{
				if( b_Value && !btnButton.fPrevPressed )
					pcController->pModifiers[i].fStatus = !pcController->pModifiers[i].fStatus;
				fChangeMod = ( pcController->pModifiers[i].fStatus != 0 );
			}
			else
			{
				fChangeMod = b_Value;
			}
		}

		if( fChangeMod )
		{
			switch( pcController->pModifiers[i].bModType )
			{
			case MDT_MOVE:
			{
				LPMODSPEC_MOVE args = (LPMODSPEC_MOVE)&pcController->pModifiers[i].dwSpecific;
				d_ModifierX *= args->XModification / 100.0f;
				d_ModifierY *= args->YModification / 100.0f;	
			}
				break;
			case MDT_MACRO:
			{
				LPMODSPEC_MACRO args = (LPMODSPEC_MACRO)&pcController->pModifiers[i].dwSpecific;

				if (args->fRapidFire) // w00t! Rapid Fire here
				{
					if ((unsigned) b_Value != btnButton.fPrevPressed) // New macro pressed
					{
						args->fPrevFireState = 0;
						args->fPrevFireState2 = 0;
					}
					if(!args->fPrevFireState) // This round, a firing is needed
					{
						w_Buttons |= args->aButtons;
						if( args->fAnalogRight )
							lAxisValueX += MAXAXISVALUE;
						else if( args->fAnalogLeft )
							lAxisValueX -= MAXAXISVALUE;

						if( args->fAnalogDown )
							lAxisValueY -= MAXAXISVALUE;
						else if( args->fAnalogUp ) // up
								lAxisValueY += MAXAXISVALUE;
					}

					// Ok, update the firing counters here
					if (args->fRapidFireRate) // Do the rapid fire slowly
					{ // Note that this updates State2 before State... Makes a nice slower square-wave type pulse for the update
						args->fPrevFireState2 = (args->fPrevFireState2 + 1) & 1;
						if (!args->fPrevFireState2)
						{
							args->fPrevFireState = (args->fPrevFireState + 1) & 1;
							DebugWriteA("Slow Rapid Fire - Mark 2\n");
						}
					}
					else // Do a fast rapid fire
					{
						args->fPrevFireState = (args->fPrevFireState + 1) & 1;
						DebugWriteA("Fast Rapid Fire\n");
					}
				}
				else
				{
					w_Buttons |= args->aButtons; // Note this: It lets you push buttons as well as the macro buttons
					if( args->fAnalogRight )
						lAxisValueX += MAXAXISVALUE;
					else if( args->fAnalogLeft )
						lAxisValueX -= MAXAXISVALUE;

					if( args->fAnalogDown )
						lAxisValueY -= MAXAXISVALUE;
					else if( args->fAnalogUp ) // up
						lAxisValueY += MAXAXISVALUE;

					args->fPrevFireState = 0;
				}
			}
				break;
			case MDT_CONFIG:
			{
				LPMODSPEC_CONFIG args = (LPMODSPEC_CONFIG)&pcController->pModifiers[i].dwSpecific;

				if( args->fChangeAnalogConfig )
				{
					BYTE bConfig = (BYTE)args->fAnalogStickMode;
					if( bConfig < PF_AXESETS )
						pcController->bAxisSet = bConfig;
					else
					{
						if( pcController->bAxisSet == PF_AXESETS-1 )
							pcController->bAxisSet = 0;
						else
							++pcController->bAxisSet;
					}

				}
				if( args->fChangeMouseXAxis )
					if (pcController->bMouseMoveX == MM_BUFF)
						pcController->bMouseMoveX = MM_ABS;
					else if (pcController->bMouseMoveX == MM_ABS)
						pcController->bMouseMoveX = MM_BUFF;
				if( args->fChangeMouseYAxis )
					if (pcController->bMouseMoveY == MM_BUFF)
						pcController->bMouseMoveY = MM_ABS;
					else if (pcController->bMouseMoveY == MM_ABS)
						pcController->bMouseMoveY = MM_BUFF;

				if( args->fChangeKeyboardXAxis )
					pcController->fKeyAbsoluteX = !pcController->fKeyAbsoluteX;
				if( args->fChangeKeyboardYAxis )
					pcController->fKeyAbsoluteY = !pcController->fKeyAbsoluteY;
			}
				break;

			case MDT_LUNA:
			{
				int arg = pcController->pModifiers[i].dwSpecific;
				if (Luna::gExCommandHandler)
				{
					switch (arg)
					{
					case LUNAEX_LOAD_STATE:
						if (b_Value)
							Luna::gExCommandHandler(Luna::gMainWindow, LUNA_EXCMD_LOAD_STATE);
						break;
					case LUNAEX_SAVE_STATE:
						if (b_Value)
							Luna::gExCommandHandler(Luna::gMainWindow, LUNA_EXCMD_SAVE_STATE);
						break;
					case LUNAEX_TOGGLE_FPS:
						if (b_Value)
							Luna::gExCommandHandler(Luna::gMainWindow, LUNA_EXCMD_UNLOCK_FPS);
						else
							Luna::gExCommandHandler(Luna::gMainWindow, LUNA_EXCMD_LOCK_FPS);

						break;
					}
				}
				break;
			}
			}
		}

		btnButton.fPrevPressed = b_Value;
		pcController->pModifiers[i].btnButton = btnButton;
	} // END N64 MODIFIERS for

	// do N64-Buttons / modifiers
	for( i = 0; i < PF_APADR; i++ )
	{
		BUTTON btnButton = pcController->aButton[i];

		b_Value = IsBtnPressed( pcController, btnButton );

		w_Buttons |= (((WORD)b_Value) << i);
	} // END N64 BUTTONS for

	long lDeadZoneValue = pcController->bPadDeadZone * RANGERELATIVE / 100;
	float fDeadZoneRelation	= (float)RANGERELATIVE  / (float)( RANGERELATIVE - lDeadZoneValue );

	// do N64 joystick axes
	for ( i = 0; i < 4; i++ )
	{
		//	0 : right
		//	1 : left
		//	2 : down
		//	3 : up

		bool fNegInput = (( i == 1 ) || ( i == 2 )); // Input has to be negated

		BUTTON btnButton = pcController->aButton[PF_APADR + pcController->bAxisSet * 4 + i];

		if( !btnButton.parentDevice )	// assigned to a device that isn't connected
			continue;

		LPLONG plRawState = (LPLONG)&btnButton.parentDevice->stateAs.joyState;
		
		switch( btnButton.bBtnType )
		{
		case DT_JOYBUTTON:
			l_Value = MAXAXISVALUE;
			b_Value = ( btnButton.parentDevice->stateAs.joyState.rgbButtons[btnButton.bOffset] & 0x80 ) != 0
					|| btnButton.parentDevice->abLatchedPress[btnButton.bOffset] != 0;
			break;

		case DT_JOYSLIDER:
		case DT_JOYAXE:
			l_Value = plRawState[btnButton.bOffset] - ZEROVALUE;

			if( btnButton.bAxisID ) // negative Range
			{
				fNegInput = !fNegInput;

				b_Value = ( l_Value <= -lDeadZoneValue );
				if( b_Value )
					l_Value = (long) ((float)(l_Value + lDeadZoneValue ) * fDeadZoneRelation );
			}
			else
			{
				b_Value = ( l_Value >= lDeadZoneValue );
				if( b_Value )
					l_Value = (long) ((float)(l_Value - lDeadZoneValue ) * fDeadZoneRelation );
			}	
			break;

		case DT_JOYPOV:
			l_Value = MAXAXISVALUE;
			b_Value = GetJoyPadPOV( (PDWORD)&plRawState[btnButton.bOffset] , btnButton.bAxisID );
			break;

		case DT_KEYBUTTON:
			// deleted a bunch of stuff hereish, won't even pretend to understand how it worked ~N3
			if(( btnButton.parentDevice->stateAs.rgbButtons[btnButton.bOffset] & 0x80 )
				|| btnButton.parentDevice->abLatchedPress[btnButton.bOffset] )
			{
				b_Value = true;

				l_Value = MAXAXISVALUE;
			}
			else
			{
				b_Value = false;
			}
			break;

		case DT_MOUSEBUTTON:
			l_Value = MAXAXISVALUE;
			b_Value = ( btnButton.parentDevice->stateAs.mouseState.rgbButtons[btnButton.bOffset] & 0x80 ) != 0;
			break;

		case DT_MOUSEAXE:
			if( i < 2 )
				pcController->wAxeBuffer[i] += plRawState[btnButton.bOffset] * pcController->wMouseSensitivityX * MOUSESCALEVALUE;	// l_Value = btnButton.parentDevice->stateAs.mouseState[btnButton.bOffset];
			else
				pcController->wAxeBuffer[i] += plRawState[btnButton.bOffset] * pcController->wMouseSensitivityY * MOUSESCALEVALUE;	// l_Value = btnButton.parentDevice->stateAs.mouseState[btnButton.bOffset];

			l_Value = pcController->wAxeBuffer[i];

			// wAxeBuffer is positive for axes 0 and 3 if buffer remains, else zero
			// wAxeBuffer is negative for axes 1 and 2 if buffer remains, else zero

			if(( pcController->bMouseMoveX == MM_ABS && i < 2 ) || ( pcController->bMouseMoveY == MM_ABS &&  i > 1 ))
				pcController->wAxeBuffer[i] = min( max( MINAXISVALUE, pcController->wAxeBuffer[i]) , MAXAXISVALUE);
			else if (( pcController->bMouseMoveX == MM_BUFF && i < 2 ) || ( pcController->bMouseMoveY == MM_BUFF &&  i > 1 ))
				pcController->wAxeBuffer[i] = pcController->wAxeBuffer[i] * MOUSEBUFFERDECAY / 100;
			else // "deadpan" mouse
			{
				pcController->wAxeBuffer[i] = 0;
			}

			if( btnButton.bAxisID == AI_AXE_N) // the mouse axis has the '-' flag set
			{
				fNegInput = !fNegInput;

				b_Value = ( l_Value < ZEROVALUE );
			}
			else
			{
				b_Value = ( l_Value > ZEROVALUE );
			}

			break;
		
		case DT_UNASSIGNED:
		default:
			b_Value = false;
		}

		if ( b_Value )
		{
			if ( fNegInput )
				l_Value = -l_Value;
			
			if( i < 2 )
				lAxisValueX += l_Value;
			else
				lAxisValueY += l_Value;
		}
	}

	if( pcController->fKeyboard )
	{
		if( pcController->fKeyAbsoluteX )
		{
			if( pcController->wAxeBuffer[0] > pcController->wAxeBuffer[1] )
			{
				pcController->wAxeBuffer[0] -= pcController->wAxeBuffer[1];
				pcController->wAxeBuffer[1] = 0;
			}
			else
			{
				pcController->wAxeBuffer[1] -= pcController->wAxeBuffer[0];
				pcController->wAxeBuffer[0] = 0;
			}
		}
		if( pcController->fKeyAbsoluteY )
		{
			if( pcController->wAxeBuffer[2] > pcController->wAxeBuffer[3] )
			{
				pcController->wAxeBuffer[2] -= pcController->wAxeBuffer[3];
				pcController->wAxeBuffer[3] = 0;
			}
			else
			{
				pcController->wAxeBuffer[3] -= pcController->wAxeBuffer[2];
				pcController->wAxeBuffer[2] = 0;
			}
		}
	}


	if (pcController->bRapidFireEnabled) {
		if (pcController->bRapidFireCounter >= pcController->bRapidFireRate) {
			w_Buttons = (w_Buttons & 0xFF1F);
			pcController->bRapidFireCounter = 0;
		} else{
			pcController->bRapidFireCounter = pcController->bRapidFireCounter + 1;
		}
	}

	if( pcController->bDiagStretch && ( lAxisValueX || lAxisValueY ))
	{
		float X = fabs(lAxisValueX / 32768.f);
		float Y = fabs(lAxisValueY / 32768.f);
		if (Y > X)
			std::swap(X, Y);

		auto slope = [](float d) { return (1 - d) / d; };
        auto unslope = [](float s) { return 1 / (1 + s); };

		float toSlope = slope(70.f / 85.f);
		float fromSlope = slope(70.f / 85.f - pcController->bDiagStretch / 500.f);

		float fromAvg = X + fromSlope * Y;
		float toAvg = X + toSlope * Y;

		float fullMultiplier = fromAvg / toAvg;
		float addition = fullMultiplier - 1;
		float dampening = fromAvg;

		float multiplier = 1.f + addition * dampening;
		float maxMultiplier = 1.f / max(X, Y);
		if (multiplier > maxMultiplier)
			multiplier = maxMultiplier;

		*pdwData = MAKELONG(w_Buttons,
							MAKEWORD(	(BYTE)(min( max( MINAXISVALUE, (long)(lAxisValueX * d_ModifierX * multiplier)), MAXAXISVALUE) / N64DIVIDER ),
										(BYTE)(min( max( MINAXISVALUE, (long)(lAxisValueY * d_ModifierY * multiplier)), MAXAXISVALUE) / N64DIVIDER )));
	}
	else
	{
		*pdwData = MAKELONG(w_Buttons,
							MAKEWORD(	(BYTE)(min( max( MINAXISVALUE, (long)(lAxisValueX * d_ModifierX )), MAXAXISVALUE) / N64DIVIDER ),
										(BYTE)(min( max( MINAXISVALUE, (long)(lAxisValueY * d_ModifierY )), MAXAXISVALUE) / N64DIVIDER )));
	}
	

	return true;
}

bool InitDirectInput( HWND hWnd )
{
	//If we havent already got the DLL then lets load it
	if( g_hDirectInputDLL == NULL )
		g_hDirectInputDLL = LoadLibrary( _T( "dinput8.dll" ));

	//If we failed to load in the dll then spit out an error
	if( g_hDirectInputDLL == NULL )
		ErrorMessage(IDS_ERR_DINOTFOUND, 0, false);
	else if( !g_pDIHandle ) // is NULL if not yet initialized
	{
		//Inititalize a handle for the directinput
		HRESULT (WINAPI *lpGetDIHandle)( HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN ) = NULL;
		//Get the function for creating the direct input interface
		lpGetDIHandle = (HRESULT (WINAPI *)( HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN ))GetProcAddress( g_hDirectInputDLL, "DirectInput8Create" );

		//If we have set the variable succesfully then continual
		if( lpGetDIHandle != NULL )
		{
			HRESULT hr;
			//create the direct input handle
			hr = lpGetDIHandle( g_strEmuInfo.hinst, DIRECTINPUT_VERSION, IID_IDirectInput8, (LPVOID*)&g_pDIHandle, NULL );
			
			//if we failed to create the handle, unload the library
			if( FAILED( hr ))
			{
				ErrorMessage(IDS_ERR_DICREATE, 0, false);
				g_pDIHandle = NULL;
				FreeLibrary( g_hDirectInputDLL );
				g_hDirectInputDLL = NULL;
			}
		}
	}

	return (g_pDIHandle != NULL);
}

// release a DirectInput device.  We don't need it anymore.
void ReleaseDevice( LPDIRECTINPUTDEVICE8 &lpDirectInputDevice )
{
	if( lpDirectInputDevice != NULL)
	{
		lpDirectInputDevice->Unacquire();
		lpDirectInputDevice->Release();
		lpDirectInputDevice = NULL;
	}
	return;
}

// release a DirectInput effect.  We don't need it anymore.
void ReleaseEffect( LPDIRECTINPUTEFFECT &lpDirectEffect )
{
	if( lpDirectEffect != NULL)
	{
		// should unload the effect on release, I hope
		lpDirectEffect->Release();
		lpDirectEffect = NULL;
	}
	return;
}

// release our DirectInput effects and devices, our DirectInput handle, and then unload dinput library
void FreeDirectInput ()
{
	int i;
	// release effects
	for( i = 0; i < ARRAYSIZE( g_apdiEffect ); ++i )
		ReleaseEffect( g_apdiEffect[i] );
	ZeroMemory( g_apdiEffect, sizeof(g_apdiEffect) );

	// release FF devices
	for( i = 0; i < ARRAYSIZE(g_apFFDevice); ++i )		// was "i << ARRAYSIZE(...)", which is 0 on the first pass, so this loop never ran and the FF devices leaked
		ReleaseDevice( g_apFFDevice[i] );
	ZeroMemory( g_apFFDevice, sizeof(g_apFFDevice) );

	// release normal devices
	for( i = 0; i < g_nDevices; i++ )
		ReleaseDevice( g_devList[i].didHandle );
	ZeroMemory( g_devList, sizeof(g_devList) );
	g_nDevices = 0;

	// release mouse device
	ReleaseDevice( g_sysMouse.didHandle );
	ZeroMemory( &g_sysMouse, sizeof(g_sysMouse) );

    // Release any DirectInput handles.
	if( g_pDIHandle != NULL )
	{
		g_pDIHandle->Release();
		g_pDIHandle = NULL;
	}
	// Unload the library.
	if( g_hDirectInputDLL != NULL )
	{
		FreeLibrary( g_hDirectInputDLL );
		g_hDirectInputDLL = NULL;
	}
	return;
}

// Acquire our device.  Our device might get unacquired for many many reasons, and we need to be able to reacquire it to get input again.
// We use this a LOT.
inline HRESULT AcquireDevice( LPDIRECTINPUTDEVICE8 lpDirectInputDevice )
{
	HRESULT hResult = lpDirectInputDevice->Acquire();
	// bounded: a device that answers DIERR_INPUTLOST forever used to hang the emulation thread here
	for( int iRetry = 0; hResult == DIERR_INPUTLOST && iRetry < ACQUIRE_MAX_RETRIES; ++iRetry )
		hResult = lpDirectInputDevice->Acquire();
	if( SUCCEEDED( hResult ))
		lpDirectInputDevice->Poll();
	return hResult;
}

// Called by the DirectInput enumerator for each FF device. What kind of force feedback effects does this device support?  We'll store it in pvRef.
BOOL CALLBACK EnumGetEffectTypes( LPCDIEFFECTINFO pdei, LPVOID pvRef )
{
	BYTE bFFType = *(LPBYTE)pvRef;
	bFFType |= ( pdei->dwEffType & DIEFT_CONSTANTFORCE )	? RUMBLE_CONSTANT : 0;
	bFFType |= ( pdei->dwEffType & DIEFT_RAMPFORCE )		? RUMBLE_RAMP : 0;
	bFFType |= ( pdei->dwEffType & DIEFT_CONDITION )		? RUMBLE_CONDITION : 0;
	bFFType |= ( pdei->dwEffType & DIEFT_PERIODIC )			? RUMBLE_PERIODIC : 0;
	bFFType |= ( pdei->dwEffType & DIEFT_CUSTOMFORCE )		? RUMBLE_CUSTOM : 0;
	*(WORD*)pvRef = bFFType;
	return DIENUM_CONTINUE;
}

// Called by the DirectInput enumerator for each attached DI device.  We use it to make a list of devices.
// EnumMakeDeviceList has been rewritten. --rabid
BOOL CALLBACK EnumMakeDeviceList( LPCDIDEVICEINSTANCE lpddi, LPVOID pvRef )
{
	if (IsEqualGUID(g_sysMouse.guidInstance, lpddi->guidInstance))
		return DIENUM_CONTINUE;

	for (int i = 0; i < g_nDevices; i++)
		if (IsEqualGUID(g_devList[i].guidInstance, lpddi->guidInstance))
			return ( g_nDevices < ARRAYSIZE(g_devList) ) ? DIENUM_CONTINUE : DIENUM_STOP;

	if (g_nDevices < ARRAYSIZE(g_devList)) // our buffer isn't full yet and the device doesn't already exist in our table
	{
		lstrcpyn( g_devList[g_nDevices].szProductName, lpddi->tszProductName, MAX_PATH );
		g_devList[g_nDevices].dwDevType = lpddi->dwDevType;
		g_devList[g_nDevices].guidInstance = lpddi->guidInstance;

		g_devList[g_nDevices].bProductCounter = 0; // counting similar devices
		for( int i = 0; i < g_nDevices; ++i )
		{
			if( !lstrcmp( lpddi->tszProductName, g_devList[i].szProductName ))
			{
				if( g_devList[g_nDevices].bProductCounter == 0 )
				{
					g_devList[g_nDevices].bProductCounter = 2;
					if( g_devList[i].bProductCounter == 0 )
						g_devList[i].bProductCounter = 1;
				}
				else
					g_devList[g_nDevices].bProductCounter++; // give em instance numbers
			}
		}

		if( !lstrcmp( lpddi->tszProductName, TEXT( STRING_ADAPTOID )))
			g_devList[g_nDevices].bEffType = RUMBLE_DIRECT;
		else
			g_devList[g_nDevices].bEffType = RUMBLE_NONE;

		if ( GetInputDevice(g_strEmuInfo.hMainWindow, g_devList[g_nDevices].didHandle, lpddi->guidInstance, lpddi->dwDevType, lpddi, DIB_DEVICE) )
		{
			g_devList[g_nDevices].didHandle->EnumEffects( EnumGetEffectTypes, &g_devList[g_nDevices].bEffType, DIEFT_ALL );		
			g_nDevices++;
		}
		else
			ZeroMemory(&g_devList[g_nDevices], sizeof(DEVICE));
	}

	return ( g_nDevices < ARRAYSIZE(g_devList) ) ? DIENUM_CONTINUE : DIENUM_STOP;
}

// Called by an enumerator in GetInputDevice to determine if a device is attached at all.
BOOL CALLBACK EnumIsDeviceAvailable( LPCDIDEVICEINSTANCE lpddi, LPVOID pvRef )
{
	if( lpddi->guidInstance == *(GUID*)((LPVOID*)pvRef)[0] )
	{
		*(bool*)((LPVOID*)pvRef)[1] = true;
		return DIENUM_STOP;
	}

	return DIENUM_CONTINUE;
}

// Ask DirectInput to queue state changes as well as tracking the current state, so
// LatchSubFrameTaps() can see presses that begin and end between two reads.
// The device must be unacquired for this to take.
static void SetDeviceBufferSize( LPDIRECTINPUTDEVICE8 lpDirectInputDevice )
{
	DIPROPDWORD dipdw;

	dipdw.diph.dwSize		= sizeof(DIPROPDWORD);
	dipdw.diph.dwHeaderSize	= sizeof(DIPROPHEADER);
	dipdw.diph.dwObj		= 0;
	dipdw.diph.dwHow		= DIPH_DEVICE;
	dipdw.dwData			= DEVICE_BUFFER_SIZE;

	lpDirectInputDevice->SetProperty( DIPROP_BUFFERSIZE, &dipdw.diph );
}

// Called by an axis enumerator in GetInputDevice.  Set the min and max range so we can pass the rec'd value right to the emulator.
BOOL CALLBACK EnumSetObjectsAxis( LPCDIDEVICEOBJECTINSTANCE lpddoi, LPVOID pvRef)
{
	LPDIRECTINPUTDEVICE8 lpDirectInputDevice = (LPDIRECTINPUTDEVICE8)pvRef;
	DIPROPRANGE diprg; 

	diprg.diph.dwSize       = sizeof(DIPROPRANGE);
	diprg.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	diprg.diph.dwHow        = DIPH_BYID;
	diprg.diph.dwObj        = lpddoi->dwType;
	diprg.lMin              = MINAXISVALUE;
	diprg.lMax              = MAXAXISVALUE;
	
	lpDirectInputDevice->SetProperty(DIPROP_RANGE, &diprg.diph); // HACK: Usually works, but not all devices support setting range.

	return DIENUM_CONTINUE;
}

// **if passed an existing DirectInputDevice which matches gGuid
//		unacquires it, and sets its cooperative level (reinitialize)
// **if the existing device does not match the passed gGuid, the existing device is released
// **if no device was passed or gGuid did not match
//		searches for the controller matching gGuid in connected and available devices
//		creates a DirectInputDevice
//		sets its data format
//		sets its cooperative level
//		for joysticks, calls EnumSetObjectsAxis for each axis
// GetInputDevice always leaves the returned device in an UNACQUIRED state.
bool GetInputDevice(HWND hWnd, LPDIRECTINPUTDEVICE8& lpDirectInputDevice, GUID gGuid, DWORD dwDevType, LPCDIDEVICEINSTANCE lpddi /*nullable if unavailable*/, DWORD dwCooperativeLevel)
{
	DebugWriteA("GetInputDevice: gGuid is {%08.8lX-%04.4hX-%04.4hX-%02.2X%02.2X-%02.2X%02.2X%02.2X%02.2X%02.2X%02.2X}\n", gGuid.Data1, gGuid.Data2, gGuid.Data3, gGuid.Data4[0], gGuid.Data4[1], gGuid.Data4[2], gGuid.Data4[3], gGuid.Data4[4], gGuid.Data4[5], gGuid.Data4[6], gGuid.Data4[7]);
	if( lpDirectInputDevice != NULL)
	{
		DIDEVICEINSTANCE didDev;
		didDev.dwSize = sizeof(DIDEVICEINSTANCE);
		lpDirectInputDevice->GetDeviceInfo( &didDev );

		if( didDev.guidInstance == gGuid )
		{	// we've already gotten this device; unacquire it and initialize
			DebugWriteA("GetInputDevice: already created, attempting to reinit\n");
			lpDirectInputDevice->Unacquire();
			lpDirectInputDevice->SetCooperativeLevel( hWnd, dwCooperativeLevel );
			SetDeviceBufferSize( lpDirectInputDevice );
			return true;
		}
		else
			ReleaseDevice( lpDirectInputDevice );
	}

	HRESULT hResult;
	
	LPCDIDATAFORMAT ppDiDataFormat = NULL;
	bool Success = false;

	switch( LOBYTE(dwDevType) )
	{
		
	case DI8DEVTYPE_KEYBOARD:
		ppDiDataFormat = &c_dfDIKeyboard;
		break;

	case DI8DEVTYPE_MOUSE:
		ppDiDataFormat = &c_dfDIMouse2;
		break;

	default: // assume everything else is a gamepad; probably not the best idea but it works
		ppDiDataFormat = &c_dfDIJoystick;
		break;
	}

	bool bDeviceAvailable = false;
		
	VOID* aRef[2] = { &gGuid, &bDeviceAvailable };
		
	// for each available device in our dwDevType category, run EnumIsDeviceAvailable with params "aRef"
	if (lpddi)
	{
		bDeviceAvailable = TRUE;
	}
	else
	{
		g_pDIHandle->EnumDevices(DI8DEVCLASS_ALL, EnumIsDeviceAvailable, (LPVOID)aRef, DIEDFL_ATTACHEDONLY);
	}
		
	if( !bDeviceAvailable )
	{
		DebugWriteA("GetInputDevice: Device does not appear available\n");
		return false;
	}
		
	hResult = g_pDIHandle->CreateDevice( gGuid, &lpDirectInputDevice, NULL );
	
	if( SUCCEEDED( hResult ))
	{
		hResult = lpDirectInputDevice->SetDataFormat( ppDiDataFormat );
		hResult = lpDirectInputDevice->SetCooperativeLevel( hWnd, dwCooperativeLevel );
		
		Success = SUCCEEDED( hResult );
		if (!Success)
		{
			DebugWriteA("GetInputDevice: SetCooperativeLevel failed\n");
		}
	}
	else
		DebugWriteA("GetInputDevice: CreateDevice failed\n");

	if (Success)
	{
		if (ppDiDataFormat == &c_dfDIJoystick)
			lpDirectInputDevice->EnumObjects(EnumSetObjectsAxis, lpDirectInputDevice, DIDFT_AXIS);

		SetDeviceBufferSize( lpDirectInputDevice );
	}

	return Success;
}

// How many force feedback axes (motors) does our device have?  We want to rumble them all.
BOOL CALLBACK EnumCountFFAxes( LPCDIDEVICEOBJECTINSTANCE lpddoi, LPVOID pnAxes )
{
	*(DWORD*)pnAxes += 1;
	return DIENUM_CONTINUE;
}

// Create a force feedback effect handle and downloads the effect.  Parameters are self-explanatory.
bool CreateEffectHandle( HWND hWnd, LPDIRECTINPUTDEVICE8 lpDirectInputDevice, LPDIRECTINPUTEFFECT &pDIEffect, BYTE bRumbleTyp, long lStrength )
{
	if( pDIEffect )
		ReleaseEffect( pDIEffect );

	if( !lpDirectInputDevice || bRumbleTyp == RUMBLE_DIRECT )
		return false;	

	DWORD nAxes = 0;
	DWORD rgdwAxes[] = { DIJOFS_X, DIJOFS_Y };

	HRESULT hResult;

	// count the FF - axes of the joystick
	lpDirectInputDevice->EnumObjects( EnumCountFFAxes, &nAxes, DIDFT_FFACTUATOR | DIDFT_AXIS );

	if( nAxes == 0 )
		return false;
	nAxes = min( nAxes, 2 );


	// Must be unaquired for setting stuff like Co-op Level
	hResult = lpDirectInputDevice->Unacquire();
	//FF Requires EXCLUSIVE LEVEL, took me hours to find the reason why it wasnt working
	hResult = lpDirectInputDevice->SetCooperativeLevel( hWnd, DIB_FF );

	// fail if we can't set coop level --rabid
	if (hResult != DI_OK)
	{
		DebugWriteA("CreateEffectHandle: couldn't set coop level: %08X\n", hResult);
		return false;
	}

    // Since we will be playing force feedback effects, we should disable the
    // auto-centering spring.
	DIPROPDWORD dipdw;
    dipdw.diph.dwSize       = sizeof(DIPROPDWORD);
    dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    dipdw.diph.dwObj        = 0;
    dipdw.diph.dwHow        = DIPH_DEVICE;
    dipdw.dwData            = FALSE;
	
	hResult = lpDirectInputDevice->SetProperty( DIPROP_AUTOCENTER, &dipdw.diph );

	long rglDirection[] = { 1, 1 };
	LPGUID EffectGuid;
    DIEFFECT eff;
    ZeroMemory( &eff, sizeof(eff) );

	eff.dwSize                  = sizeof(DIEFFECT);
	eff.dwFlags                 = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
	eff.dwGain                  = lStrength * 100;
    eff.dwTriggerButton         = DIEB_NOTRIGGER;
    eff.dwTriggerRepeatInterval = 0;
    eff.cAxes                   = nAxes; //Number of Axes
    eff.rgdwAxes                = rgdwAxes;
    eff.rglDirection            = rglDirection;
    eff.lpEnvelope              = NULL;
	eff.dwStartDelay            = 0;

	DICONSTANTFORCE cf;
	DIRAMPFORCE rf;
	DIPERIODIC pf;

	switch( bRumbleTyp )
	{
	case RUMBLE_CONSTANT:
		EffectGuid = (GUID*)&GUID_ConstantForce;

		eff.dwDuration              = 150000; // microseconds
		eff.dwSamplePeriod          = 0;
		eff.cbTypeSpecificParams    = sizeof(DICONSTANTFORCE);
		eff.lpvTypeSpecificParams   = &cf;

		cf.lMagnitude = 10000;
		break;
	case RUMBLE_RAMP:
		EffectGuid = (GUID*)&GUID_RampForce;
		
		eff.dwDuration              = 300000; // microseconds
		eff.dwSamplePeriod          = 0;
		eff.cbTypeSpecificParams    = sizeof(DIRAMPFORCE);
		eff.lpvTypeSpecificParams   = &rf;

		rf.lStart = 10000;
		rf.lEnd = 2000;
		break;
	
	case RUMBLE_CONDITION:
	case RUMBLE_PERIODIC:
		EffectGuid = (GUID*)&GUID_Sine;

		eff.dwDuration              = 150000; // microseconds
		eff.dwSamplePeriod          = 0;
		eff.cbTypeSpecificParams    = sizeof(DIPERIODIC);
		eff.lpvTypeSpecificParams   = &pf;

		pf.dwMagnitude = 10000;
		pf.lOffset = 0;
		pf.dwPhase = 0;
		pf.dwPeriod = 2000;
		break;

	case RUMBLE_NONE:
	case RUMBLE_CUSTOM:
	default:
		return false;
	}

	hResult = lpDirectInputDevice->CreateEffect( *EffectGuid, &eff, &pDIEffect, NULL );

	if (hResult == DI_OK)
	{
		hResult = lpDirectInputDevice->Acquire();
		hResult = pDIEffect->Download();
	}
	else
	{
		DebugWriteA("CreateEffectHandle: didn't CreateEffect: %08X\n", hResult);
	}
	return SUCCEEDED( hResult );
}

// Counts how many of each type of button assignment (keyboard, mouse, gamepad)
DWORD CountControllerStructDevs( CONTROLLER *pController )
{
	BYTE bMouse = 0, bKeyboard = 0, bGamePad = 0;
	BYTE bDType;

	bool fModifier = ( pController->pModifiers != NULL );
	int i = ( fModifier ) ? pController->nModifiers : ARRAYSIZE( pController->aButton );
	i--;

	for( ; i >= 0 || fModifier; --i )
	{
		if( i < 0 )
		{
			i = ARRAYSIZE( pController->aButton ) - 1;
			fModifier = false;
		}

		bDType = ( fModifier )	? pController->pModifiers[i].btnButton.bBtnType
								: pController->aButton[i].bBtnType;

		switch( bDType )
		{
		case DT_JOYBUTTON:
		case DT_JOYAXE:
		case DT_JOYSLIDER:
		case DT_JOYPOV:
			++bGamePad;
			break;

		case DT_KEYBUTTON:
			++bKeyboard;
			break;

		case DT_MOUSEBUTTON:
		case DT_MOUSEAXE:
			++bMouse;
			break;
		}
	}

	pController->fGamePad	= bGamePad != 0;
	pController->fKeyboard	= bKeyboard != 0;
	pController->fMouse		= bMouse != 0;

	return MAKELONG( MAKEWORD( bGamePad, bMouse ), MAKEWORD( bKeyboard, ( bMouse + bKeyboard + bGamePad )));
}


// Work out which devices anything actually reads, so GetDeviceDatas() can skip the rest.
// A typical setup has a handful of HID devices connected and bindings on one of them; polling
// the others every frame is work the emulation thread doesn't need to be doing.
void UpdateDeviceUsage()
{
	int i, j;

	for( i = 0; i < g_nDevices; ++i )
		g_devList[i].fInUse = false;
	g_sysMouse.fInUse = false;

	for( i = 0; i < ARRAYSIZE( g_pcControllers ); ++i )
	{
		if( !g_pcControllers[i].fPlugged )
			continue;		// GetKeys doesn't read an unplugged controller's assignments

		for( j = 0; j < ARRAYSIZE( g_pcControllers[i].aButton ); ++j )
			if( g_pcControllers[i].aButton[j].parentDevice )
				g_pcControllers[i].aButton[j].parentDevice->fInUse = true;

		for( j = 0; j < g_pcControllers[i].nModifiers; ++j )
			if( g_pcControllers[i].pModifiers[j].btnButton.parentDevice )
				g_pcControllers[i].pModifiers[j].btnButton.parentDevice->fInUse = true;
	}

	// Shortcuts are checked every frame no matter which controller they belong to
	for( i = 0; i < ARRAYSIZE( g_scShortcuts.Player ); ++i )
		for( j = 0; j < SC_TOTAL; ++j )
			if( g_scShortcuts.Player[i].aButtons[j].parentDevice )
				g_scShortcuts.Player[i].aButtons[j].parentDevice->fInUse = true;

	if( g_scShortcuts.bMouseLock.parentDevice )
		g_scShortcuts.bMouseLock.parentDevice->fInUse = true;

	// A device we're about to stop reading must not keep reporting whatever it last said
	for( i = 0; i < g_nDevices; ++i )
		if( !g_devList[i].fInUse )
			NeutralizeDeviceState( g_devList[i] );

	if( !g_sysMouse.fInUse )
		NeutralizeDeviceState( g_sysMouse );
}

// PrepareInputDevices rewritten --rabid
bool PrepareInputDevices()
{
	bool fKeyboard = false;
	bool fMouse = false;
	bool fGamePad = false;

	for( int i = 0; i < ARRAYSIZE( g_pcControllers ); ++i )
	{
		if( g_pcControllers[i].fPlugged )
		{
			CountControllerStructDevs( &g_pcControllers[i] );

			// OR across every plugged controller, not just whichever one happened to be last:
			// these used to be plain assignments, so a later controller with no mouse binding
			// would clear the flag for an earlier one that had them, and turn off mouse lock.
			fKeyboard |= g_pcControllers[i].fKeyboard != 0;
			fMouse |= g_pcControllers[i].fMouse != 0;
			fGamePad |= ( g_pcControllers[i].fGamePad != 0); // we'll assume for now that there's a gamepad to go with those buttons
		}

		ReleaseEffect( g_apdiEffect[i] );
		if( g_pcControllers[i].guidFFDevice != GUID_NULL && GetInputDevice( g_strEmuInfo.hMainWindow, g_apFFDevice[i], g_pcControllers[i].guidFFDevice, DI8DEVTYPE_JOYSTICK, nullptr, DIB_FF )) // not necessarily a joystick type device, but we don't use the data anyway
		{
			DIDEVICEINSTANCE diDev;
			diDev.dwSize = sizeof( DIDEVICEINSTANCE );

			g_apFFDevice[i]->GetDeviceInfo( &diDev );

			if( !lstrcmp( diDev.tszProductName, _T( STRING_ADAPTOID )))
			{
				g_pcControllers[i].fIsAdaptoid = 1;
				DebugWriteA("FF device on controller %d is of type Adaptoid\n", i+1);
			}
			else
			{
				g_pcControllers[i].fIsAdaptoid = 0;
			}

			if ( CreateEffectHandle( i, g_pcControllers[i].bRumbleTyp, g_pcControllers[i].bRumbleStrength ) )
			{
				AcquireDevice( g_apFFDevice[i] );
				DebugWriteA("Got FF device %d\n", i);
			}
			else
				DebugWriteA("Couldn't get FF device: CreateEffectHandle failed!\n");
		}
		else
		{
			ReleaseDevice( g_apFFDevice[i] );	// used to just NULL the pointer, leaking the device
			DebugWriteA("Didn't get FF device %d\n", i);
		}
	}

	if( fMouse )
	{
		if( !g_sysMouse.didHandle )
		{
			if( GetInputDevice( g_strEmuInfo.hMainWindow, g_sysMouse.didHandle, GUID_SysMouse, DI8DEVTYPE_MOUSE, nullptr, g_bExclusiveMouse ? DIB_MOUSE : DIB_KEYBOARD ))
			{
				AcquireDevice( g_sysMouse.didHandle );
			}
		}
	}
	else
	{
		g_bExclusiveMouse = false;
	}

	UpdateDeviceUsage();

	return true;
}

/////////////////////////////////////////////////////////////////////////////////
// Device hotplug
//
// DirectInput only ever shows us the devices that existed when we enumerated, and the plugin
// enumerated exactly once -- when the emulator first initialised it, which happens when a ROM
// is loaded. Anything plugged in after that stayed invisible until the emulator was restarted.
//
// A message-only window on its own thread listens for HID arrivals and departures and does
// nothing but raise a flag. The rescan itself runs on the emulation thread out of
// GetDeviceDatas(), which already holds g_critical, so no other code has to learn about
// locking and no DirectInput object is touched from two threads at once.

	// {4D1E55B2-F16F-11CF-88CB-001111000030} == GUID_DEVINTERFACE_HID.
	// Spelled out so we don't link hid.lib for a single constant.
static const GUID guidDevInterfaceHID =
	{ 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 }};

static const TCHAR szDeviceWatcherClass[] = _T("NRageDeviceWatcher");

static HANDLE g_hWatcherThread = NULL;
static DWORD g_dwWatcherThreadID = 0;
static volatile LONG g_lHotplugPending = 0;		// raised by the watcher thread, cleared by the emulation thread
static volatile LONG g_lHotplugDeadline = 0;	// tick count to rescan at; LONG so it writes atomically

static LRESULT CALLBACK DeviceWatcherWndProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	if( uMsg == WM_DEVICECHANGE && ( wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE ))
	{
		InterlockedExchange( &g_lHotplugDeadline, (LONG)( GetTickCount() + HOTPLUG_DEBOUNCE_MS ));
		InterlockedExchange( &g_lHotplugPending, 1 );
		return TRUE;
	}

	return DefWindowProc( hWnd, uMsg, wParam, lParam );
}

static DWORD WINAPI DeviceWatcherThread( LPVOID )
{
	WNDCLASS wc;
	ZeroMemory( &wc, sizeof(wc) );
	wc.lpfnWndProc = DeviceWatcherWndProc;
	wc.hInstance = g_strEmuInfo.hinst;
	wc.lpszClassName = szDeviceWatcherClass;
	RegisterClass( &wc );	// harmless if it's still registered from a previous run

	// HWND_MESSAGE: no UI, no taskbar entry, and it never appears in the emulator's window list
	HWND hWnd = CreateWindowEx( 0, szDeviceWatcherClass, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL );

	if( !hWnd )
	{
		DebugWriteA( "DeviceWatcher: CreateWindowEx failed (%lu)\n", GetLastError() );
		UnregisterClass( szDeviceWatcherClass, wc.hInstance );
		return 0;
	}

	DEV_BROADCAST_DEVICEINTERFACE filter;
	ZeroMemory( &filter, sizeof(filter) );
	filter.dbcc_size = sizeof(filter);
	filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	filter.dbcc_classguid = guidDevInterfaceHID;

	HDEVNOTIFY hNotify = RegisterDeviceNotification( hWnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE );

	if( !hNotify )
	{	// braces matter: in release builds DebugWriteA expands to more than one statement
		DebugWriteA( "DeviceWatcher: RegisterDeviceNotification failed (%lu)\n", GetLastError() );
	}

	MSG msg;
	while( GetMessage( &msg, NULL, 0, 0 ) > 0 )
	{
		TranslateMessage( &msg );
		DispatchMessage( &msg );
	}

	if( hNotify )
		UnregisterDeviceNotification( hNotify );
	DestroyWindow( hWnd );
	UnregisterClass( szDeviceWatcherClass, wc.hInstance );
	return 0;
}

// Begin watching for controllers being plugged in and unplugged. Safe to call repeatedly.
void StartDeviceWatcher()
{
	if( g_hWatcherThread )
		return;

	g_hWatcherThread = CreateThread( NULL, 0, DeviceWatcherThread, NULL, 0, &g_dwWatcherThreadID );

	if( !g_hWatcherThread )
	{	// braces matter: in release builds DebugWriteA expands to more than one statement
		DebugWriteA( "DeviceWatcher: CreateThread failed (%lu)\n", GetLastError() );
	}
	else
	{
		DebugWriteA( "DeviceWatcher: started\n" );
	}
}

void StopDeviceWatcher()
{
	if( !g_hWatcherThread )
		return;

	// PostThreadMessage can't be delivered until the thread owns a message queue, which it
	// only gets once it has touched user32, so give it a few tries before giving up.
	for( int i = 0; i < 200; ++i )
	{
		if( WaitForSingleObject( g_hWatcherThread, 0 ) != WAIT_TIMEOUT )
			break;		// already gone
		if( PostThreadMessage( g_dwWatcherThreadID, WM_QUIT, 0, 0 ))
			break;
		Sleep( 5 );
	}

	if( WaitForSingleObject( g_hWatcherThread, 2000 ) != WAIT_OBJECT_0 )
	{
		DebugWriteA( "DeviceWatcher: thread didn't shut down in time\n" );
	}

	CloseHandle( g_hWatcherThread );
	g_hWatcherThread = NULL;
	g_dwWatcherThreadID = 0;
	InterlockedExchange( &g_lHotplugPending, 0 );
}

// Re-enumerate DirectInput and pick up whatever has appeared since we last looked.
//
// Entries already in g_devList are never moved or removed: BUTTON::parentDevice points
// straight into that array, so the list may only ever be appended to. A device that gets
// unplugged keeps its slot with a dead handle, which is exactly what lets it come back later
// without the user having to rebind anything.
void RescanInputDevices()
{
	if( !g_pDIHandle )
		return;

	const int nBefore = g_nDevices;

	g_pDIHandle->EnumDevices( DI8DEVCLASS_ALL, EnumMakeDeviceList, NULL, DIEDFL_ATTACHEDONLY );

	if( g_nDevices == nBefore )
		return;		// nothing new; the arrival was something we already know about, or not an input device

	DebugWriteA( "Hotplug: picked up %d new device(s), %d total\n", g_nDevices - nBefore, g_nDevices );

	// Assignments that couldn't be bound at startup because their device wasn't connected get
	// another chance now. Only reinitialise force feedback if something actually got bound.
	if( ResolvePendingBindings() )
		PrepareInputDevices();		// ends with UpdateDeviceUsage()
	else
		UpdateDeviceUsage();		// still needed: the new device starts out unread
}

// Called once per frame from GetDeviceDatas(), on the emulation thread, with g_critical held.
static void PollDeviceHotplug()
{
	if( !g_lHotplugPending )
		return;

	// Connecting one controller fires a burst of HID notifications; one rescan per burst.
	if( (LONG)( GetTickCount() - (DWORD)g_lHotplugDeadline ) < 0 )
		return;

	InterlockedExchange( &g_lHotplugPending, 0 );
	RescanInputDevices();
}

bool IsAdaptoidCommandSupported( LPDIRECTINPUTDEVICE8 lpDirectInputDevice, DWORD cmd )
{
    DIEFFESCAPE esc;
    DWORD inbuf, outbuf;
    HRESULT hr;

	esc.dwSize = sizeof(esc);
	esc.dwCommand = ADAPT_TEST;   // command to determine if a command is supported
	esc.lpvInBuffer = &inbuf;
	esc.cbInBuffer = 4;
	esc.lpvOutBuffer = &outbuf;   
	esc.cbOutBuffer = 4;
	inbuf = cmd;                  // command that we are asking is supported
	outbuf = 0;                   

	hr = lpDirectInputDevice->Escape(&esc);

    return( SUCCEEDED(hr) && esc.cbOutBuffer == 4 && outbuf == 0xB0CAB0CA );
}

#ifdef _DEBUG
// Direct Adaptoid debugging stuff.
void _debugAd( LPCSTR szMessage, HRESULT res )
{
	LPCSTR suc = (SUCCEEDED(res)) ? "OK" : "FAILED";

	DebugWriteA( "%s: %s (RC:%08X)\n", szMessage, suc, res );
}
#endif // #ifdef _DEBUG

HRESULT DirectRumbleCommand( LPDIRECTINPUTDEVICE8 lpDirectInputDevice, DWORD cmd )
{
    DIEFFESCAPE esc;

    esc.dwSize = sizeof(esc);
    esc.dwCommand = ADAPT_RUMBLE;   // send rumble command
    esc.lpvInBuffer = &cmd;  // 1=go, 0=stop
    esc.cbInBuffer = 4;
    esc.lpvOutBuffer = NULL;   
    esc.cbOutBuffer = 0;

	HRESULT hr = lpDirectInputDevice->Escape(&esc);

#ifdef _DEBUG
	_debugAd( "Direct Adaptoid RumbleCommand", hr );
#endif // #ifdef _DEBUG

    return hr;
}

HRESULT InitializeAdaptoid( LPDIRECTINPUTDEVICE8 lpDirectInputDevice, LPBYTE status )
{
    DIEFFESCAPE esc;

    esc.dwSize = sizeof(esc);
    esc.dwCommand = ADAPT_INIT;   // Initialize Pak
    esc.lpvInBuffer = NULL;
    esc.cbInBuffer = 0;
    esc.lpvOutBuffer = status;   
    esc.cbOutBuffer = 1;

	HRESULT hr = lpDirectInputDevice->Escape(&esc);

#ifdef _DEBUG
	_debugAd( "Direct Adaptoid InitPak", hr );
#endif // #ifdef _DEBUG

    return hr;
}

HRESULT ReadAdaptoidPak( LPDIRECTINPUTDEVICE8 lpDirectInputDevice, DWORD addr, LPBYTE data )
{
    DIEFFESCAPE esc;

    esc.dwSize = sizeof(esc);
    esc.dwCommand = ADAPT_READPAK;   // Read 32 bytes from pak
    esc.lpvInBuffer = &addr;
    esc.cbInBuffer = 4;
    esc.lpvOutBuffer = data;   
    esc.cbOutBuffer = 32;

	HRESULT hr = lpDirectInputDevice->Escape(&esc);

#ifdef _DEBUG
	LPCSTR suc = (SUCCEEDED(hr)) ? "OK" : "FAILED";

	DebugWriteA( "Direct Adaptoid ReadPak(Addr:%04X): %s (RC:%08X)\n", addr, suc, hr );
#endif // #ifdef _DEBUG

    return hr;
}

HRESULT WriteAdaptoidPak( LPDIRECTINPUTDEVICE8 lpDirectInputDevice, DWORD addr, LPBYTE data )
{
    DIEFFESCAPE esc;
    struct
    {
        DWORD addr;
        BYTE data[32];
    } buf;

    buf.addr = addr;
    CopyMemory( buf.data, data, 32 );

    esc.dwSize = sizeof(esc);
    esc.dwCommand = ADAPT_WRITEPAK;   // Write 32 bytes to pak
    esc.lpvInBuffer = &buf;
    esc.cbInBuffer = 36;
    esc.lpvOutBuffer = NULL;   
    esc.cbOutBuffer = 0;
	
	HRESULT hr = lpDirectInputDevice->Escape(&esc);

#ifdef _DEBUG
	LPCSTR suc = (SUCCEEDED(hr)) ? "OK" : "FAILED";

	DebugWriteA( "Direct Adaptoid WritePak(Addr:%04X): %s (RC:%08X)\n", addr, suc, hr );
#endif // #ifdef _DEBUG

    return hr;
}

BYTE GetAdaptoidStatus( LPDIRECTINPUTDEVICE8 lpDirectInputDevice )
{
	HRESULT hr;
	BYTE bStatus = 0;
	hr = InitializeAdaptoid( lpDirectInputDevice, &bStatus );

	int iRetrys = 10;
	while( FAILED( hr ) && iRetrys > 0 )
	{
		Sleep( 5 );
		hr = AcquireDevice( lpDirectInputDevice );
		hr = InitializeAdaptoid( lpDirectInputDevice, &bStatus );
		iRetrys--;
	}

	return (SUCCEEDED(hr)) ? bStatus : RD_NOPLUGIN | RD_NOTINITIALIZED;
}

// Fill the handle for g_sysMouse properly
void InitMouse()
{
	if (GetInputDevice( g_strEmuInfo.hMainWindow, g_sysMouse.didHandle, GUID_SysMouse, DI8DEVTYPE_MOUSE, nullptr, DIB_KEYBOARD ))
	{
		g_sysMouse.guidInstance = GUID_SysMouse;
		g_sysMouse.dwDevType = DI8DEVTYPE_MOUSE;
		_tcsncpy_s(g_sysMouse.szProductName, STRING_GUID_SYSMOUSE, ARRAYSIZE(g_sysMouse.szProductName));
	}
	else
		g_sysMouse.didHandle = NULL;
}

// treat btnButton as a b_Value, and return whether it is pressed or not
bool IsBtnPressed(LPCONTROLLER pcController, BUTTON btnButton)
{
	// The assignment names a device that isn't connected (yet). ResolvePendingBindings() will
	// hook it up if the device turns up later; until then it simply reads as not pressed.
	if( !btnButton.parentDevice )
		return false;

	long l_Value;
	LPLONG plRawState = (LPLONG)&btnButton.parentDevice->stateAs.joyState;

	long AbsThreshold;
	if (pcController) {
		AbsThreshold = (MAXAXISVALUE * pcController->bPadThreshold / 100);
	}
	else {
		AbsThreshold = (RANGERELATIVE * 90 / 100);
	}

	switch ( btnButton.bBtnType )
	{
	case DT_JOYBUTTON:
		return ( btnButton.parentDevice->stateAs.joyState.rgbButtons[btnButton.bOffset] & 0x80 ) != 0
			|| btnButton.parentDevice->abLatchedPress[btnButton.bOffset] != 0;

	case DT_JOYSLIDER:
	case DT_JOYAXE:
		l_Value = plRawState[btnButton.bOffset] - ZEROVALUE;

		if ( btnButton.bAxisID )
			return ( l_Value <= -AbsThreshold );
		else
			return ( l_Value >= AbsThreshold );

	case DT_JOYPOV:
		return GetJoyPadPOV( (PDWORD)&plRawState[btnButton.bOffset] , btnButton.bAxisID );

	case DT_KEYBUTTON:
		return ( btnButton.parentDevice->stateAs.rgbButtons[btnButton.bOffset] & 0x80 ) != 0
			|| btnButton.parentDevice->abLatchedPress[btnButton.bOffset] != 0;

	case DT_MOUSEBUTTON:
		return ( btnButton.parentDevice->stateAs.mouseState.rgbButtons[btnButton.bOffset] & 0x80 ) != 0;

	case DT_MOUSEAXE:
		l_Value = MOUSEMOVE; // a.k.a. lvalue is button threshold

		if( btnButton.bAxisID )
			return ( ((LPLONG)(&btnButton.parentDevice->stateAs.mouseState))[btnButton.bOffset] < -l_Value );
		else
			return ( ((LPLONG)(&btnButton.parentDevice->stateAs.mouseState))[btnButton.bOffset] > l_Value );
		break;

	case DT_UNASSIGNED:
	default:
		return false;
	}
}
