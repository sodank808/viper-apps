//
//	ID Engine
//	ID_IN.c - Input Manager
//	v1.0d1
//	By Jason Blochowiak
//

#include "wl_def.h"

/*
=============================================================================

					GLOBAL VARIABLES

=============================================================================
*/


//
// configuration variables
//
boolean MousePresent;
boolean forcegrabmouse;


// 	Global variables
bool        Keyboard[sc_Last];
char        textinput[TEXTINPUTSIZE];
boolean	    Paused;
ScanCode	LastScan;

int JoyNumButtons;              // Viper: no joystick; the buttons are keys

bool GrabInput = false;

/*
=============================================================================

					LOCAL VARIABLES

=============================================================================
*/

static	boolean		IN_Started;

static	byte    DirTable[] =        // Quick lookup for total direction
{
    dir_NorthWest,	dir_North,	dir_NorthEast,
    dir_West,		dir_None,	dir_East,
    dir_SouthWest,	dir_South,	dir_SouthEast
};


// Viper: no mouse or joystick. The device's buttons arrive as keys
// (wolf3d_poll_key()), which is what the menus and the game read.
static int INL_GetMouseButtons (void)
{
    return 0;
}

void IN_GetJoyDelta (int *dx, int *dy)
{
    *dx = *dy = 0;
}

void IN_GetJoyFineDelta (int *dx, int *dy)
{
    *dx = *dy = 0;
}

int IN_JoyButtons (void)
{
    return 0;
}

boolean IN_JoyPresent (void)
{
    return false;
}

void IN_CenterMouse (void)
{
}

/*
===================
=
= Map certain keys to their defaults
=
===================
*/

ScanCode IN_MapKey (int key)
{
    ScanCode scan = key;

    switch (key)
    {
        case sc_KeyPadEnter: scan = sc_Enter; break;
        case sc_RShift: scan = sc_LShift; break;
        case sc_RAlt: scan = sc_LAlt; break;
        case sc_RControl: scan = sc_LControl; break;
    }

    return scan;
}

// Viper: a key from the platform, handled as SDL's key events were
static void IN_HandleKey (int key, int down)
{
    if (down)
    {
        LastScan = IN_MapKey(key);

        if (LastScan < sc_Last)
            Keyboard[LastScan] = true;
        if (LastScan == sc_Pause)
            Paused = true;
    }
    else
    {
        key = IN_MapKey(key);
        if (key < sc_Last)
            Keyboard[key] = false;
    }
}

void IN_ProcessEvents (void)
{
    int key, down;

    while (wolf3d_poll_key(&key,&down))
        IN_HandleKey (key,down);
}

void IN_WaitEvent (void)
{
    wolf3d_next_frame ();           // Viper: input comes a frame at a time
}

void IN_WaitAndProcessEvents (void)
{
    IN_WaitEvent ();
    IN_ProcessEvents ();
}

///////////////////////////////////////////////////////////////////////////
//
//	IN_Startup() - Starts up the Input Mgr
//
///////////////////////////////////////////////////////////////////////////
void IN_Startup(void)
{
	if (IN_Started)
		return;

    IN_ClearKeysDown();

    MousePresent = false;           // Viper: buttons only

    IN_Started = true;
}

///////////////////////////////////////////////////////////////////////////
//
//	IN_Shutdown() - Shuts down the Input Mgr
//
///////////////////////////////////////////////////////////////////////////
void IN_Shutdown(void)
{
	if (!IN_Started)
		return;


	IN_Started = false;
}

///////////////////////////////////////////////////////////////////////////
//
//	IN_ClearKeysDown() - Clears the keyboard array
//
///////////////////////////////////////////////////////////////////////////
void IN_ClearKeysDown(void)
{
	LastScan = sc_None;

	memset (Keyboard,0,sizeof(Keyboard));
}


void IN_ClearTextInput (void)
{
    memset (textinput,0,sizeof(textinput));
}


///////////////////////////////////////////////////////////////////////////
//
//	IN_ReadControl() - Reads the device associated with the specified
//		player and fills in the control info struct
//
///////////////////////////////////////////////////////////////////////////
void IN_ReadControl (ControlInfo *info)
{
	word buttons;
	int  dx,dy;
	int  mx,my;

	dx = dy = 0;
	mx = my = 0;
	buttons = 0;

	IN_ProcessEvents();

    if (Keyboard[sc_Home])
    {
        mx = -1;
        my = -1;
    }
    else if (Keyboard[sc_PgUp])
    {
        mx = 1;
        my = -1;
    }
    else if (Keyboard[sc_End])
    {
        mx = -1;
        my = 1;
    }
    else if (Keyboard[sc_PgDn])
    {
        mx = 1;
        my = 1;
    }

    if (Keyboard[sc_UpArrow])
        my = -1;
    else if (Keyboard[sc_DownArrow])
        my = 1;

    if (Keyboard[sc_LeftArrow])
        mx = -1;
    else if (Keyboard[sc_RightArrow])
        mx = 1;

	dx = mx * 127;
	dy = my * 127;

	info->x = dx;
	info->xaxis = mx;
	info->y = dy;
	info->yaxis = my;
	info->button0 = (buttons & 1) != 0;
	info->button1 = (buttons & (1 << 1)) != 0;
	info->button2 = (buttons & (1 << 2)) != 0;
	info->button3 = (buttons & (1 << 3)) != 0;
	info->dir = DirTable[((my + 1) * 3) + (mx + 1)];
}

///////////////////////////////////////////////////////////////////////////
//
//	IN_WaitForKey() - Waits for a scan code, then clears LastScan and
//		returns the scan code
//
///////////////////////////////////////////////////////////////////////////
ScanCode IN_WaitForKey (void)
{
	ScanCode result;

	for (result = LastScan; !result; result = LastScan)
		IN_WaitAndProcessEvents();

	LastScan = 0;

	return result;
}


///////////////////////////////////////////////////////////////////////////
//
//	IN_Ack() - waits for a button or key press.  If a button is down, upon
// calling, it must be released for it to be recognized
//
///////////////////////////////////////////////////////////////////////////

boolean	btnstate[NUMBUTTONS];

void IN_StartAck (void)
{
    int i;

    IN_ProcessEvents();
//
// get initial state of everything
//
	IN_ClearKeysDown();
	memset (btnstate,0,sizeof(btnstate));

	int buttons = IN_JoyButtons() << 4;

	if (MousePresent)
		buttons |= IN_MouseButtons();

	for (i = 0; i < NUMBUTTONS; i++, buttons >>= 1)
    {
		if (buttons & 1)
			btnstate[i] = true;
    }
}


boolean IN_CheckAck (void)
{
    int i;

    IN_ProcessEvents();
//
// see if something has been pressed
//
	if (LastScan)
		return true;

	int buttons = IN_JoyButtons() << 4;

	if (MousePresent)
		buttons |= IN_MouseButtons();

	for (i = 0; i < NUMBUTTONS; i++, buttons >>= 1)
	{
		if (buttons & 1)
		{
			if (!btnstate[i])
            {
                // Wait until button has been released
                do
                {
                    IN_WaitAndProcessEvents();

                    buttons = IN_JoyButtons() << 4;

                    if (MousePresent)
                        buttons |= IN_MouseButtons();

                } while (buttons & (1 << i));

				return true;
            }
		}
		else
			btnstate[i] = false;
	}

	return false;
}


void IN_Ack (void)
{
	IN_StartAck ();

    do
    {
        IN_WaitAndProcessEvents ();

    } while (!IN_CheckAck());
}


///////////////////////////////////////////////////////////////////////////
//
//	IN_UserInput() - Waits for the specified delay time (in ticks) or the
//		user pressing a key or a mouse button. If the clear flag is set, it
//		then either clears the key or waits for the user to let the mouse
//		button up.
//
///////////////////////////////////////////////////////////////////////////
boolean IN_UserInput (longword delay)
{
	longword	lasttime;

	lasttime = GetTimeCount();
	IN_StartAck ();

	do
	{
        IN_ProcessEvents();

		if (IN_CheckAck())
			return true;

        SDL_Delay(5);

	} while (GetTimeCount() - lasttime < delay);

	return false;
}

//===========================================================================

/*
===================
=
= IN_MouseButtons
=
===================
*/
int IN_MouseButtons (void)
{
	if (MousePresent)
		return INL_GetMouseButtons();
	else
		return 0;
}
