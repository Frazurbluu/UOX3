/*

  Ultima Offline eXperiment III (UOX3)
  UO Server Emulation Program
  
	Copyright 1997, 98 by Marcus Rating (Cironian)
	
	  This program is free software; you can redistribute it and/or modify
	  it under the terms of the GNU General Public License as published by
	  the Free Software Foundation; either version 2 of the License, or
	  (at your option) any later version.
	  
		This program is distributed in the hope that it will be useful,
		but WITHOUT ANY WARRANTY; without even the implied warranty of
		MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
		GNU General Public License for more details.
		
		  You should have received a copy of the GNU General Public License
		  along with this program; if not, write to the Free Software
		  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
		  
			* In addition to that license, if you are running this program or modified  *
			* versions of it on a public system you HAVE TO make the complete source of *
			* the version used by you available or provide people with a location to    *
			* download it.                                                              *
			
			  You can contact the author by sending email to <cironian@stratics.com>.
			  
*/

#include "uox3.h"

#include "weight.h"
#include "boats.h"
#include "books.h"
#include "cGuild.h"
#include "combat.h"
#include "msgboard.h"
#include "townregion.h"
#include "cWeather.hpp"
#include "movement.h"
#include "cRaces.h"
#include "cServerDefinitions.h"
#include "commands.h"
#include "cSpawnRegion.h"
#include "wholist.h"
#include "cMagic.h"
#include "skills.h"
#include "PageVector.h"
#include "speech.h"
#include "cVersionClass.h"
#include "ssection.h"
#include "cHTMLSystem.h"
#include "gump.h"
#include "trigger.h"
#include "mapstuff.h"
#include "cScript.h"
#include "cEffects.h"
#include "teffect.h"
#include "packets.h"
#include "fileio.h"

#if defined(__unix__)
#include <errno.h>
#include <signal.h>
#endif

#include <set>

//using namespace std;

extern cVersionClass CVC;

void Bounce( cSocket *bouncer, CItem *bouncing );
void DumpCreatures( void );

#if defined(__unix__)
	typedef void *HANDLE;
	pthread_t cons, netw;
#endif

bool cluox_io          = false;   // is cluox-IO enabled?
bool cluox_nopipe_fill = false;   // the stdin-pipe is known to be none-empty, no need to fill it.
HANDLE cluox_stdin_writeback = 0; // the write-end of the stdin-pipe

//o---------------------------------------------------------------------------o
//|	Function	-	cl_getch
//|	Programmer	-	knoxos
//o---------------------------------------------------------------------------o
//|	Purpose		-	Read a character from stdin, in a cluox compatble way.
//|                 This routine is non-blocking!
//|	Returns		-	>0 -> character read
//|                 -1 -> no char available.
//o---------------------------------------------------------------------------o
//
// now cluox is GUI wrapper over uox using stdin and stdout redirection to capture
// the console, if it is active uox can't use kbhit() to determine if there is a 
// character aviable, it can only get one directly by getch().
// However the problem arises that uox will get blocked if none is aviable.
// The solution to this problem is that cluox also hands over the second pipe-end
// of stdin so uox can write itself into this stream. To determine if a character is 
// now done that way. UOX write's itself a ZERO on the other end of the pipe, and reads
// a charecter, if it is again the same ZERO just putted in nothing was entered. However
// it is not a zero the user has entered a char.
// 
int cl_getch( void )
{
#if defined(__unix__)
	// first the linux style, don't change it's behavoir
	UI08 c = 0;
	fd_set KEYBOARD;
	FD_ZERO( &KEYBOARD );
	FD_SET( 0, &KEYBOARD );
	int s = select( 1, &KEYBOARD, NULL, NULL, &cwmWorldState->uoxtimeout );
	if( s < 0 )
	{
		Console.Error( 1, "Error scanning key press" );
		Shutdown( 10 );
	}
	if( s > 0 )
	{
		read( 0, &c, 1 );
		if( c == 0x0A )
			return -1;
	}
#else
	// now the windows one
	if( !cluox_io ) 
	{
		// uox is not wrapped simply use the kbhit routine
		if( kbhit() )
			return getch();
		else 
			return -1;
	}
	// the wiered cluox getter.
	UI08 c = 0;
	UI32 bytes_written = 0;
	int asw = 0;
	if( !cluox_nopipe_fill )
		asw = WriteFile( cluox_stdin_writeback, &c, 1, &bytes_written, NULL );
	if( bytes_written != 1 || asw == 0 ) 
	{
		Console.Warning( 1, "Using cluox-io" );
		Shutdown( 10 );
	}
	c = (UI08)fgetc( stdin );
	if( c == 0 )
	{
		cluox_nopipe_fill = false;
		return -1;
	}
#endif
	// here an actual charater is read in
	return c;
}

//o---------------------------------------------------------------------------o
//|   Function    :  void DoMessageLoop( void )
//|   Date        :  Unknown
//|   Programmer  :  Unknown
//o---------------------------------------------------------------------------o
//|   Purpose     :  Watch for messages thrown by UOX
//o---------------------------------------------------------------------------o
void DoMessageLoop( void )
{
	while( !messageLoop.Empty() )
	{
		MessagePassed tVal = messageLoop.GrabMessage();
		switch( tVal.actualMessage )
		{
		case MSG_SHUTDOWN:		cwmWorldState->SetKeepRun( false );				break;
		case MSG_COUNT:															break; 
		case MSG_WORLDSAVE:		cwmWorldState->SetOldTime( 0 );					break;
		case MSG_PRINT:			Console << tVal.data << myendl;					break;
		case MSG_RELOADJS:		Trigger->ReloadJS();	Console.PrintDone();	break;
		case MSG_CONSOLEBCAST:	consolebroadcast( tVal.data );					break;
		case MSG_PRINTDONE:		Console.PrintDone();							break;
		case MSG_PRINTFAILED:	Console.PrintFailed();							break;
		case MSG_SECTIONBEGIN:	Console.PrintSectionBegin();					break;
		case MSG_UNKNOWN:
		default:				Console.Error( 2, "Unknown message type" );		break;
		}
	}
}
//------------------------------------------------------------
//---------------------------------------------------------------------------

#undef DBGFILE
#define DBGFILE "uox3.cpp"

//	EviLDeD	-	June 21, 1999
//	Ok here is thread number one its a simple thread for the checkkey() function
#if !defined(__unix__)
CRITICAL_SECTION sc;	//
#endif
bool conthreadcloseok = false;
bool netpollthreadclose = false;
void NetworkPollConnectionThread( void *params );

#if defined(__unix__)
void *CheckConsoleKeyThread( void *params )
#else
void CheckConsoleKeyThread( void *params )
#endif
{
	messageLoop << "Thread: CheckConsoleThread has started";
	conthreadcloseok = false;
	while( !conthreadcloseok )
	{
		checkkey();
		UOXSleep( 500 );
	}
#if !defined(__unix__)
	_endthread();		// linux will kill the thread when it returns
#endif
	messageLoop << "Thread: CheckConsoleKeyThread Closed";
#if defined(__unix__)
	return NULL;
#endif
}
//	EviLDeD	-	End

#if !defined(__unix__)
///////////////////

//HANDLE hco;
//CONSOLE_SCREEN_BUFFER_INFO csbi;

///////////////////
#endif

#if defined(__unix__)

void closesocket( UOXSOCKET s )
{
	shutdown( s, 2 );
	close( s );
}
#endif

//o---------------------------------------------------------------------------o
//|   Function    :  void numtostr( int i, char *string )
//|   Date        :  Unknown
//|   Programmer  :  Unknown
//o---------------------------------------------------------------------------o
//|   Purpose     :  Wrapping of the stdlib num-2-str functions
//o---------------------------------------------------------------------------o
void numtostr( int i, char *string )
{
#if !defined(__unix__)
	itoa( i, string, 10 );
#else
	sprintf(string, "%d", i );
#endif
}

//o--------------------------------------------------------------------------o
//|	Function			-	void safeCopy(char *dest, const char *src, UI32 maxLen )
//|	Date					-	
//|	Developers		-	
//|	Organization	-	UOX3 DevTeam
//|	Status				-	Currently under development
//o--------------------------------------------------------------------------o
//|	Description		-	Safely copy a string that might be longer than the 
//|									destination Will truncate if needed, but will never copy 
//|									over too much to avoid possible crashes.
//o--------------------------------------------------------------------------o
//| Modifications	-	
//o--------------------------------------------------------------------------o
void safeCopy(char *dest, const char *src, UI32 maxLen )
{
	assert( src );
	assert( dest );
	assert( maxLen );
	
	strncpy( dest, src, maxLen );
	dest[maxLen - 1] = '\0';
}

//o--------------------------------------------------------------------------o
//|	Function			-	bool isOnline( CChar *c )
//|	Date					-	
//|	Developers		-	EviLDeD
//|	Organization	-	UOX3 DevTeam
//|	Status				-	Currently under development
//o--------------------------------------------------------------------------o
//|	Description		-	Check if the socket owning character c is still connected
//o--------------------------------------------------------------------------o
//| Modifications	-	
//o--------------------------------------------------------------------------o
bool isOnline( CChar *c )
{
	if( c == NULL )
		return false;
	if( c->IsNpc() )
		return false;
	ACCOUNTSBLOCK actbTemp = c->GetAccount();
	if(actbTemp.wAccountIndex != AB_INVALID_ID)
	{
		if(actbTemp.dwInGame == c->GetSerial() )
			return true;
	}
	Network->PushConn();
	for( cSocket *tSock = Network->FirstSocket(); !Network->FinishedSockets(); tSock = Network->NextSocket() )
	{
		if( tSock->CurrcharObj() == c )
		{
			Network->PopConn();
			return true;
		}
	}
	Network->PopConn();
	return false;
}

//o---------------------------------------------------------------------------o
//|	Function	-	void doGCollect( void )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Remove items in invalid locations (cleanup deleted packs and
//|					their items)
//o---------------------------------------------------------------------------o
void doGCollect( void )
{
	CChar *c;
	CItem *j;
	cBaseObject *iCont;
	UI32 rtotal = 0, removed = 0;
	bool idelete = false;
	cwmWorldState->SetUICurrentTime( 0 );
	
	Console << "Performing Garbage Collection...";
	do
	{
		removed = 0;
		for( ITEM i = 0; i < cwmWorldState->GetItemCount(); i++ )
		{
			if( !items[i].isFree() )
			{
				idelete = false; 
				iCont = items[i].GetCont();
				if( iCont != NULL )
				{
					idelete = true;
					if( iCont->GetObjType() == OT_CHAR ) // container is a character...verify the character??
					{
						c = (CChar *)iCont;
						if( c != NULL )
						{
							if( !c->isFree() )
								idelete = false;
						}
					} 
					else 
					{// find the container if there is one.
						j = (CItem *)iCont;
						if( j != NULL )
						{
							if( !j->isFree() )
								idelete = false;
						}
					}
				}
				if( idelete )
				{
					Items->DeleItem( &items[i] );
					removed++;
				}
			}
		}
		rtotal += removed;
	} while( removed > 0 );
	
	
	cwmWorldState->SetUICurrentTime( getclock() );
    Console << " Removed " << rtotal << " items." << myendl;
}

//o---------------------------------------------------------------------------o
//|	Function	-	void item_test( void )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Check for and correct bugged items
//o---------------------------------------------------------------------------o
void item_test( void )
{
	SERIAL serial;
	cwmWorldState->SetUICurrentTime( 0 );
	
	Console << "Starting item-consistency check...";
	for( ITEM a = 0; a < cwmWorldState->GetItemCount(); a++ )
	{
		if( !items[a].isFree() )
		{
			serial = items[a].GetSerial();
			if( serial == INVALIDSERIAL )
			{
				Console << "WARNING: item " << items[a].GetName() << " [" << a << "] has an invalid serial number" << myendl;
				continue;
			}

			if( serial == items[a].GetContSerial() )
			{
				Console << "ALERT ! item " << items[a].GetName() << " [" << a << "] [serial: " << items[a].GetSerial() << "] has dangerous container value, autocorrecting" << myendl;
				items[a].SetCont( NULL );
			}
			if( serial == items[a].GetOwner() )
			{
				Console << "ALERT ! item " << items[a].GetName() << " [" << a << "] [serial: " << items[a].GetSerial() << "] has dangerous owner value" << myendl;
				items[a].SetOwner( NULL );
			}
			if( serial != INVALIDSERIAL && serial == items[a].GetSpawn()  )
			{
				Console << "ALERT ! item " << items[a].GetName() << " [" << a << "] [serial: " << items[a].GetSerial() << "] has dangerous spawner value" << myendl;
				items[a].SetSpawn( INVALIDSERIAL, a );
				nspawnsp.Remove( serial, a );
			}
		}
	}
	cwmWorldState->SetUICurrentTime( getclock() );
	Console.PrintDone();
}

//o---------------------------------------------------------------------------o
//|	Function	-	char *RealTime( char *time_str )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Returns the real time
//o---------------------------------------------------------------------------o
char *RealTime(char *time_str)
{
	struct tm *curtime;
	time_t bintime;
	time(&bintime);
	curtime = localtime(&bintime);
	strftime(time_str, 256, "%B %d %I:%M:%S %p", curtime);
	return time_str;
}

//o---------------------------------------------------------------------------o
//|	Function	-	int makenumber( int countx )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Converts decimal string comm[count] to an integer
//o---------------------------------------------------------------------------o
int makenumber( int countx )
{
	if( comm[countx] == NULL )
		return 0;
	return makeNum( comm[countx] );
}

SI32 makeNum( const std::string *data )
{
	return makeNum( data->c_str() );
}
//o---------------------------------------------------------------------------o
//|	Function	-	SI32 makeNum( const char *s )
//|	Programmer	-	seank
//o---------------------------------------------------------------------------o
//|	Purpose		-	Converts a string to an integer
//o---------------------------------------------------------------------------o
SI32 makeNum( const char *data )
{
	if(!data/* == NULL*/)
		return 0;

	char o, h;
	// NOTE: You MUST leave ret as an unsigned 32 bit integer rather than signed, due to the stupidity of VC
	// VC will NOT cope with a string like "4294967295" if it's SI32, but will if it's UI32.  Noteably,
	// VC WILL cope if it's UI32 and the string is "-1".

	UI32 ret = 0;

	std::string s( data );
	std::istringstream ss( s );
	ss >> o;
	if( o == '0' )			// oct and hex both start with 0
	{
		ss >> h;
		if( h == 'x' || h == 'X' )
			ss >> std::hex >> ret >> std::dec;	// it's hex
		else
		{
			ss.unget();
			ss >> std::oct >> ret >> std::dec;	// it's octal
		}
	}
	else
	{
		ss.unget();
		ss >> std::dec >> ret;		// it's decimal
	}
	return ret;
}

//o---------------------------------------------------------------------------o
//|	Function	-	CItem *getPack( CChar *p )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Get characters main pack
//o---------------------------------------------------------------------------o
CItem *getPack( CChar *p )
{
	if( p == NULL ) 
		return NULL;

	CItem *i = p->GetPackItem();
	if( i != NULL )
	{
		if( i->GetCont() == p && i->GetLayer() == 0x15 )
			return i;
	}

	CItem *packItem = p->GetItemAtLayer( 0x15 );
	if( packItem != NULL )
	{
		p->SetPackItem( packItem );
		return packItem;
	}
	return NULL;
}

//o---------------------------------------------------------------------------o
//|	Function	-	void wornItems( cSocket *s, CChar *j )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Find all items a character is wearing
//o---------------------------------------------------------------------------o
void wornItems( cSocket *s, CChar *j )
{
	j->SetOnHorse( false );
	CPWornItem toSend;
	for( CItem *i = j->FirstItem(); !j->FinishedItems(); i = j->NextItem() )
	{
		if( i != NULL && !i->isFree() )
		{
			if( i->GetLayer() == 0x19 ) 
				j->SetOnHorse( true );
			toSend = (*i);
			s->Send( &toSend );
		}
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	void openPack( cSocket *s, CItem *i )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Open backpack and display contents
//o---------------------------------------------------------------------------o
void openPack( cSocket *s, CItem *i )
{
	if( i == NULL )
	{
		Console << "WARNING: openPack() couldn't find backpack: " << myendl;
		return;
	}
	CPDrawContainer contSend = (*i);
	contSend.Model( 0x3C );

	if( i->GetID( 1 ) == 0x3E )            // boats
		contSend.Model( 0x4C );
	else
	{
		switch( i->GetID() )
		{
		case 0x2006:      // coffin
			contSend.Model( 0x09 );
			break;
		case 0x0E75:      // backpack
		case 0x0E79:      // pouch
		case 0x09B0:	  // pouch
			contSend.Model( 0x3C );
			break;
		case 0x0E76:      // leather bag
			contSend.Model( 0x3D );
			break;
		case 0x0E77:      // barrel
		case 0x0E7F:      // keg
		case 0x0E83:
		case 0x0FAE:	// barrel with lids
		case 0x1AD7:	// potion kegs
		case 0x1940:	// barrel with lids
			contSend.Model( 0x3E );
			break;
		case 0x0E7A:      // square basket
			contSend.Model( 0x3F );
			break;
		case 0x0990:      // round basket
		case 0x09AC:
		case 0x09B1:
			contSend.Model( 0x41 );
			break;
		case 0x0E40:      // metal & gold chest
		case 0x0E41:      // metal & gold chest
			contSend.Model( 0x42 );
			break;
		case 0x0E7D:      // wooden box
		case 0x09AA:      // wooden box
			contSend.Model( 0x43 );
			break;
		case 0x0E3C:      // large wooden crate
		case 0x0E3D:      // large wooden crate
		case 0x0E3E:      // small wooden create
		case 0x0E3F:      // small wooden crate
		case 0x0E7E:      // wooden crate
		case 0x09A9:      // small wooden crate
			contSend.Model( 0x44 );
			break;
		case 0x2AF8:		// Shopkeeper buy, sell and sold layers
			contSend.Model( 0x47 );
			break;
		case 0x0A30:   // chest of drawers (fancy)
		case 0x0A38:   // chest of drawers (fancy)
			contSend.Model( 0x48 );
			break;
		case 0x0E42:      // wooden & gold chest
		case 0x0E43:      // wooden & gold chest
			contSend.Model( 0x49 );
			break;
		case 0x0E80:      // brass box
		case 0x09A8:      // metal box
			contSend.Model( 0x4B );
			break;
		case 0x0E7C:      // silver chest
		case 0x09AB:      // metal & silver chest
			contSend.Model( 0x4A );
			break;
			
		case 0x0A97:   // bookcase
		case 0x0A98:   // bookcase
		case 0x0A99:   // bookcase
		case 0x0A9A:   // bookcase
		case 0x0A9B:   // bookcase
		case 0x0A9C:   // bookcase
		case 0x0A9D:	// bookcase (empty)
		case 0x0A9E:	// bookcase (empty)
			contSend.Model( 0x4D );
			break;

		case 0x0A4C:   // fancy armoire (open)
		case 0x0A4D:   // fancy armoire
		case 0x0A50:   // fancy armoire (open)
		case 0x0A51:   // fancy armoire
			contSend.Model( 0x4E );
			break;
			
		case 0x0A4E:   // wooden armoire (open)
		case 0x0A4F:   // wooden armoire
		case 0x0A52:   // wooden armoire (open)
		case 0x0A53:   // wooden armoire
			contSend.Model( 0x4F );
			break;
		case 0x0A2C:   // chest of drawers (wood)
		case 0x0A34:   // chest of drawers (wood)
		case 0x0A35:   // dresser
		case 0x0A3C:   // dresser
		case 0x0A3D:   // dresser
		case 0x0A44:   // dresser
			contSend.Model( 0x51 );
			break;
		case 0x09B2:      // bank box ..OR.. backpack 2
			if( i->GetMoreX() == 1 )
				contSend.Model( 0x4A );
			else 
				contSend.Model( 0x3C );
			break;
		}
	}   
	s->Send( &contSend );
	CPItemsInContainer itemsIn( i );
	s->Send( &itemsIn );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void openCorpse( cSocket *s, SERIAL serial )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Send corpse container gump and all items it contains
//o---------------------------------------------------------------------------o
void openCorpse( cSocket *s, SERIAL serial )
{
	int count = 0, count2;
	char bpopen2[6]="\x3C\x00\x05\x00\x00";
	char display1[8]="\x89\x00\x0D\x40\x01\x02\x03";
	char display2[6]="\x01\x40\x01\x02\x03";

	CItem *i = calcItemObjFromSer( serial );
	if( i == NULL )
	{
		Console << "WARNING: openCorpse() couldn't find backpack: " << serial << myendl;
		return;
	}

	CItem *ci;
	for( ci = i->FirstItemObj(); !i->FinishedItems(); ci = i->NextItemObj() )
	{
		if( ci != NULL )
		{
			if( ci->GetLayer() != 0 && !ci->isFree() )
				count++;
		}
	}
	count2 = ( count * 5 ) + 7 + 1; // 5 bytes per object, 7 for this header and 1 for
	// terminator
	display1[1] = (UI08)(count2>>8);
	display1[2] = (UI08)(count2%256);
	display1[3] = (UI08)(serial>>24);
	display1[4] = (UI08)(serial>>16);
	display1[5] = (UI08)(serial>>8);
	display1[6] = (UI08)(serial%256);
	s->Send( display1, 7 );
	for( ci = i->FirstItemObj(); !i->FinishedItems(); ci = i->NextItemObj() )
	{
		if( ci != NULL )
		{
			if( ci->GetLayer() != 0 && !ci->isFree() )
			{
				display2[0] = ci->GetLayer();
				display2[1] = ci->GetSerial( 1 );
				display2[2] = ci->GetSerial( 2 );
				display2[3] = ci->GetSerial( 3 );
				display2[4] = ci->GetSerial( 4 );
				s->Send( display2, 5 );
			}
		}
	}
	
	// Terminate with a 0
	char nul = 0;
	s->Send( &nul, 1 );
	
	bpopen2[3] = (UI08)(count>>8);
	bpopen2[4] = (UI08)(count%256);
	count2 = ( count * 19 ) + 5;
	bpopen2[1] = (UI08)(count2>>8);
	bpopen2[2] = (UI08)(count2%256);
	s->Send( bpopen2, 5 );
	char bpitem[20]="\x40\x0D\x98\xF7\x0F\x4F\x00\x00\x09\x00\x30\x00\x52\x40\x0B\x00\x1A\x00\x00";
	for( ci = i->FirstItemObj(); !i->FinishedItems(); ci = i->NextItemObj() )
	{
		if( ci != NULL )
		{
			if( ci->GetLayer() != 0 && !ci->isFree() )
			{
				bpitem[0] = ci->GetSerial( 1 );
				bpitem[1] = ci->GetSerial( 2 );
				bpitem[2] = ci->GetSerial( 3 );
				bpitem[3] = ci->GetSerial( 4 );
				bpitem[4] = ci->GetID( 1 );
				bpitem[5] = ci->GetID( 2 );
				bpitem[7] = (UI08)(ci->GetAmount()>>8);
				bpitem[8] = (UI08)(ci->GetAmount()%256);
				bpitem[9] = (UI08)(ci->GetX()>>8);
				bpitem[10] = (UI08)(ci->GetX()%256);
				bpitem[11] = (UI08)(ci->GetY()>>8);
				bpitem[12] = (UI08)(ci->GetY()%256);
				bpitem[13] = (UI08)(serial>>24);
				bpitem[14] = (UI08)(serial>>16);
				bpitem[15] = (UI08)(serial>>8);
				bpitem[16] = (UI08)(serial%256);
				bpitem[17] = ci->GetColour( 1 );
				bpitem[18] = ci->GetColour( 2 );
				bpitem[19] = 0;
				ci->SetDecayTime( 0 );// reseting the decaytimer in the backpack  //moroallan
				s->Send( bpitem, 19 );
			}
		}
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	void sendPackItem( cSocket *s, cItem *i )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Update a single item in a backpack
//o---------------------------------------------------------------------------o
void sendPackItem( cSocket *s, CItem *i )
{
	CPAddItemToCont itemSend = (*i);

	CChar *mChar = s->CurrcharObj();

	if( i->GetGlow() > 0 )
		Items->GlowItem( i );

	if( mChar->IsGM() && i->GetID() == 0x1647 )
	{
		itemSend.Model( 0x0A0F );
		itemSend.Colour( 0x00C6 );
	}
	i->SetDecayTime( 0 );

	CChar *c = getPackOwner( i );
	if( c == NULL )
	{
		CItem *j = getRootPack( i );
		if( j != NULL && itemInRange( mChar, j ) )
			s->Send( &itemSend );
	}
	else if( charInRange( mChar, c ) )
		s->Send( &itemSend );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void sendItem( cSocket *s, CItem *i )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Send items on the ground
//o---------------------------------------------------------------------------o
void sendItem( cSocket *s, CItem *i )
{	
	if( i == NULL || s == NULL ) 
		return;

	CChar *mChar = s->CurrcharObj();
	if( i->GetVisible() > 1 && !mChar->IsGM() )
		return;

	bool pack = false;
	UI08 itmput[20] = "\x1A\x00\x13\x40\x01\x02\x03\x20\x42\x00\x32\x06\x06\x06\x4A\x0A\x00\x00\x00";

	if( i->GetGlow() > 0 )
		Items->GlowItem( i );

	if( i->GetCont( 1 ) != 255 )
	{
		pack = true;
		if( i->GetCont( 1 ) < 0x40 )
		{
			cBaseObject *iCont = i->GetCont();
			if( iCont == NULL ) 
				return;
			CChar *j = (CChar *)iCont;
			if( j != NULL && j == iCont )
				pack = false;
		}
		if( pack )
		{
			if( i->GetID( 1 ) < 0x40 ) // Client crashfix, no show multis in BP
			{
				sendPackItem( s, i );
				return;
			}
		}
	}
	
	if( i->GetCont() == NULL && itemInRange( mChar, i ) )
	{
		itmput[3] = (UI08)((i->GetSerial( 1 ) )+0x80); // Enable Piles
		itmput[4] = i->GetSerial( 2 );
		itmput[5] = i->GetSerial( 3 );
		itmput[6] = i->GetSerial( 4 );

		// if player is a gm, this item
		// is shown like a candle (so that he can move it),
		// ....if not, the item is a normal
		// invisible light source!
		if( mChar->IsGM() && i->GetID() == 0x1647 ) // items[i].id1 == 0x16 && items[i].id2 == 0x47 )
		{
			itmput[7] = 0x0A;
			itmput[8] = 0x0F;
		}
		else
		{
			itmput[7] = i->GetID( 1 );
			itmput[8] = i->GetID( 2 );
		}

		itmput[9] = (UI08)(i->GetAmount()>>8);
		itmput[10] = (UI08)(i->GetAmount()%256);
		itmput[11] = (UI08)(i->GetX()>>8);
		itmput[12] = (UI08)(i->GetX()%256);
		itmput[13] = (UI08)((i->GetY()>>8) + 0xC0); // Enable Dye and Move
		itmput[14] = (UI08)(i->GetY()%256);
		itmput[15] = i->GetZ();
		if( mChar->IsGM() && i->GetID() == 0x1647 ) // items[i].id1 == 0x16 && items[i].id2 == 0x47 )
		{
			itmput[16] = 0x00;
			itmput[17] = 0xC6;
		}
		else
		{
			itmput[16] = i->GetColour( 1 );
			itmput[17] = i->GetColour( 2 );
		}
		itmput[18] = 0;
		if( i->GetVisible() > 0 )
		{
			if( !mChar->IsGM() && ( i->GetVisible() == 2 || ( i->GetVisible() == 1 && mChar != i->GetOwnerObj() ) ) )
				return;
			itmput[18] |= 0x80;
		}
		if( i->GetMovable() == 1 ) 
			itmput[18]+=0x20;
		else if( mChar->AllMove() ) 
			itmput[18]+=0x20;
		else if( i->IsLockedDown() && mChar == i->GetOwnerObj() )
			itmput[18]+=0x20;
		if( mChar->ViewHouseAsIcon() )
		{
			if( i->GetID( 1 ) >= 0x40 )
			{
				itmput[7] = 0x14;
				itmput[8] = 0xF0;
			}
		}
		UI08 dir = 0;
		if( i->GetDir() )
		{
			dir = 1;
			itmput[19] = itmput[18];
			itmput[18] = itmput[17];
			itmput[17] = itmput[16];
			itmput[16] = itmput[15];
			itmput[15] = i->GetDir();
			itmput[2] = 0x14;
			itmput[11] += 0x80;
		}
		itmput[2] = (UI08)(0x13 + dir);
		s->Send( itmput, 19 + dir );
		if( i->GetID() == 0x2006 )
			openCorpse( s, i->GetSerial() );
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	void target( cSocket *s, UI08 a1, UI08 a2, UI08 a3, UI08 a4, const char *txt )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Send targeting cursor to client
//o---------------------------------------------------------------------------o
void target( cSocket *s, UI08 targType, UI08 targID, const char *txt )
{
	target( s, calcserial( 0, 1, targType, targID ), txt );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void target( cSocket *s, UI08 a1, UI08 a2, UI08 a3, UI08 a4, SI32 dictEntry )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Send targeting cursor to client
//o---------------------------------------------------------------------------o
void target( cSocket *s, UI08 targType, UI08 targID, SI32 dictEntry )
{
	if( s == NULL )
		return;
	target( s, calcserial( 0, 1, targType, targID ), Dictionary->GetEntry( dictEntry, s->Language() ) );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void target( cSocket *s, SERIAL ser, const char *txt )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Send targeting cursor to client
//o---------------------------------------------------------------------------o
void target( cSocket *s, SERIAL ser, const char *txt )
{
	CPTargetCursor toSend;
	toSend.ID( ser );
	toSend.Type( 1 );
	s->TargetOK( true );
	sysmessage( s, txt );
	s->Send( &toSend );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void explodeItem( cSocket *mSock, CItem *nItem )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Explode an item
//o---------------------------------------------------------------------------o
void explodeItem( cSocket *mSock, CItem *nItem )
{
	CChar *c = mSock->CurrcharObj();
	UI32 dmg = 0;
	UI32 dx, dy, dz;
	// - send the effect (visual and sound)
	if( nItem->GetCont() != NULL )
	{
		Effects->staticeffect( c, 0x36B0, 0x00, 0x09 );
		nItem->SetCont( NULL );
		nItem->SetLocation(  c );
		Effects->PlaySound( c, 0x0207 );
	}
	else
	{
		Effects->staticeffect( nItem, 0x36B0, 0x00, 0x09, 0x00);
		Effects->PlaySound( nItem, 0x0207 );
	}
	UI32 len = nItem->GetMoreX() / 250; //4 square max damage at 100 alchemy
	dmg = RandomNum( nItem->GetMoreZ() * 5, nItem->GetMoreZ() * 10 );
	
	if( dmg < 5 ) 
		dmg = RandomNum( 5, 10 );  // 5 points minimum damage
	if( len < 2 ) 
		len = 2;  // 2 square min damage range
	int xOffset = MapRegion->GetGridX( nItem->GetX() );
	int yOffset = MapRegion->GetGridY( nItem->GetY() );
	
	UI08 worldNumber = nItem->WorldNumber();

	for( dx = 0xffffffff; dx <= 1; dx++ )
	{
		for( dy = 0xffffffff; dy <= 1; dy++ )
		{
			SubRegion *Cell = MapRegion->GetGrid( xOffset+dx, yOffset+dy, worldNumber );
			bool chain = false;
	
			Cell->PushChar();
			Cell->PushItem();
			for( CChar *tempChar = Cell->FirstChar(); !Cell->FinishedChars(); tempChar = Cell->GetNextChar() )
			{
				dx = abs( tempChar->GetX() - nItem->GetX() );
				dy = abs( tempChar->GetY() - nItem->GetY() );
				dz = abs( tempChar->GetZ() - nItem->GetZ() );
				if( dx <= len && dy <= len && dz <= len )
				{
					if( !tempChar->IsGM() && !tempChar->IsInvulnerable() && ( tempChar->IsNpc() || isOnline( tempChar ) ) )
					{
						if( tempChar->IsInnocent() && tempChar->GetSerial() != c->GetSerial() )
							criminal( c );
						tempChar->SetHP( (UI16)( tempChar->GetHP() - ( dmg + ( 2 - min( dx, dy ) ) ) ) );
						updateStats( tempChar, 0 );
						if( tempChar->GetHP() <= 0 )
							doDeathStuff( tempChar );
						else
							npcAttackTarget( tempChar, c );
					}
				}
			}
			Cell->PopChar();
			for( CItem *tempItem = Cell->FirstItem(); !Cell->FinishedItems(); tempItem = Cell->GetNextItem() )
			{
				if( tempItem->GetID() == 0x0F0D && tempItem->GetType() == 19 )
				{
					dx = abs( nItem->GetX() - tempItem->GetX() );
					dy = abs( nItem->GetY() - tempItem->GetY() );
					dz = abs( nItem->GetZ() - tempItem->GetZ() );
			
					if( dx <= 2 && dy <= 2 && dz <= 2 && !chain ) // only trigger if in a 2*2*2 cube
					{
						if( !( dx == 0 && dy == 0 && dz == 0 ) )
						{
							if( RandomNum( 0, 1 ) == 1 ) 
								chain = true;
							Effects->tempeffect( c, tempItem, 17, 0, 1, 0 );
						}
					}
				}
			}
			Cell->PopItem();
		}
	}
	Items->DeleItem( nItem );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void updateStats( CChar *c, char x )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Updates characters stats
//o---------------------------------------------------------------------------o
void updateStats( CChar *c, char x)
{
	CPUpdateStat toSend( (*c), x );
	Network->PushConn();
	for( cSocket *tSock = Network->FirstSocket(); !Network->FinishedSockets(); tSock = Network->NextSocket() )
	{
		if( charInRange( tSock->CurrcharObj(), c ) )
			tSock->Send( &toSend );
	}
	Network->PopConn();
}

//o---------------------------------------------------------------------------o
//|	Function	-	void statwindow( cSocket *s, CChar *i )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Opens the status window
//o---------------------------------------------------------------------------o
void statwindow( cSocket *s, CChar *i )
{
	if( s == NULL || i == NULL ) 
		return;

	CPStatWindow toSend = (*i);
	
	CChar *mChar = s->CurrcharObj();
	//Zippy 9/17/01 : fixed bug of your name on your own stat window
	toSend.NameChange( mChar != i && ( mChar->IsGM() || i->GetOwnerObj() == mChar ) );
	toSend.Gold( calcGold( i ) );
	toSend.AC( Combat->calcDef( i, 0, false ) );
	toSend.Weight( (UI16)(i->GetWeight() / 100) );
	s->Send( &toSend );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void updates( cSocket *s )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Opens the Updates window
//o---------------------------------------------------------------------------o
void updates( cSocket *s )
{
	if( s == NULL )
		return;
	ScriptSection *Updates = FileLookup->FindEntry( "MOTD", misc_def );
	if( Updates == NULL )
		return;

	char updateData[2048];
	const char *tag = NULL;
	const char *data = NULL;
	updateData[0] = 0;
	for( tag = Updates->First(); !Updates->AtEnd(); tag = Updates->Next() )
	{
		data = Updates->GrabData();
		sprintf( updateData, "%s%s %s ", updateData, tag, data );
	}
	CPUpdScroll toSend( 2 );
	toSend.AddString( updateData );
	toSend.Finalize();
	s->Send( &toSend );
}

//o---------------------------------------------------------------------------o
//|   Function    :  bool autoStack( cSocket *mSock, CItem *i, CItem *pack )
//|   Date        :  8/14/01
//|   Programmer  :  Zane
//|	  Modified	  :	 Abaddon, 9th September, 2001, returns true if item deleted
//o---------------------------------------------------------------------------o
//|   Purpose     :  Searches pack for pileable items that match the item being
//|					 dropped into said pack (only if it's pileable), if found
//|					 ensures the amount won't go over 65535 (the limit how large
//|					 an item can stack) then stacks it. If the item is not stackable
//|					 or it cannot stack the item with a pile and have an amount that
//|					 is <= 65355 then it creates a new pile.
//|									
//|	Modification	-	09/25/2002	-	Brakthus - Weight fixes
//o---------------------------------------------------------------------------o
bool doStacking( cSocket *mSock, CChar *mChar, CItem *i, CItem *stack )
{
	UI32 newAmt = stack->GetAmount() + i->GetAmount();
	if( newAmt > MAX_STACK )
	{
		i->SetAmount( ( newAmt - MAX_STACK ) );
		stack->SetAmount( MAX_STACK );
		RefreshItem( stack );
	}
	else
	{
		CPRemoveItem toRemove = (*i);
		Network->PushConn();
		for( cSocket *tSock = Network->FirstSocket(); !Network->FinishedSockets(); tSock = Network->NextSocket() )
			tSock->Send( &toRemove );
		Network->PopConn();

		stack->SetAmount( newAmt );
		Items->DeleItem( i );
		RefreshItem( stack );
		if( mSock != NULL )
		{
			statwindow( mSock, mChar );
			Effects->itemSound( mSock, stack, false );
		}
		return true;
	}
	return false;
}
bool autoStack( cSocket *mSock, CItem *i, CItem *pack )
{
	if( !mSock ) 
		return false;
	CChar *mChar = mSock->CurrcharObj();
	if( mChar == NULL || i == NULL || pack == NULL )
		return false;

	i->SetCont( pack );
	if( i->isPileable() )
	{
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->subtractItemWeight( mChar, i );
		for( CItem *stack = pack->FirstItemObj(); !pack->FinishedItems(); stack = pack->NextItemObj() )
		{
			if( stack == NULL )
				continue;

			if( stack->isPileable() && stack->GetSerial() != i->GetSerial() &&
				stack->GetID() == i->GetID() && stack->GetColour() == i->GetColour() &&
				stack->GetAmount() < MAX_STACK )
			{ // Autostack
				if( doStacking( mSock, mChar, i, stack ) )
					return true;
			}
		}
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->addItemWeight( mChar, i );
	}
	i->SetX( (UI16)( 20 + RandomNum( 0, 79 ) ) );
	i->SetY( (UI16)( 40 + RandomNum( 0, 99 ) ) );
	i->SetZ( 9 );

	CPRemoveItem toRemove = (*i);
	Network->PushConn();
	for( cSocket *tSock = Network->FirstSocket(); !Network->FinishedSockets(); tSock = Network->NextSocket() )
		tSock->Send( &toRemove );
	Network->PopConn();

	RefreshItem( i );

	if( mSock != NULL )
	{
		statwindow( mSock, mChar );
		Effects->itemSound( mSock, i, false );
	}
	return false;
}

//o---------------------------------------------------------------------------o
//|   Function    :  void grabItem( cSocket *s )
//|   Date        :  Unknown
//|   Programmer  :  Unknown
//o---------------------------------------------------------------------------o
//|   Purpose     :  Called when a player picks up an item
//o---------------------------------------------------------------------------o
void grabItem( cSocket *mSock )
{
	SERIAL serial = mSock->GetDWord( 1 );
	if( serial == INVALIDSERIAL ) 
		return;

	CItem *i = calcItemObjFromSer( serial );
	if( i == NULL ) 
		return;

	CChar *mChar = mSock->CurrcharObj();

	mChar->BreakConcentration( mSock );
	CPBounce bounce( 0 );

	CItem *x = i;
	CChar *npc = NULL;
	cBaseObject *iCont = i->GetCont();
	if( iCont != NULL )  //Find character owning item
	{
		mSock->PickupSerial( i->GetContSerial() );
		if( iCont->GetObjType() == OT_CHAR )
			mSock->PickupSpot( PL_PAPERDOLL );
		else
		{
			if( getPackOwner( i ) != mChar )
				mSock->PickupSpot( PL_OTHERPACK );
			else
				mSock->PickupSpot( PL_OWNPACK );
		}
		CChar *npc = getPackOwner( i );
		if( npc != NULL )
		{
			if( !mChar->IsGM() && npc != mChar && npc->GetOwnerObj() != mChar )
			{
				mSock->Send( &bounce );
				return;
			}
		}
		else
		{
			x = getRootPack( i );
			if( x != NULL )
			{
				if( x->isCorpse() )
				{
					CChar *corpseTargChar = (CChar *)x->GetOwnerObj();
					if( corpseTargChar != NULL )
					{
						if( corpseTargChar->IsGuarded() ) // Is the corpse being guarded?
							petGuardAttack( mChar, corpseTargChar, corpseTargChar->GetSerial() );
						else if( x->isGuarded() )
							petGuardAttack( mChar, corpseTargChar, x->GetSerial() );
					}
				}
				else if( x->GetLayer() == 0 && x->GetID() == 0x1E5E ) // Trade Window
				{
					serial = x->GetMoreB();
					if( serial == INVALIDSERIAL ) 
						return;
					CItem *z = calcItemObjFromSer( serial );
					if( z != NULL )
					{
						if( z->GetMoreZ() || x->GetMoreZ() )
						{
							z->SetMoreZ( 0 );
							x->SetMoreZ( 0 );
							sendTradeStatus( z, x );
						}
						// Default item pick up sound sent to other player involved in trade
						cSocket *zSock = calcSocketObjFromChar( (CChar *)z->GetCont() );
						if( zSock != NULL )
							Effects->PlaySound( zSock, 0x0057, false );
					}
				}
			}
		}
	}
	else
	{
		mSock->PickupSpot( PL_GROUND );
		mSock->PickupLocation( i->GetX(), i->GetY(), i->GetZ() );
	}

	if( i->isCorpse() || !checkItemRange( mChar, x, 3 ) )
	{
		mSock->Send( &bounce );
		return;
	}

	if( x->GetMultiObj() != NULL )
	{
		if( ( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND ) && x->GetMultiObj() != mChar->GetMultiObj() ) 
		{
			mSock->Send( &bounce );
			return;
		}
		i->SetMulti( INVALIDSERIAL );
	}

	if( i->isDecayable() )
		i->SetDecayTime( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetSystemTimerStatus( DECAY ) ) );

	if( iCont != NULL )
	{
		if( iCont->GetObjType() == OT_CHAR )
		{
			CChar *pChar = (CChar *)iCont;
			if( pChar ) 
				pChar->TakeOffItem( i->GetLayer() );
		} 
		else 
		{
			CItem *pItem = (CItem *)iCont;
			if( pItem ) 
				pItem->ReleaseItem( i );
		}
	}

	if( i->isGuarded() )
	{
		if( npc != NULL && mSock->PickupSpot() == PL_OTHERPACK )
			petGuardAttack( mChar, npc, i->GetSerial() );

		CChar *petGuard = Npcs->getGuardingPet( mChar, i->GetSerial() );
		if( petGuard != NULL )
			petGuard->SetGuarding( INVALIDSERIAL );
		i->SetGuarded( false );
	}

	CTile tile;
	Map->SeekTile( i->GetID(), &tile );
	if( !mChar->AllMove() && ( i->GetMovable() == 2 || ( i->IsLockedDown() && i->GetOwnerObj() != mChar ) ||
		( tile.Weight() == 255 && i->GetMovable() != 1 ) ) )
	{
		mSock->Send( &bounce );
		if( i->GetID( 1 ) >= 0x40 )
			sendItem( mSock, i );
	}
	else
	{
		Effects->PlaySound( mSock, 0x0057, true );
		if( i->GetAmount() > 1 )
		{
			UI16 amount = mSock->GetWord( 5 );
			if( amount > i->GetAmount() ) 
				amount = i->GetAmount();
			if( amount < i->GetAmount() )
			{
				CItem *c = i->Dupe();
				if( c != NULL )
				{
					c->SetAmount( i->GetAmount() - amount );
					c->SetCont( i->GetCont() );
					if( c->GetSpawnObj() != NULL )
						nspawnsp.AddSerial( c->GetSpawn(), calcItemFromSer( c->GetSerial() ) );
					RefreshItem( c );
				}
			}
			i->SetAmount( amount );
			if( i->GetID() == 0x0EED )
			{
				if( mSock->PickupSpot() == PL_OWNPACK )
					statwindow( mSock, mChar );
			}
		}
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->addItemWeight( mChar, i );
		MapRegion->RemoveItem( i );
		CPRemoveItem remove( *i );
		Network->PushConn();
		for( cSocket *pSock = Network->FirstSocket(); !Network->FinishedSockets(); pSock = Network->NextSocket() )
			pSock->Send( &remove );
		Network->PopConn();
	}
	if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
		statwindow( mSock, mChar );
}

//o---------------------------------------------------------------------------o
//|   Function    :  void wearItem( cSocket *s )
//|   Date        :  Unknown
//|   Programmer  :  Unknown
//o---------------------------------------------------------------------------o
//|   Purpose     :  Called when an item is dropped on a players paperdoll
//o---------------------------------------------------------------------------o
void wearItem( cSocket *mSock )
{
	CChar *mChar = mSock->CurrcharObj();

	SERIAL cserial = mSock->GetDWord( 6 );
	SERIAL iserial = mSock->GetDWord( 1 );
	if( cserial == INVALIDSERIAL || iserial == INVALIDSERIAL ) 
		return;
	
	CPBounce bounce( 5 );
	CItem *i = calcItemObjFromSer( iserial );
	CChar *k = calcCharObjFromSer( cserial );
	if( i == NULL )
		return;

	if( !mChar->IsGM() && k != mChar )	// players cant equip items on other players or npc`s paperdolls.  // GM PRIVS
	{
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
		{
			Weight->subtractItemWeight( mChar, i );
			statwindow( mSock, mChar );
		}
		Bounce( mSock, i );
		sysmessage( mSock, 1186 );
		RefreshItem( i );
		return;
	}
/*
	if( i->GetCont() != NULL )
	{
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
		{
			Weight->subtractItemWeight( mChar, i );
			statwindow( mSock, mChar );
		}
		Bounce( mSock, i );
		RefreshItem( i );
		return;
	}
*/
	if( mChar->IsDead() )
	{
		sysmessage( mSock, 1185 );
		return;
	}
	if( k == NULL ) 
		return;

	ARMORCLASS ac1 = Races->ArmorRestrict( k->GetRace() );
	ARMORCLASS ac2 = i->GetArmourClass();

	if( ac1 != 0 && ( (ac1&ac2) == 0 ) )	// bit comparison, if they have ANYTHING in common, they can wear it
	{
		sysmessage( mSock, 1187 );
		Bounce( mSock, i );
		RefreshItem( i );
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
		{
			Weight->subtractItemWeight( mChar, i );
			statwindow( mSock, mChar );
		}
		return;
	}
	if( k == mChar )
	{
		bool canWear = false;
		if( i->GetStrength() > k->GetStrength() )
			sysmessage( mSock, 1188 );
		else if( i->GetDexterity() > k->GetDexterity() )
			sysmessage( mSock, 1189 );
		else if( i->GetIntelligence() > k->GetIntelligence() )
			sysmessage( mSock, 1190 );
		else
			canWear = true;
		if( !canWear )
		{
			Bounce( mSock, i );
			RefreshItem( i );

			if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			{
				Weight->subtractItemWeight( mChar, i );
				statwindow( mSock, mChar );
				Effects->itemSound( mSock, i, true );
			}
			else
				Effects->itemSound( mSock, i, false );
			RefreshItem( i );
			return;
		}
	}
	CTile tile;
	Map->SeekTile( i->GetID(), &tile);
	if( !mChar->AllMove() && ( i->GetMovable() == 2 || ( i->IsLockedDown() && i->GetOwnerObj() != mChar ) ||
		( tile.Weight() == 255 && i->GetMovable() != 1 ) ) )
	{
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->subtractItemWeight( mChar, i );

		Bounce( mSock, i );
		RefreshItem( i );
		return;
	}

	if( i->GetLayer() == 0 )
		i->SetLayer( mSock->GetByte( 5 ) );

	// 1/13/2003 - Xuri - Fix for equiping an item to more than one hand, or multiple equiping.
	CItem *j = k->GetItemAtLayer( i->GetLayer() );
	if( j != NULL )
	{
		sysmessage( mSock, "You can't equip two items in the same slot." );
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->subtractItemWeight( mChar, i );
		statwindow( mSock, mChar );
		Bounce( mSock, i );
		RefreshItem( i );
		return;
	}

	if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
		Weight->subtractItemWeight( mChar, i ); // SetCont() adds the weight for us (But we also had to add the weight in grabItem() since it sets cont as INVALIDSERIAL
	i->SetCont( k );

	if( cwmWorldState->GetDisplayLayers() ) 
		Console << "Item equipped on layer " << i->GetLayer() << myendl;

	CPRemoveItem toRemove = (*i);

	Network->PushConn();
	for( cSocket *tSock = Network->FirstSocket(); !Network->FinishedSockets(); tSock = Network->NextSocket() )
		tSock->Send( &toRemove );
	Network->PopConn();

	RefreshItem( i );

	Effects->PlaySound( mSock, 0x0057, false );
	statwindow( mSock, mChar );
}

//o---------------------------------------------------------------------------o
//|   Function    :  void dropItemOnChar( cSocket *mSock, CChar *targChar, CItem *i )
//|   Date        :  Unknown
//|   Programmer  :  Unknown
//|	  Modified	  :	 Abaddon, September 14th, 2001, returns true if item deleted
//o---------------------------------------------------------------------------o
//|   Purpose     :  Called when an item is dropped on a character
//|									
//|	Modification	-	09/25/2002	-	Xuri/Brakthus - Weight fixes
//o---------------------------------------------------------------------------o
bool dropItemOnChar( cSocket *mSock, CChar *targChar, CItem *i )
{
	CChar *mChar = mSock->CurrcharObj();
	if( mChar == NULL )
		return false;

	bool stackDeleted = false;
	if( targChar == mChar )
	{
		CItem *pack = getPack( mChar );
		if( pack == NULL ) // if player has no pack, put it at its feet
		{
			i->SetCont( NULL );
			i->SetLocation( mChar );
			RefreshItem( i );
		} 
		else
			stackDeleted = autoStack( mSock, i, pack );
	}
	else if( targChar->IsNpc() )
	{
		if( !( targChar->GetTaming() > 1000 || targChar->GetTaming() == 0 ) && i->GetType() == 14 && 
			targChar->GetHunger() <= (SI32)( targChar->ActualStrength() / 10 ) ) // do food stuff
		{
			Effects->PlaySound( mSock, 0x003A + RandomNum( 0, 2 ), true );
			npcAction( targChar, 3 );

			if( i->GetPoisoned() && targChar->GetPoisoned() < i->GetPoisoned() )
			{
				Effects->PlaySound( mSock, 0x0246, true ); //poison sound - SpaceDog
				targChar->SetPoisoned( i->GetPoisoned() );
				targChar->SetPoisonWearOffTime( BuildTimeValue( static_cast<R32>(cwmWorldState->ServerData()->GetSystemTimerStatus( POISON )) ) );
				targChar->SendToSocket( mSock, true, mChar );
			}
			//Remove a food item
			i = DecreaseItemAmount( i );
			targChar->SetHunger( targChar->GetHunger() + 1 );
			if( i == NULL )
				return true; //stackdeleted
		}
		if( !targChar->isHuman() )
		{
			// Sept 25, 2002 - Xuri - Weight fixes
			if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
				Weight->subtractItemWeight( mChar, i );

			Bounce( mSock, i );
			RefreshItem( i );
		}
		else if( mChar->GetTrainer() != targChar->GetSerial() )
		{
			// Sept 25, 2002 - Xuri - weight fix
			if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
				Weight->subtractItemWeight( mChar, i );

			npcTalk( mSock, targChar, 1197, false );
			Bounce( mSock, i );
			RefreshItem( i );
		}
		else // This NPC is training the player
		{
			if( i->GetID() == 0x0EED ) // They gave the NPC gold
			{
				UI08 trainedIn = targChar->GetTrainingPlayerIn();
				npcTalk( mSock, targChar, 1198, false );
				UI16 oldskill = mChar->GetBaseSkill( trainedIn ); 
				mChar->SetBaseSkill( (UI16)( mChar->GetBaseSkill( trainedIn ) + i->GetAmount() ), trainedIn );
				if( mChar->GetBaseSkill( trainedIn ) > 250 ) 
					mChar->SetBaseSkill( 250, trainedIn );
				Skills->updateSkillLevel( mChar, trainedIn );
				updateskill( mSock, trainedIn );
				UI16 getAmount = i->GetAmount();
				if( i->GetAmount() > 250 ) // Paid too much
				{
					i->SetAmount( i->GetAmount() - 250 - oldskill );
					if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
						Weight->subtractItemWeight( mChar, i );
					Bounce( mSock, i );
					RefreshItem( i );
				}
				else  // Gave exact change
				{
					if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
						Weight->subtractItemWeight( mChar, i );
					Items->DeleItem( i );
					stackDeleted = true;
				}
				mChar->SetTrainer( INVALIDSERIAL );
				targChar->SetTrainingPlayerIn( 255 );
				Effects->goldSound( mSock, getAmount, false );
			}
			else // Did not give gold
			{
				if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
					Weight->subtractItemWeight( mChar, i );
				npcTalk( mSock, targChar, 1199, false );
				Bounce( mSock, i );
				RefreshItem( i );
			}
		}
	}
	else // Trade stuff
	{
		if( isOnline( targChar ) )
		{
			CItem *j = startTrade( mSock, targChar );
			if ( j )
			{
				i->SetCont( j );
				i->SetX( 30 );
				i->SetY( 30 );
				i->SetZ( 9 );
				CPRemoveItem toRemove = (*i);

				Network->PushConn();
				for( cSocket *bSock = Network->FirstSocket(); !Network->FinishedSockets(); bSock = Network->NextSocket() )
					bSock->Send( &toRemove );
				Network->PopConn();
		
				RefreshItem( i );
			}
		}
		else if( mChar->GetCommandLevel() >= CNS_CMDLEVEL )
		{
			CItem *p = getPack( targChar );
			if( p == NULL )
			{
				if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
					Weight->subtractItemWeight( mChar, i );
				Bounce( mSock, i );
				return stackDeleted;
			}
			CPRemoveItem toRemove = (*i);
			Network->PushConn();
			for( cSocket *tSock = Network->FirstSocket(); !Network->FinishedSockets(); tSock = Network->NextSocket() )
				tSock->Send( &toRemove );
			Network->PopConn();

			stackDeleted = autoStack( calcSocketObjFromChar( targChar ), i, p );
			if( !stackDeleted )
				RefreshItem( i );
		}
		else
		{
			if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
				Weight->subtractItemWeight( mChar, i );
			Bounce( mSock, i );
		}
	}
	return stackDeleted;
}

//o---------------------------------------------------------------------------o
//|   Function    :  void dropItem( cSocket *s )
//|   Date        :  Unknown
//|   Programmer  :  Unknown
//o---------------------------------------------------------------------------o
//|   Purpose     :  Called when an item is dropped on the ground
//o---------------------------------------------------------------------------o
void dropItem( cSocket *mSock ) // Item is dropped on ground
{
	CChar *nChar = mSock->CurrcharObj();
	SERIAL serial = mSock->GetDWord( 1 );
	CItem *i = calcItemObjFromSer( serial );
	bool stackDeleted = false;
	
	CPBounce bounce( 5 );
	if( i == NULL ) 
	{
		nChar->Teleport();
		return;
	}
/*	if( i->GetCont() != NULL )
	{
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->subtractItemWeight( nChar, i );
		Bounce( mSock, i );
		RefreshItem( i );
		return;
	}
*/
	CTile tile;
	Map->SeekTile( i->GetID(), &tile);
	if( !nChar->AllMove() && ( i->GetMovable() == 2 || ( i->IsLockedDown() && i->GetOwnerObj() != nChar ) ||
		( tile.Weight() == 255 && i->GetMovable() != 1 ) ) )
	{
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->subtractItemWeight( nChar, i );
		Bounce( mSock, i );
		RefreshItem( i );
		return;
	}
	
	if( mSock->GetByte( 5 ) != 0xFF )	// Dropped in a specific location or on an item
	{
		CPRemoveItem toRemove = (*i);
		
		Network->PushConn();
		for( cSocket *tSock = Network->FirstSocket(); !Network->FinishedSockets(); tSock = Network->NextSocket() )
			tSock->Send( &toRemove );
		Network->PopConn();

		i->SetCont( NULL );
		i->SetLocation( mSock->GetWord( 5 ), mSock->GetWord( 7 ), mSock->GetByte( 9 ), nChar->WorldNumber() );
		RefreshItem( i );
	}
	else
	{
		CChar *t = calcCharObjFromSer( mSock->GetDWord( 10 ) );
		if( t != NULL )
			stackDeleted = dropItemOnChar( mSock, t, i );
		else
		{
			//Bounces items dropped in illegal locations in 3D UO client!!!
			if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
				Weight->subtractItemWeight( nChar, i );
			statwindow( mSock, nChar );
			Bounce( mSock, i );
			RefreshItem( i );
			return;
		}
	}

	if( !stackDeleted )
	{
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->subtractItemWeight( nChar, i );

		if( i->isDecayable() )
			i->SetDecayTime( BuildTimeValue( static_cast< R32 >(cwmWorldState->ServerData()->GetSystemTimerStatus( DECAY ) ) ) );

		if( nChar->GetMultiObj() != NULL )
		{
			CMultiObj *multi = findMulti( i );
			if( multi != NULL )
				i->SetMulti( multi );
		}
		Effects->itemSound( mSock, i, ( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND ) );
	}
	statwindow( mSock, nChar );
}

//o---------------------------------------------------------------------------o
//|   Function    :  void packItem( cSocket *s )
//|   Date        :  Unknown
//|   Programmer  :  Unknown
//o---------------------------------------------------------------------------o
//|   Purpose     :  Called when an item is dropped in a container or on another item
//o---------------------------------------------------------------------------o
void packItem( cSocket *mSock )
{
	CChar *mChar = mSock->CurrcharObj();
	CItem *nCont = calcItemObjFromSer( mSock->GetDWord( 10 ) );
	if( nCont == NULL || mChar == NULL )
		return;

	CItem *nItem = calcItemObjFromSer( mSock->GetDWord( 1 ) );
	if( nItem == NULL ) 
		return;

	bool stackDeleted = false;

	CPBounce bounce( 5 );

	if( nCont->GetLayer() == 0 && nCont->GetID() == 0x1E5E && nCont->GetCont() == mChar )
	{	// Trade window
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->subtractItemWeight( mChar, nItem );
		nItem->SetCont( mChar );
		CItem *z = calcItemObjFromSer( nCont->GetMoreB() );
		if( z != NULL )
		{
			if( z->GetMoreZ() || nCont->GetMoreZ() )
			{
				z->SetMoreZ( 0 );
				nCont->SetMoreZ( 0 );
				sendTradeStatus( z, nCont );
			}
			cSocket *zSock = calcSocketObjFromChar( (CChar *)z->GetCont() );
			if( zSock != NULL )
				Effects->itemSound( zSock, nCont, ( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND ) );
		}
		return;
	}

	if( cwmWorldState->ServerData()->GetWildernessBankStatus() ) // only if special bank is activated
	{
		if( nCont->GetMoreY() == 123 && nCont->GetMoreX() == 1 && nCont->GetType() == 1 )
		{
			if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
				Weight->subtractItemWeight( mChar, nItem );
			if( nItem->GetID() == 0x0EED )
			{
				nItem->SetCont( nCont );
				Effects->goldSound( mSock, 2 );
			}
			else // If not gold, bounce to the ground
			{
				sysmessage( mSock, 1200 );

				nItem->SetCont( NULL );
				nItem->SetLocation( mChar );
			}
			RefreshItem( nItem );
			Effects->itemSound( mSock, nItem, false );
			statwindow( mSock, mChar );
			return;
		}
	}
/*
	if( nItem->GetCont() != NULL )
	{
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->subtractItemWeight( mChar, nItem );
		Bounce( mSock, nItem );
		RefreshItem( nItem );
		return;
	}
*/
	CTile tile;
	Map->SeekTile( nItem->GetID(), &tile);
	if( !mChar->AllMove() && ( nItem->GetMovable() == 2 || ( nItem->IsLockedDown() && nItem->GetOwnerObj() != mChar ) ||
		( tile.Weight() == 255 && nItem->GetMovable() != 1 ) ) )
	{
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->subtractItemWeight( mChar, nItem );
		Bounce( mSock, nItem );
		RefreshItem( nItem );
		return;
	}

	if( nCont->GetType() == 87 )	// Trash container
	{
		Effects->PlaySound( mSock, 0x0042, false );
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->subtractItemWeight( mChar, nItem );
		statwindow( mSock, mChar );
		Items->DeleItem( nItem );
		sysmessage( mSock, 1201 );
		return;
	}
	else if( nCont->GetType() == 9 )	// Spell Book
	{
		if( nItem->GetID( 1 ) != 0x1F || nItem->GetID( 2 ) < 0x2D || nItem->GetID( 2 ) > 0x72 )
		{
			if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
				Weight->subtractItemWeight( mChar, nItem );
			Bounce( mSock, nItem );
			sysmessage( mSock, 1202 );
			RefreshItem( nItem );
			return;
		}
		CChar *c = getPackOwner( nCont );
		if( c != NULL && c != mChar && !mChar->CanSnoop() )
		{
			if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
				Weight->subtractItemWeight( mChar, nItem );
			Bounce( mSock, nItem );
			RefreshItem( nItem );
			sysmessage( mSock, 1203 );
			return;
		}
		char name[MAX_NAME];
		if( nItem->GetName()[0] == '#' )
			getTileName( nItem, name );
		else
			strcpy( name, nItem->GetName() );

		if( nCont->GetMore( 1 ) == 1 )	// using more1 to "lock" a spellbook for RP purposes
		{
			if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
				Weight->subtractItemWeight( mChar, nItem );
			sysmessage( mSock, 1204 );
			Bounce( mSock, nItem );
			RefreshItem( nItem );
			return;
		}

		if( !strcmp( name, Dictionary->GetEntry( 1605 ) ) )
		{
			if( nCont->GetMoreX() == 0xFFFFFFFF && nCont->GetMoreY() == 0xFFFFFFFF && nCont->GetMoreZ() == 0xFFFFFFFF )
			{
				if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
					Weight->subtractItemWeight( mChar, nItem );
				sysmessage( mSock, 1205 );
				Bounce( mSock, nItem );
				RefreshItem( nItem );
				return;
			}
			nCont->SetMoreX( 0xFFFFFFFF );
			nCont->SetMoreY( 0xFFFFFFFF );
			nCont->SetMoreZ( 0xFFFFFFFF );
		}
		else
		{
			int targSpellNum = nItem->GetID() - 0x1F2D;
			if( Magic->HasSpell( nCont, targSpellNum ) )
			{
				if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
					Weight->subtractItemWeight( mChar, nItem );
				sysmessage( mSock, 1206 );
				Bounce( mSock, nItem );
				RefreshItem( nItem );
				return;
			}
			else
				Magic->AddSpell( nCont, targSpellNum );
		}
		Effects->PlaySound( mSock, 0x0042, false );
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->subtractItemWeight( mChar, nItem );
		statwindow( mSock, mChar );
		Items->DeleItem( nItem );
		return;
	}
	else if( nCont->isPileable() && nItem->isPileable() && nCont->GetID() == nItem->GetID() && nCont->GetColour() == nItem->GetColour() )
	{	// Stacking
		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->subtractItemWeight( mChar, nItem );
		if( nCont->GetCont( 1 ) >= 0x40 && !Weight->checkPackWeight( (CItem *)nCont->GetCont(), nItem ) )
		{
			sysmessage( mSock, "That pack cannold hold any more weight" );
			Bounce( mSock, nItem );
			RefreshItem( nItem );
			return;
		}
		nItem->SetCont( nCont );
		stackDeleted = doStacking( mSock, mChar, nItem, nCont );
		if( !stackDeleted )
		{
			Bounce( mSock, nItem );
			RefreshItem( nItem );
		}
	}
	else if( nCont->GetType() == 1 )
	{
		CChar *j = getPackOwner( nCont );
		if( j != NULL )
		{
			if( j->IsNpc() && j->GetNPCAiType() == 17 && j->GetOwnerObj() == mChar )
			{
				mChar->SetSpeechMode( 3 );
				mChar->SetSpeechItem( nItem->GetSerial() );
				sysmessage( mSock, 1207 );
			}
			else if( j != mChar && mChar->GetCommandLevel() < CNS_CMDLEVEL )
			{
				if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
					Weight->subtractItemWeight( mChar, nItem );
				sysmessage( mSock, 1630 );
				Bounce( mSock, nItem );
				RefreshItem( nItem );
				return;
			}
		}
		if( mSock->GetByte( 5 ) != 0xFF )	// In a specific spot in a container
		{
			if( !Weight->checkPackWeight( nCont, nItem ) )
			{
				if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
					Weight->subtractItemWeight( mChar, nItem );
				sysmessage( mSock, "That pack cannold hold any more weight" );
				Bounce( mSock, nItem );
				RefreshItem( nItem );
				return;
			}
			nItem->SetX( mSock->GetWord( 5 ) );
			nItem->SetY( mSock->GetWord( 7 ) );

			if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
				Weight->subtractItemWeight( mChar, nItem );
			nItem->SetCont( nCont );
			nItem->SetZ( 9 );

			CPRemoveItem toRemove = (*nItem);
			Network->PushConn();
			for( cSocket *tSock = Network->FirstSocket(); !Network->FinishedSockets(); tSock = Network->NextSocket() )
				tSock->Send( &toRemove );
			Network->PopConn();

			RefreshItem( nItem );
			statwindow( mSock, mChar );
		}
		else
		{
			if( !Weight->checkPackWeight( nCont, nItem ) )
			{
				if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
					Weight->subtractItemWeight( mChar, nItem );
				sysmessage( mSock, "That pack cannold hold any more weight" );
				Bounce( mSock, nItem );
				RefreshItem( nItem );
				return;
			}
			nItem->SetCont( nCont );
			stackDeleted = autoStack( mSock, nItem, nCont );
			if( !stackDeleted )
			{
				if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
					Weight->subtractItemWeight( mChar, nItem );
			}
			statwindow( mSock, mChar );
		}
	}
	else
	{
		MapRegion->RemoveItem( nItem );

		nItem->SetX( mSock->GetWord( 5 ) );
		nItem->SetY( mSock->GetWord( 7 )  );
		nItem->SetZ( mSock->GetByte( 9 ) );

		if( nCont->GetType() == 63 || nCont->GetType() == 65 ) // - Unlocked item spawner or unlockable item spawner
			nItem->SetCont( nCont );
		else
		{
			nItem->SetCont( NULL );
			MapRegion->AddItem( nItem );
		}
		
		CPRemoveItem toRemove = (*nItem);
		Network->PushConn();
		for( cSocket *aSock = Network->FirstSocket(); !Network->FinishedSockets(); aSock = Network->NextSocket() )
			aSock->Send( &toRemove );
		Network->PopConn();

		if( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND )
			Weight->subtractItemWeight( mChar, nItem );
		RefreshItem( nCont );
		statwindow( mSock, mChar );
	}
	if( !stackDeleted )
		Effects->itemSound( mSock, nItem, ( mSock->PickupSpot() == PL_OTHERPACK || mSock->PickupSpot() == PL_GROUND ) );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void MountCreature( CChar *s, CChar *x )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Mount a ridable creature
//|									
//|	Modification	-	09/22/2002	-	Xuri - Unhide players when mounting horses etc.
//o---------------------------------------------------------------------------o
//|	Returns			- N/A
//o--------------------------------------------------------------------------o	
void MountCreature( CChar *s, CChar *x )
{
	cSocket *sockPtr = calcSocketObjFromChar( s );
	if( !objInRange( s, x, 2 ) ) 
		return;
	char temp[1024];
	if( x->GetOwnerObj() == s || s->IsGM() )
	{
		if( s->IsOnHorse() )
		{
			if(!cwmWorldState->ServerData()->GetCharHideWhileMounted())
				s->ExposeToView();
		}
		strcpy( temp, x->GetName() );
		s->SetOnHorse( true );
		CItem *c = Items->SpawnItem( NULL, s, 1, temp, false, 0x0915, x->GetSkin(), false, false );

		// Weazel 12 July, 2001 - Not all 3rd dawn creature mount id's are correct still missing a faction horse/dragon horse and
		// the ethereal llama and ostards.
		switch( x->GetID() )
		{
		case 0xC8:	c->SetID( 0x3E9F );	break;	// horse
		case 0xE2:	c->SetID( 0x3EA0 );	break;	// horse
		case 0xE4:	c->SetID( 0x3EA1 );	break;	// horse
		case 0xCC:	c->SetID( 0x3EA2 );	break;	// horse
		case 0xD2:	c->SetID( 0x3EA3 );	break;	// Desert Ostard
		case 0xDA:	c->SetID( 0x3EA4 );	break;	// Frenzied Ostard
		case 0xDB:	c->SetID( 0x3EA5 );	break;	// Forest Ostard
		case 0xDC:	c->SetID( 0x3EA6 );	break;	// llama
		case 0x75:	c->SetID( 0x3EA8 );	break;	// Silver Steed
		case 0x72:	c->SetID( 0x3EA9 );	break;	// Dark Steed
		case 0x73:	c->SetID( 0x3EAA );	break;	// Etheral Horse
		case 0xAA:	c->SetID( 0x3EAB );	break;	// Etheral Llama
		case 0xAB:	c->SetID( 0x3EAC );	break;	// Etheral Ostard
		case 0x84:	c->SetID( 0x3EAD );	break;	// Unicorn
		case 0x78:	c->SetID( 0x3EAF );	break;	// Faction Horse
		case 0x79:	c->SetID( 0x3EB0 );	break;	// Faction Horse
		case 0x77:	c->SetID( 0x3EB1 );	break;	// Faction Horse
		case 0x76:	c->SetID( 0x3EB2 );	break;	// Faction Horse
		case 0x8A:	c->SetID( 0x3EB4 );	break;	// Dragon Horse
		case 0x74:	c->SetID( 0x3EB5 );	break;	// Nightmare
		case 0xBB:	c->SetID( 0x3EB8 );	break;	// Ridgeback
		case 0x319:     c->SetID( 0x3EBB );     break;  // Skeletal Mount 
		case 0x317:     c->SetID( 0x3EBC );     break;  // Giant Beetle  
		case 0x31A:     c->SetID( 0x3EBD );     break;  // Swamp Dragon  
		case 0x31F:     c->SetID( 0x3EBE );     break;  // Armored Swamp Dragon  

		default:	c->SetID( 0x3E00 );	break;	// Bad
		}
		
		c->SetLayer( 0x19 );

		if( !c->SetCont( s ) )
		{
			s->SetOnHorse( false );	// let's get off our horse again
			return;
		}
		//s->WearItem( c );
		wornItems( sockPtr, s ); // send update to current socket
		Network->PushConn();
		for( cSocket *tSock = Network->FirstSocket(); !Network->FinishedSockets(); tSock = Network->NextSocket() ) // and to all inrange sockets (without re-sending to current socket )
		{
			if( sockPtr != tSock && charInRange( s, tSock->CurrcharObj() ) )
				wornItems( tSock, s );
		}
		Network->PopConn();

		if( x->GetTarg() != INVALIDSERIAL )	// zero out target, under all circumstances
		{
			x->SetTarg( INVALIDSERIAL );
			x->SetWar( false );
		}
		if( x->GetAttacker() != INVALIDSERIAL && x->GetAttacker() < cwmWorldState->GetCMem() )
			chars[x->GetAttacker()].SetTarg( INVALIDSERIAL );
		x->SetLocation( 7000, 7000, 0 );
		x->SetFrozen( true );
		c->SetMoreX( x->GetSerial() );
		if( x->GetSummonTimer() != 0 )
			c->SetDecayTime( x->GetSummonTimer() );

		x->Update();
	}
	else
		sysmessage( sockPtr, 1214 );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void DismountCreature( CChar *s )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Dismount a ridable creature
//o---------------------------------------------------------------------------o
void DismountCreature( CChar *s )
{
	if( s == NULL )
		return;
	CItem *ci = s->GetItemAtLayer( 0x19 );
	if( ci == NULL || ci->isFree() )	// if no horse item, or it's our default one
		return;

	s->SetOnHorse( false );
	CChar *tMount = calcCharObjFromSer( ci->GetMoreX() );
	if( tMount != NULL )
	{
		tMount->SetLocation( s );
		tMount->SetFrozen( false );
		if( ci->GetDecayTime() != 0 )
			tMount->SetSummonTimer( ci->GetDecayTime() );
		tMount->SetVisible( 0 );
		tMount->Update();
	}
	Items->DeleItem( ci );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void endmessage( int x )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Global message players with shutdown message
//o---------------------------------------------------------------------------o
void endmessage( int x )
{
	x = 0;
	UI32 igetclock = cwmWorldState->GetUICurrentTime();
	if( cwmWorldState->GetEndTime() < igetclock )
		cwmWorldState->SetEndTime( igetclock );
	char temp[1024];
	sprintf( temp, Dictionary->GetEntry( 1209 ), ((cwmWorldState->GetEndTime()-igetclock)/CLOCKS_PER_SEC) / 60 );
	sysbroadcast( temp );
}

#if defined(__unix__)
void illinst( int x = 0 ) //Thunderstorm linux fix
{
	sysbroadcast( "Fatal Server Error! Bailing out - Have a nice day!" );
	Console.Error( 0, "Illegal Instruction Signal caught - attempting shutdown" );
	endmessage( x );
}

void aus( int signal )
{
	Console.Error( 2, "Server crash averted! Floating point exception caught." );
} 

#endif

//o---------------------------------------------------------------------------o
//|	Function	-	void weblaunch( cSocket *s, char *txt )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Launch a webpage from within the client
//o---------------------------------------------------------------------------o
void weblaunch( cSocket *s, const char *txt )
{
	sysmessage( s, 1210 );
	CPWebLaunch toSend( txt );
	s->Send( &toSend );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void scriptcommand( cSocket *s, char *cmd, char *data )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Execute a command from scripts
//o---------------------------------------------------------------------------o
void scriptcommand( cSocket *s, const char *cmd2, const char *data2 )
{
	char tstring[1024];
	int total, ho, mi, se, tmp;
	CChar *mChar =s->CurrcharObj();
	char cmd[512], data[512];
	char idname[256];
	if ( !s || !cmd2 || !data2 )
		return;

	strcpy( cmd, cmd2 );
	strcpy( data, data2 );
	strupr( cmd );
	strupr( data );
	
	switch( cmd[0] )
	{
	case 'A':
		if( !strcmp( "ADDITEM", cmd ) )
			Items->menuAddItem( s, data );
		break;
	case 'B':
		if( !strcmp( "BATCH", cmd ) )
			cwmWorldState->SetExecuteBatch( makeNum( data ) );
		break;
	case 'C':
		if( !strcmp( "CPAGE", cmd ) )
			Commands->CPage( s, data );
		else if( !strcmp( "CREATETRACKINGMENU", cmd ) )
			Skills->CreateTrackingMenu( s, makeNum( data ) );
		break;
	case 'G':
		if( !strcmp( "GMMENU", cmd ) )
			gmmenu( s, makeNum( data ) );
		else if( !strcmp( "GMPAGE", cmd ) )
			Commands->GMPage( s, data );
		else if( !strcmp( "GCOLLECT", cmd ) )
			doGCollect();
		else if( !strcmp( "GOPLACE", cmd ) )
		{
			tmp = makeNum( data );
			Commands->MakePlace( s, tmp );
			if( s->AddX() != 0 )
			{
				mChar->SetLocation( (UI16)s->AddX(), (UI16)s->AddY(), s->AddZ() );
				mChar->Teleport();
			}
		}
		else if( !strcmp( "GUIINFORMATION", cmd ) )
		{
			GumpDisplay guiInfo( s, 400, 300 );
			guiInfo.SetTitle( "Server status" );

			total = (cwmWorldState->GetUICurrentTime() - cwmWorldState->GetStartTime() ) / CLOCKS_PER_SEC;
			ho = total / 3600;
			total -= ho * 3600;
			mi = total / 60;
			total -= mi * 60;
			se = total;
			total = 0;
			char hoStr[4];
			char miStr[4];
			char seStr[4];
			if( ho < 10 )
				sprintf( hoStr, "0%i", ho );
			else
				sprintf( hoStr, "%i", ho );
			if( mi < 10 )
				sprintf( miStr, "0%i", mi );
			else
				sprintf( miStr, "%i", mi );
			if( se < 10 )
				sprintf( seStr, "0%i", se );
			else
				sprintf( seStr, "%i", se );
			sprintf( tstring, "%s:%s:%s", hoStr, miStr, seStr );
			guiInfo.AddData( "Uptime", tstring );
			guiInfo.AddData( "Accounts", Accounts->size() );
			guiInfo.AddData( "Items", items.Count() );
			guiInfo.AddData( "Chars", chars.Count() );
			guiInfo.AddData( "Players in world", cwmWorldState->GetPlayersOnline() );
			sprintf( idname, "%s v%s(%s) [%s] Compiled by %s ", CVC.GetProductName(), CVC.GetVersion(), CVC.GetBuild(), OS_STR, CVC.GetName() );
			guiInfo.AddData( idname, idname, 7 );
			guiInfo.Send( 0, false, INVALIDSERIAL );
		}
		break;
	case 'I':
		if( !strcmp( "ITEMMENU", cmd ) )
			NewAddMenu( s, makeNum( data ) );
		else if( !strcmp( "INFORMATION", cmd ) )
		{
			total = (cwmWorldState->GetUICurrentTime() - cwmWorldState->GetStartTime() ) / CLOCKS_PER_SEC;
			ho = total / 3600;
			total -= ho * 3600;
			mi = total / 60;
			total -= mi * 60;
			se = total;
			total = 0;
			sysmessage( s, 1211, ho, mi, se, cwmWorldState->GetPlayersOnline(), Accounts->size(), items.Count(), chars.Count() );
		}
		break;
	case 'M':
		if( !strcmp( "MAKEMENU", cmd ) )
			Skills->NewMakeMenu( s, makeNum( data ), (UI08)mChar->GetMaking() );
		break;
	case 'N':
		if( !strcmp( "NPC", cmd ) )
		{
			s->XText( data );
			sprintf( tstring, Dictionary->GetEntry( 1212, s->Language() ), s->XText() );
			target( s, 0, 27, tstring );
		}
		break;
	case 'P':
		if( !strcmp( "POLY", cmd ) )
		{
			UI16 newBody = (UI16)makeNum( data );
			mChar->SetID( newBody );
			mChar->SetxID( newBody );
			mChar->SetOrgID( newBody );
			mChar->Teleport();	// why this is needed I'm not sure
		}
		break;
	case 'S':
		if( !strcmp( "SYSMESSAGE", cmd ) )
			sysmessage( s, data );
		else if( !strcmp( "SKIN", cmd ) )
		{
			COLOUR newSkin = (COLOUR)makeNum( data );
			mChar->SetSkin( newSkin );
			mChar->SetxSkin( newSkin );
			mChar->Teleport();
		}
		break;
	case 'T':
		if( !strcmp( "TRACKINGMENU", cmd ) )
			Skills->TrackingMenu( s, makeNum( data ) );
		break;
	case 'V':
		if( !strcmp( "VERSION", cmd ) )
		{
			sprintf( idname, "%s v%s(%s) [%s] Compiled by %s ", CVC.GetProductName(), CVC.GetVersion(), CVC.GetBuild(), OS_STR, CVC.GetName() );
			sysmessage( s, idname );
		}
		break;
	case 'W':
		if( !strcmp( "WEBLINK", cmd ) )
			weblaunch( s, data );
		break;
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	void batchcheck( cSocket *s )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Check if we run a batch file
//o---------------------------------------------------------------------------o
void batchcheck( cSocket *s )
{
	char temp[1024];
	sprintf( temp, "BATCH %i", cwmWorldState->GetExecuteBatch() );
	ScriptSection *Batch = FileLookup->FindEntry( temp, menus_def );
	if( Batch == NULL )
		return;
	const char *data = NULL;
	const char *tag = NULL;
	for( tag = Batch->First(); !Batch->AtEnd(); tag = Batch->Next() )
	{
		data = Batch->GrabData();
		scriptcommand( s, tag, data );
	}
	cwmWorldState->SetExecuteBatch( 0 );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void callguards( CChar *mChar )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Used when a character calls "Guards" Will look for a criminal
//|					first checking for anyone attacking him. If no one is attacking
//|					him it will look for any people nearby who are criminal or
//|					murderers
//o---------------------------------------------------------------------------o
void callGuards( CChar *mChar )
{
	if( mChar == NULL )
		return;

	if( !region[mChar->GetRegion()]->IsGuarded() || !cwmWorldState->ServerData()->GetGuardsStatus() )
		return;

	int xOffset = MapRegion->GetGridX( mChar->GetX() );
	int yOffset = MapRegion->GetGridY( mChar->GetY() );

	SubRegion *toCheck = MapRegion->GetGrid( xOffset, yOffset, mChar->WorldNumber() );
	if( toCheck == NULL )
		return;

	CHARACTER attacker = mChar->GetAttacker();
	if( attacker != INVALIDSERIAL )
	{
		CChar *aChar = &chars[attacker];
		if( aChar != NULL )
		{
			if( !aChar->IsDead() && ( aChar->IsCriminal() || aChar->IsMurderer() ) )
			{
				if( charInRange( mChar, aChar ) )
				{
					callGuards( mChar, aChar );
					return;
				}
			}
		}
	}

	for( CChar *tempChar = toCheck->FirstChar(); !toCheck->FinishedChars(); tempChar = toCheck->GetNextChar() )
	{
		if( tempChar == NULL )
			break;

		if( !tempChar->IsDead() && ( tempChar->IsCriminal() || tempChar->IsMurderer() ) )
		{
			if( charInRange( tempChar, mChar ) )
			{
				callGuards( mChar, tempChar );
				return;
			}
		}
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	void callguards( CChar *mChar, CChar *targChar )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Used when a character calls guards on another character, will
//|					ensure that character is not dead and is either a criminal or
//|					murderer, and that he is in visual range of the victim, will
//|					then spawn a guard to take care of the criminal.
//o---------------------------------------------------------------------------o
void callGuards( CChar *mChar, CChar *targChar )
{
	if( mChar == NULL || targChar == NULL ) 
		return;

	if( !region[mChar->GetRegion()]->IsGuarded() || !cwmWorldState->ServerData()->GetGuardsStatus() )
		return;

	if( !targChar->IsDead() && ( targChar->IsCriminal() || targChar->IsMurderer() ) )
	{
		if( charInRange( mChar, targChar ) )
			Combat->SpawnGuard( mChar, targChar, targChar->GetX(), targChar->GetY(), targChar->GetZ() );
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	void DisplaySettings( void )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	UOX startup stuff
//| Moved that here because we need it in processkey now
//|									
//|	Modification	-	10/21/2002	-	EviLDeD - Xuri found the bug in one spot, just
//|									happened upon this quick fix. for BackUp operation.
//o---------------------------------------------------------------------------o
void DisplaySettings( void )
{
	std::map< bool, std::string > activeMap;
	activeMap[true] = "Activated!";
	activeMap[false] = "Disabled!";

	// Server.scp status --- By Magius(CHE)
	Console << "Server Settings:" << myendl;
	
	Console << "   -Archiving[";
	if(cwmWorldState->ServerData()->GetServerBackupStatus() )
		Console << "Enabled]. (" << cwmWorldState->ServerData()->GetBackupDirectory() << ")" << myendl;
	else 
		Console << "Disabled]" << myendl;
	
	Console << "   -Weapons & Armour Rank System: ";
	Console << activeMap[cwmWorldState->ServerData()->GetRankSystemStatus()].c_str() << myendl;
	
	Console << "   -Vendors buy by item name: ";
	Console << activeMap[cwmWorldState->ServerData()->GetSellByNameStatus()].c_str() << myendl;
	
	Console << "   -Adv. Trade System: ";
	Console << activeMap[cwmWorldState->ServerData()->GetTradeSystemStatus()].c_str() << myendl;
	
	Console << "   -Special Bank stuff: ";
	Console << activeMap[cwmWorldState->ServerData()->GetWildernessBankStatus()].c_str() << myendl;
	
	Console << "   -Crash Protection: ";
	if( cwmWorldState->ServerData()->GetServerCrashProtectionStatus() < 1 ) 
		Console << "Disabled!" << myendl;
#ifndef _CRASH_PROTECT_
	else 
		Console << "Unavailable in this version" << myendl;
#else
	else if( cwmWorldState->ServerData()->GetServerCrashProtectionStatus() == 1 ) 
		Console << "Save on crash" << myendl;
	else 
		Console << "Save & Restart Server" << myendl;
#endif

	Console << "   -xGM Remote: ";
	Console << activeMap[cwmWorldState->GetXGMEnabled()].c_str() << myendl;

	Console << "   -Races: " << Races->Count() << myendl;
	Console << "   -Guilds: " << GuildSys->NumGuilds() << myendl;
	Console << "   -Char count: " << cwmWorldState->GetCMem() << myendl;
	Console << "   -Item count: " << cwmWorldState->GetIMem() << myendl;
	Console << "   -Num Accounts: " << Accounts->size() << myendl;
	Console << "   Directories: " << myendl;
	Console << "   -Shared:          " << cwmWorldState->ServerData()->GetSharedDirectory() << myendl;
	Console << "   -Archive:         " << cwmWorldState->ServerData()->GetBackupDirectory() << myendl;
	Console << "   -Data:            " << cwmWorldState->ServerData()->GetDataDirectory() << myendl;
	Console << "   -Defs:            " << cwmWorldState->ServerData()->GetDefsDirectory() << myendl;
	Console << "   -Scripts:         " << cwmWorldState->ServerData()->GetScriptsDirectory() << myendl;
	Console << "   -HTML:            " << cwmWorldState->ServerData()->GetHTMLDirectory() << myendl;
	Console << "   -Books:           " << cwmWorldState->ServerData()->GetBooksDirectory() << myendl;
	Console << "   -MessageBoards:   " << cwmWorldState->ServerData()->GetMsgBoardDirectory() << myendl;
}

//o---------------------------------------------------------------------------o
//|	Function	-	void processkey( int c )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Handle keypresses in console
//o---------------------------------------------------------------------------o
void processkey( int c )
{
	char outputline[128], temp[1024];
	bool kill = false;
	int indexcount = 0;
	int i, j;
	int keyresp;

	if( c == '*' )
	{
		if( cwmWorldState->GetSecure() )
			messageLoop << "Secure mode disabled. Press ? for a commands list";
		else
			messageLoop << "Secure mode re-enabled";
		cwmWorldState->SetSecure( !cwmWorldState->GetSecure() );
		return;
	} 
	else 
	{
		if( cwmWorldState->GetSecure() )
		{
			messageLoop << "Secure mode prevents keyboard commands! Press '*' to disable";
			return;
		}
		
		switch( c )
		{
			case '!':
				// Force server to save accounts file
				messageLoop << "CMD: Saving Accounts... ";
				Accounts->Save();
				messageLoop << MSG_PRINTDONE;
				break;
			case '@':
				// Force server to save all files.(Manual save)
				messageLoop << MSG_WORLDSAVE;
				break;
		case 'Y':
#pragma note("Console Broadcast needs to not require cout anymore.  Is there a better way?")
			//messageLoop << "Console> ";
			std::cout << "System: ";
			while( !kill )
			{
				keyresp = cl_getch();
				switch( keyresp )
				{
				case -1:	// no key pressed
				case 0:
					break;
				case 0x1B:
					memset( outputline, 0x00, sizeof( outputline ) );
					indexcount = 0;
					kill = true;
					std::cout << std::endl;
					messageLoop << "| CMD: System broadcast canceled.";
					break;
				case 0x08:
					indexcount--;
					if( indexcount < 0 )	
						indexcount = 0;
					else
						std::cout << "\b \b";
					break;
				case 0x0A:
				case 0x0D:
					outputline[indexcount] = 0;
					messageLoop.NewMessage( MSG_CONSOLEBCAST, outputline );
					indexcount = 0;
					kill = true;
					std::cout << std::endl;
					sprintf( temp, "| CMD: System broadcast sent message \"%s\"", outputline );
					memset( outputline, 0x00, sizeof( outputline ) );
					messageLoop << temp;
					break;
				default:
					if( indexcount < sizeof( outputline ) )
					{
						outputline[indexcount++] = (UI08)(keyresp);
						std::cout << (char)keyresp;
					}
					break;
				}
				keyresp = 0x00;
			}
			break;
			case 0x1B:
			case 'Q':
				messageLoop << MSG_SECTIONBEGIN;
				messageLoop << "CMD: Immediate Shutdown initialized!";
				messageLoop << MSG_SHUTDOWN;
				break;
			case '1':
				// Reload server ini file
				messageLoop << "CMD: Loading Server INI... ";
				cwmWorldState->ServerData()->load();
				messageLoop << MSG_PRINTDONE;
				break;
			case '2':
				// Reload accounts, and update Access.adm if new accounts available.
				messageLoop << "CMD: Loading Accounts... ";
				Accounts->Load();
				messageLoop << MSG_PRINTDONE;
				break;
			case '3':
				// Reload Region Files
				messageLoop << "CMD: Loading Regions... ";
				FileIO->LoadRegions();
				messageLoop << MSG_PRINTDONE;
				break;
			case '4':
				// Reload the serve spawn regions
				messageLoop << "CMD: Loading Spawn Regions... ";
				FileIO->LoadSpawnRegions();
				messageLoop << MSG_PRINTDONE;
				break;
			case '5':
				// Reload the current Spells 
				messageLoop << "CMD: Loading spells... ";
				Magic->LoadScript();
				messageLoop << MSG_PRINTDONE;
				break;
			case '6':
				// Reload the server command list
				messageLoop << "CMD: Loading commands... ";
				Commands->Load();
				messageLoop << MSG_PRINTDONE;
				break;
			case '7':
				// Reload the server defantion files
				messageLoop << "CMD: Loading Server DFN... ";
				FileLookup->Reload();
				messageLoop << MSG_PRINTDONE;
				break;
			case '8':
				// messageLoop access is REQUIRED, as this function is executing in a different thread, so we need thread safety
				messageLoop << "CMD: Loading JSE Scripts... ";
				messageLoop << MSG_RELOADJS;
				break;
			case '9':
				// Reload the HTML output templates
				messageLoop << "CMD: Loading HTML Templates... ";
				HTMLTemplates->Load();
				messageLoop << MSG_PRINTDONE;
				break;
			case '0':
				// Reload all the files. If there are issues with these files change the order reloaded from here first.
				cwmWorldState->ServerData()->load();
				messageLoop << "CMD: Loading All";
				messageLoop << "     Server INI... ";
				// Reload accounts, and update Access.adm if new accounts available.
				messageLoop << "     Loading Accounts... ";
				Accounts->Load();
				messageLoop << MSG_PRINTDONE;
				// Reload Region Files
				messageLoop << "     Loading Regions... ";
				FileIO->LoadRegions();
				messageLoop << MSG_PRINTDONE;
				// Reload the serve spawn regions
				messageLoop << "     Loading Spawn Regions... ";
				FileIO->LoadSpawnRegions();
				messageLoop << MSG_PRINTDONE;
				// Reload the current Spells 
				messageLoop << "     Loading spells... ";
				Magic->LoadScript();
				messageLoop << MSG_PRINTDONE;
				// Reload the server command list
				messageLoop << "     Loading commands... ";
				Commands->Load();
				messageLoop << MSG_PRINTDONE;
				// Reload DFN's
				messageLoop << "     Loading Server DFN... ";
				FileLookup->Reload();
				messageLoop << MSG_PRINTDONE;
				// messageLoop access is REQUIRED, as this function is executing in a different thread, so we need thread safety
				messageLoop << "     Loading JSE Scripts... ";
				messageLoop << MSG_RELOADJS;
				// Reload the HTML output templates
				messageLoop << "     Loading HTML Templates... ";
				HTMLTemplates->Load();
				messageLoop << MSG_PRINTDONE;
				break;
		case 'T':
			// Timed shut down(10 minutes)
			messageLoop << "CMD: 10 Minute Server Shutdown Announced(Timed)";
			cwmWorldState->SetEndTime( BuildTimeValue( 600 ) );
			endmessage(0);
			break;
		case 'L':
			// Show Layer info
			if( cwmWorldState->GetDisplayLayers() )
				messageLoop << "CMD: Show Layer Disabled";
			else
				messageLoop << "CMD: Show Layer Enabled";
			cwmWorldState->SetDisplayLayers( !cwmWorldState->GetDisplayLayers() );
			break;
		case  'D':    
			// Disconnect account 0 (useful when client crashes)
			cSocket *tSock;
			for( tSock = Network->LastSocket(); tSock != NULL; tSock = Network->PrevSocket() )
			{
				if( tSock->AcctNo() == 0 )
					Network->Disconnect( calcSocketFromSockObj( tSock ) );
			}
			messageLoop << "CMD: Socket Disconnected(Account 0).";
			break;
		case 'K':		
			// mass disconnect
			for( i = cwmWorldState->GetPlayersOnline() - 1; i >= 0; i-- )
				Network->Disconnect( i );
			messageLoop << "CMD: All Connections Closed.";
			break;
		case 'H':                
			// Enable/Disable heartbeat
			if( !cwmWorldState->ServerData()->GetSystemHeartBeatStatus() ) 
				messageLoop << "CMD: Heartbeat Disabled";
			else 
				messageLoop << "CMD: Heartbeat Enabled";
			cwmWorldState->ServerData()->SetSystemHeartBeatStatus( !cwmWorldState->ServerData()->GetSystemHeartBeatStatus() );
			break;
		case 'P':                
			// 1/13/2003 - Dreoth - Log Performance Information enhancements
			Console.LogEcho( true );
			Console.Log( "--- Starting Performance Dump ---", "performance.log");
			Console.Log( "Performace Dump:", "performance.log");
			Console.Log( "Network code: %.2fmsec [%i samples]", "performance.log", (R32)((R32)networkTime/(R32)networkTimeCount), networkTimeCount);
			Console.Log( "Timer code: %.2fmsec [%i samples]", "performance.log", (R32)((R32)timerTime/(R32)timerTimeCount), timerTimeCount);
			Console.Log( "Auto code: %.2fmsec [%i samples]", "performance.log", (R32)((R32)autoTime/(R32)autoTimeCount), autoTimeCount);
			Console.Log( "Loop Time: %.2fmsec [%i samples]", "performance.log", (R32)((R32)loopTime/(R32)loopTimeCount), loopTimeCount);
			Console.Log( "Characters: %i/%i - Items: %i/%i (Dynamic)", "performance.log", chars.Count(), cwmWorldState->GetCMem(), items.Count(), cwmWorldState->GetIMem());
			Console.Log( "Simulation Cycles: %f per sec", "performance.log", (1000.0*(1.0/(R32)((R32)loopTime/(R32)loopTimeCount))));
			Console.Log( "Bytes sent: %i", "performance.log", globalSent);
			Console.Log( "Bytes Received: %i", "performance.log", globalRecv);
			Console.Log( "--- Performance Dump Complete ---", "performance.log");
			Console.LogEcho( false );
			break;
		case 'W':                
			// Display logged in chars
			messageLoop << "CMD: Current Users in the World:";
			j = 0;
			cSocket *iSock;
			Network->PushConn();
			for( iSock = Network->FirstSocket(); !Network->FinishedSockets(); iSock = Network->NextSocket() )
			{
				j++;
				CChar *mChar = iSock->CurrcharObj();
				sprintf( temp, "     %i) %s [%i %i %i %i]", j - 1, mChar->GetName(), mChar->GetSerial( 1 ), mChar->GetSerial( 2 ), mChar->GetSerial( 3 ), mChar->GetSerial( 4 ) );
				messageLoop << temp;
			}
			Network->PopConn();
			sprintf( temp, "     Total users online: %i", j );
			messageLoop << temp;
			break;
		case 'M':
			UI32 tmp, total;
			total = 0;
			tmp = 0;
			messageLoop << "CMD: UOX Memory Information:";
			messageLoop << "     Cache:";
			sprintf( temp, "        Tiles: %i bytes", Map->TileMem );
			messageLoop << temp;
			sprintf( temp, "        Statics: %i bytes", Map->StaMem );
			messageLoop << temp;
			sprintf( temp, "        Version: %i bytes", Map->versionMemory );
			messageLoop << temp;
			sprintf( temp, "        Map0: %i bytes [%i Hits - %i Misses]", 9*MAP0CACHE, Map->Map0CacheHit, Map->Map0CacheMiss );
			messageLoop << temp;
			total += tmp = chars.Size() + cwmWorldState->GetCMem()*sizeof( teffect_st ) + cwmWorldState->GetCMem()*sizeof(char) + cwmWorldState->GetCMem()*sizeof(int)*5;
			sprintf( temp, "     Characters: %i bytes [%i chars ( %i allocated )]", tmp, chars.Count(), cwmWorldState->GetCMem() );
			messageLoop << temp;
			total += tmp = items.Size() + cwmWorldState->GetIMem()*sizeof(int)*4;
			sprintf( temp, "     Items: %i bytes [%i items ( %i allocated )]", tmp, items.Count(), cwmWorldState->GetIMem() );
			messageLoop << temp;
			sprintf( temp, "        You save I: %i & C: %i bytes!", cwmWorldState->GetIMem() * sizeof(CItem) - items.Size(), cwmWorldState->GetCMem() * sizeof( CChar ) - chars.Size() );
			total += tmp = 69 * sizeof( SpellInfo );
			sprintf( temp, "     Spells: %i bytes", tmp );
			messageLoop << "     Sizes:";
			sprintf( temp, "        CItem  : %i bytes", sizeof( CItem ) );
			messageLoop << temp;
			sprintf( temp, "        CChar  : %i bytes", sizeof( CChar ) );
			messageLoop << temp;
			sprintf( temp, "        TEffect: %i bytes (%i total)", sizeof( teffect_st ), sizeof( teffect_st ) * TEffects->Count() );
			messageLoop << temp;
			total += tmp = Map->TileMem + Map->StaMem + Map->versionMemory;
			sprintf( temp, "        Approximate Total: %i bytes", total );
			messageLoop << temp;
			break;
		case 'e':
		case 'E':
			// Toggle Layer errors
			j = 0;
			for( i = 0; i < MAXLAYERS; i++ )
			{
				if( cwmWorldState->GetErroredLayer( i ) != 0 )
				{
					j ++;
					if( i < 10 )
						sprintf( temp, "| ERROR: Layer 0%i -> %i errors", i, cwmWorldState->GetErroredLayer( i ) );
					else
						sprintf( temp, "| ERROR: Layer %i -> %i errors", i, cwmWorldState->GetErroredLayer( i ) );
					messageLoop << temp;
				}
			}
			sprintf( temp, "| ERROR: Found errors on %i layers.", j );
			messageLoop << temp;
			break;
		case '?':
			messageLoop << MSG_SECTIONBEGIN;
			messageLoop << "Console commands:";
			messageLoop << MSG_SECTIONBEGIN;
			messageLoop << " ShardOP:";
			messageLoop << "    * - Lock/Unlock Console ? - Commands list(this)";
			messageLoop << "    C - Configuration       H - Heart Beat";
			messageLoop << "    Y - Console Broadcast   Q - Quit/Exit           ";
			messageLoop << " Load Commands:";
			messageLoop << "    1 - Ini                 2 - Accounts";
			messageLoop << "    3 - Regions             4 - Spawn Regions";
			messageLoop << "    5 - Spells              6 - Commands";
			messageLoop << "    7 - Dfn's               8 - JavaScript";
			messageLoop << "    9 - HTML Templates      0 - ALL(1-9)";
			messageLoop << " Save Commands:";
			messageLoop << "    ! - Accounts            @ - World";
			messageLoop << "    # - Unused              $ - Unused";
			messageLoop << "    % - Unused              ^ - Unused";
			messageLoop << "    & - Unused              ( - Unused";
			messageLoop << "    ) - Unused";
			messageLoop << " Server Maintenence:";
			messageLoop << "    P - Performance         W - Characters Online";
			messageLoop << "    M - Memory Information  T - 10 Minute Shutdown";
			messageLoop << "    V - Dump Lookups(Devs)  N - Dump Npc.Dat";
			messageLoop << "    L - Layer Information"; 
			messageLoop << " Network Maintenence:";
			messageLoop << "    D - Disconnect Acct0    K - Disconnect All";
			messageLoop << "    Z - Socket Logging      ";
			messageLoop << MSG_SECTIONBEGIN;
			break;
		case 'v':
		case 'V':
			// Dump look up data to files so developers working with extending the ini will have a table to use
			messageLoop << "| CMD: Creating Server.scp and Uox3.ini Tag Lookup files(For Developers)....";
			cwmWorldState->ServerData()->DumpLookup( 0 );
			cwmWorldState->ServerData()->DumpLookup( 1 );
			cwmWorldState->ServerData()->save( "./uox.tst.ini" );
			messageLoop << MSG_PRINTDONE;
			break;
		case 'z':
		case 'Z':
		{
			// Log socket activity
			Network->PushConn();
			bool loggingEnabled = false;
			cSocket *snSock = Network->FirstSocket();
			if( snSock != NULL )
				loggingEnabled = !snSock->Logging();
			for( ; !Network->FinishedSockets(); snSock = Network->NextSocket() )
			{
				if( snSock != NULL )
					snSock->Logging( !snSock->Logging() );
			}
			Network->PopConn();
			if( loggingEnabled )
				messageLoop << "CMD: Network Logging Enabled.";
			else
				messageLoop << "CMD: Network Logging Disabled.";
			break;
		}
		case 'N':
			//. Dump a file that contains the id and sound some toher misc data about monster NPC.
			DumpCreatures();
			break;
		case 'c':
		case 'C':
			// Shows a configuration header
			DisplaySettings();
			break;
		default:
			sprintf( temp, "Key \'%c\' [%i] does not perform a function", (char)c, c );
			messageLoop << temp;
			break;
		}
	}
}

//o----------------------------------------------------------------------------o
//|   Function -	 void checkkey( void )
//|   Date     -	 Unknown
//|   Programmer  -  Unknown  (Touched up by EviLDeD)
//o----------------------------------------------------------------------------o
//|   Purpose     -  Facilitate console control. SysOp keys, and localhost 
//|					 controls.
//o----------------------------------------------------------------------------o
void checkkey( void )
{
	int c = cl_getch();
	if( c > 0 )
	{
		if( (cluox_io) && ( c == 250 ) )
		{  // knox force unsecure mode, need this since cluox can't know
			//      how the toggle status is.
			if( cwmWorldState->GetSecure() )
			{
				Console << "Secure mode disabled. Press ? for a commands list." << myendl;
				cwmWorldState->SetSecure( false );
				return;
			}
		}
		c = toupper(c);
		processkey( c );
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	bool genericCheck( CChar *i )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Check characters status.  Returns true if character was killed
//o---------------------------------------------------------------------------o
bool genericCheck( CChar *i )
{
	if( i->IsDead() )
		return false;

	UI16 c;
	bool strUpdate = false;
	bool intUpdate = false;
	bool dexUpdate = false;

	bool isOn = isOnline( i ), isNPC = i->IsNpc();
	
	if( i->GetHP() > i->GetMaxHP() )
	{
		i->SetHP( i->GetMaxHP() );
		strUpdate = true;
	}
	if( i->GetStamina() > i->GetMaxStam() )
	{
		i->SetStamina( i->GetMaxStam() );
		dexUpdate = true;
	}
	if( i->GetMana() > i->GetMaxMana() )
	{
		i->SetMana( i->GetMaxMana() );
		intUpdate = true;
	}

	if( i->GetRegen( 0 ) <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
	{
		if( i->GetHP() < i->GetMaxHP() && ( i->GetHunger() > (SI16)cwmWorldState->ServerData()->GetHungerThreshold() || cwmWorldState->ServerData()->GetHungerRate() == 0 ) )
		{
			for( c = 0; c < i->GetMaxHP() + 1; c++ )
			{
				if( i->GetHP() <= i->GetMaxHP() && ( i->GetRegen( 0 ) + ( c * cwmWorldState->ServerData()->GetSystemTimerStatus( HITPOINT_REGEN ) * CLOCKS_PER_SEC) ) <= cwmWorldState->GetUICurrentTime() )
				{
					if( i->GetSkill( HEALING ) < 500 ) 
						i->IncHP( 1 );
					else if( i->GetSkill( HEALING ) < 800 ) 
						i->IncHP( 2 );
					else 
						i->IncHP( 3 );
					if( i->GetHP() > i->GetMaxHP() ) 
					{
						i->SetHP( i->GetMaxHP() ); 
						break;
					}
					strUpdate = true;
				}
				else			// either we're all healed up, or all time periods have passed
					break;
			}
		}
		i->SetRegen( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetSystemTimerStatus( HITPOINT_REGEN ) ), 0 );
	}
	if( i->GetRegen( 1 ) <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
	{
		if( i->GetStamina() < i->GetMaxStam() )
		{
			for( c = 0; c < i->GetMaxStam() + 1; c++ )
			{
				if( ( i->GetRegen( 1 ) + ( c * cwmWorldState->ServerData()->GetSystemTimerStatus( STAMINA_REGEN ) * CLOCKS_PER_SEC) ) <= cwmWorldState->GetUICurrentTime() && i->GetStamina() <= i->GetMaxStam() )
				{
					i->SetStamina( (SI16)( i->GetStamina() + 1 ) );
					if( i->GetStamina() > i->GetMaxStam() ) 
					{
						i->SetStamina( i->GetMaxStam() );
						break;
					}
					dexUpdate = true;
				}
				else
					break;
			}
		}
		i->SetRegen( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetSystemTimerStatus( STAMINA_REGEN ) ), 1 );
	}

	// CUSTOM START - SPUD:MANA REGENERATION:Rewrite of passive and active meditation code
	if( i->GetRegen( 2 ) <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
	{
		if( i->GetMana() < i->GetMaxMana() )
		{
			for( c = 0; c < i->GetMaxMana() + 1; c++ )
			{
				if( i->GetRegen( 2 ) + ( c * cwmWorldState->ServerData()->GetSystemTimerStatus( MANA_REGEN ) * CLOCKS_PER_SEC) <= cwmWorldState->GetUICurrentTime() && i->GetMana() <= i->GetMaxMana() )
				{
					Skills->CheckSkill( i, MEDITATION, 0, 1000 );	// Check Meditation for skill gain ala OSI
					i->SetMana( (SI16)( i->GetMana() + 1 ) );	// Gain a mana point
					if( i->GetMana() > i->GetMaxMana() ) 
					{
						if( i->GetMed() ) // Morrolan = Meditation
						{
							sysmessage( calcSocketObjFromChar( i ), 969 );
							i->SetMed( 0 );
						}
						i->SetMana( i->GetMaxMana() ); 
						break;
					}
					intUpdate = true;
				}
			}
		}
		R32 MeditationBonus = ( .00075f * i->GetSkill( MEDITATION ) );	// Bonus for Meditation
		int NextManaRegen = static_cast<int>(cwmWorldState->ServerData()->GetSystemTimerStatus( MANA_REGEN ) * ( 1 - MeditationBonus ) * CLOCKS_PER_SEC);
		if( cwmWorldState->ServerData()->GetServerArmorAffectManaRegen() )	// If armor effects mana regeneration...
		{
			R32 ArmorPenalty = Combat->calcDef( i, 0, false );	// Penalty taken due to high def
			if( ArmorPenalty > 100 )	// For def higher then 100, penalty is the same...just in case
				ArmorPenalty = 100;
			ArmorPenalty = 1 + (ArmorPenalty / 25);
			NextManaRegen = static_cast<int>(NextManaRegen * ArmorPenalty);
		}
		if( i->GetMed() )	// If player is meditation...
			i->SetRegen( ( cwmWorldState->GetUICurrentTime() + ( NextManaRegen / 2 ) ), 2 );
		else
			i->SetRegen( ( cwmWorldState->GetUICurrentTime() + NextManaRegen ), 2 );
	}
	// CUSTOM END
	if( i->GetHidden() == 2 && ( i->GetInvisTimeout() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() ) && !i->IsPermHidden() )
		i->ExposeToView();
	cSocket *s = NULL;
	if( !isNPC )
		s = calcSocketObjFromChar( i );
	if( isOn || isNPC )
	{
		if( cwmWorldState->ServerData()->GetHungerRate() > 1 && i->GetHungerStatus() && ( i->GetHungerTime() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() ) )
		{
			if( i->GetHunger() > 0 && !i->IsCounselor() && !i->IsGM() )
				i->DecHunger(); //Morrolan GMs and Counselors don't get hungry
			
			UI16 HungerTrig = i->GetScriptTrigger();
			cScript *toExecute = Trigger->GetScript( HungerTrig );
			bool doHunger = true;
			if( toExecute != NULL )
				doHunger = !toExecute->OnHungerChange( i, i->GetHunger() );
			if( doHunger )
			{
				switch( i->GetHunger() )
				{
				case 6: break;
				case 5:	sysmessage( s, 1222 );	break;
				case 4:	sysmessage( s, 1223 );	break;
				case 3:	sysmessage( s, 1224 );	break;
				case 2:	sysmessage( s, 1225 );	break;
				case 1:	sysmessage( s, 1226 );	break;
				case 0:
					if( !i->IsGM() && !i->IsCounselor() ) 
						sysmessage( s, 1227 );
					break;	
				}
			}
			i->SetHungerTime( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetHungerRate() ) );
		}
		if( ( cwmWorldState->GetHungerDamageTimer() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() ) && cwmWorldState->ServerData()->GetHungerDamage() > 0 ) // Damage them if they are very hungry
		{
			cwmWorldState->SetHungerDamageTimer( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetHungerDamageRateTimer() ) ); // set new hungertime
			if( i->GetHungerStatus() && i->GetHP() > 0 && i->GetHunger() < 2 && !i->IsCounselor() && !i->IsDead() )
			{     
				sysmessage( s, 1228 );
				i->IncHP( (SI16)( -cwmWorldState->ServerData()->GetHungerDamage() ) );
				updateStats( i, 0 );
				if( i->GetHP() <= 0 )
				{ 
					sysmessage( s, 1229 );
					doDeathStuff( i );
				}
			}
		}
	}
	if( !i->IsInvulnerable() && i->GetPoisoned() && ( isNPC || isOn ) )
	{
		if( i->GetPoisonTime() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
		{
			if( i->GetPoisonWearOffTime() > cwmWorldState->GetUICurrentTime() )
			{
				SI16 pcalc = 0;
				switch( i->GetPoisoned() )
				{
				case 1:
					i->SetPoisonTime( BuildTimeValue( 5 ) );
					if( i->GetPoisonTextTime() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
					{
						i->SetPoisonTextTime( BuildTimeValue( 10 ) );
						npcEmoteAll( i, 1240, true, i->GetName() );
					}
					i->IncHP( (SI16)( -RandomNum( 1, 2 ) ) );
					strUpdate = true;
					break;
				case 2:
					i->SetPoisonTime( BuildTimeValue( 4 ) );
					if( i->GetPoisonTextTime() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
					{
						i->SetPoisonTextTime( BuildTimeValue( 10 ) );
						npcEmoteAll( i, 1241, true, i->GetName() );
					}
					pcalc = (SI16)( ( i->GetHP() * RandomNum( 2, 5 ) / 100 ) + RandomNum( 0, 2 ) ); // damage: 1..2..5% of hp's+ 1..2 constant
					i->IncHP( (SI16)( -pcalc ) );
					strUpdate = true;
					break;
				case 3:
					i->SetPoisonTime( BuildTimeValue( 3 ) );
					if( i->GetPoisonTextTime() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
					{
						i->SetPoisonTextTime( BuildTimeValue( 10 ) );
						npcEmoteAll( i, 1242, true, i->GetName() );
					}
					pcalc = (SI16)( ( i->GetHP() * RandomNum( 5, 10 ) / 100 ) + RandomNum( 1, 3 ) ); // damage: 5..10% of hp's+ 1..2 constant
					i->IncHP( (SI16)( -pcalc ) );
					strUpdate = true;
					break;
				case 4:
					i->SetPoisonTime( BuildTimeValue( 3 ) );
					if( i->GetPoisonTextTime() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() ) 
					{
						i->SetPoisonTextTime( BuildTimeValue( 10 ) );
						npcEmoteAll( i, 1243, true, i->GetName() );
					}
					pcalc = (SI16)( i->GetHP() / 5 + RandomNum( 3, 6 ) ); // damage: 20% of hp's+ 3..6 constant, quite deadly <g>
					i->IncHP( (SI16)( -pcalc ) );
					strUpdate = true;
					break;
				default:
					Console.Error( 2, " Fallout of switch statement without default. uox3.cpp, genericCheck()" );
					i->SetPoisoned( 0 );
					break;
				}
				if( i->GetHP() < 1 )
				{
					doDeathStuff( i );
					sysmessage( s, 1244 );
				} 
			}
		}
	}

	if( i->GetPoisonWearOffTime() <= cwmWorldState->GetUICurrentTime() )
	{
		if( i->GetPoisoned() )
		{
            i->SetPoisoned( 0 );
			i->SendToSocket( s, true, i );
            sysmessage( s, 1245 );
		}
	}

	bool part1 = doLightEffect( i );
	bool part2 = doRainEffect( i );
	bool part3 = doSnowEffect( i );
	bool part4 = doHeatEffect( i );
	bool part5 = doColdEffect( i );

	if( strUpdate || part1 || part2 || part3 || strUpdate || part4 || part5 )
		updateStats( i, 0 );

	if( dexUpdate || part4 )
		updateStats( i, 2 );

	if( intUpdate )
		updateStats( i, 1 );
	
	if( i->GetHP() <= 0 && !i->IsDead() )
	{
		doDeathStuff( i );
		return true;
	}
	return false;
}

//o---------------------------------------------------------------------------o
//|	Function	-	void checkPC( CChar *i, bool doWeather )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Check a PC's status
//o---------------------------------------------------------------------------o
void checkPC( CChar *i, bool doWeather )
{
	LIGHTLEVEL toShow;
	cSocket *mSock = calcSocketObjFromChar( i );
	
	Magic->CheckFieldEffects2( i, 1 );
	
	bool isOn = isOnline( i );

	if( !i->IsDead() )
	{
		if( i->GetSwingTarg() == INVALIDSERIAL )
			Combat->DoCombat( i );
		else if( Combat->TimerOk( i ) )
			Combat->CombatHit( i, &chars[i->GetSwingTarg()] );
	}	
	
	if( doWeather )
	{
		SI16 curLevel = cwmWorldState->ServerData()->GetWorldLightCurrentLevel();
		if( Races->VisLevel( i->GetRace() ) > curLevel )
			toShow = 0;
		else
			toShow = (UI08)( curLevel - Races->VisLevel( i->GetRace() ) );

		doLight( mSock, toShow );
		Weather->DoPlayerStuff( i );
	}
	
	if( isOn && i->GetSquelched() == 2 )
	{
		if( i->GetMuteTime() !=-1)
		{
			if( (UI32)i->GetMuteTime() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
			{
				i->SetSquelched( 0 );
				i->SetMuteTime( -1 );
				sysmessage( mSock, 1237 );
			}
		}
	}
	
	if( isOn )
	{
		if( (i->GetCrimFlag() > 0 ) && ( (UI32)i->GetCrimFlag() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() ) && i->IsCriminal() )
		{
			sysmessage( mSock, 1238 );
			i->SetCrimFlag( -1 );
			setcharflag( i );
		}
		i->SetMurderRate( i->GetMurderRate() - 1 );
		
		if( (UI32)i->GetMurderRate() <= cwmWorldState->GetUICurrentTime() )
		{
			if( i->GetKills() > 0 )
				i->SetKills( (SI16)( i->GetKills() - 1 ) );
			if( i->GetKills() == cwmWorldState->ServerData()->GetRepMaxKills() ) 
				sysmessage( mSock, 1239 );
			i->SetMurderRate( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetRepMurderDecay() ) );//Murder rate (in mins) to seconds. (checkauto is done about 25 times per second)
		}
		setcharflag( i );
	}
	
	if( i->GetCasting() == 1 )	// Casting a spell
	{
		i->SetNextAct( i->GetNextAct() - 1 );
		if( (UI32)i->GetSpellTime() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )//Spell is complete target it.
		{
			if( spells[i->GetSpellCast()].RequireTarget() )
				target( mSock, 0, 100, spells[i->GetSpellCast()].StringToSay() );
			else
				Magic->CastSpell( mSock, i );
			i->SetCasting( -1 );
			i->SetSpellTime( 0 );
			i->SetFrozen( false );
		} 
		else if( i->GetNextAct() <= 0 )//redo the spell action
		{
			i->SetNextAct( 75 );
			if( !i->IsOnHorse() )
				Effects->impaction( mSock, i->GetSpellAction() );
		}
	}
	
	if( cwmWorldState->ServerData()->GetWorldAmbientSounds() >= 1 )
	{
		if( cwmWorldState->ServerData()->GetWorldAmbientSounds() > 10 ) 
			cwmWorldState->ServerData()->SetWorldAmbientSounds( 10 );
		SI16 soundTimer = cwmWorldState->ServerData()->GetWorldAmbientSounds() * 100;
		if( isOn && !i->IsDead() && ( rand()%( soundTimer ) ) == ( soundTimer / 2 ) ) 
			Effects->bgsound( i ); // bgsound uses array positions not sockets!
	}
	
	if( i->GetSpiritSpeakTimer() > 0 && i->GetSpiritSpeakTimer() < cwmWorldState->GetUICurrentTime() )
		i->SetSpiritSpeakTimer( 0 );
	
	if( i->GetTrackingTimer() > cwmWorldState->GetUICurrentTime() && isOn )
	{
		if( i->GetTrackingDisplayTimer() <= cwmWorldState->GetUICurrentTime() )
		{
			i->SetTrackingDisplayTimer( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetTrackingRedisplayTime() ) );
			Skills->Track( i );
		}
	}
	else
	{
		if( i->GetTrackingTimer() > ( cwmWorldState->GetUICurrentTime() / 10 ) ) // dont send arrow-away packet all the time
		{
			i->SetTrackingTimer( 0 );
			CPTrackingArrow tSend = chars[i->GetTrackingTarget()];
			tSend.Active( 0 );
			mSock->Send( &tSend );
		}
	}
	
	if( i->GetFishingTimer() )
	{
		if( i->GetFishingTimer() <= cwmWorldState->GetUICurrentTime() )
		{
			Skills->Fish( i );
			i->SetFishingTimer( 0 );
		}
	}
	
	if( i->IsOnHorse() )
	{
		CItem *horseItem = i->GetItemAtLayer( 0x19 );
		if( horseItem == NULL )
			i->SetOnHorse( false );	// turn it off, we aren't on one because there's no item!
		else if( horseItem->GetDecayTime() != 0 && ( horseItem->GetDecayTime() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() ) )
		{
			i->SetOnHorse( false );
			Items->DeleItem( horseItem );
		}
	}
	
}

//o---------------------------------------------------------------------------o
//|	Function	-	void checkNPC( CChar *i )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Check NPC's status
//o---------------------------------------------------------------------------o
void checkNPC( CChar *i )
{
	if( i == NULL || i->isFree() )
		return;
	// okay, this will only ever trigger after we check an npc...  Question is:
	// should we remove the time delay on the AI check as well?  Just stick with AI/movement
	// AI can never be faster than how often we check npcs
	// This periodically generates access violations.  No idea why either
	UI16 AITrig = i->GetScriptTrigger();
	cScript *toExecute = Trigger->GetScript( AITrig );
	bool doAICheck = true;
	if( toExecute != NULL )
	{
		if( toExecute->OnAISliver( i ) )
			doAICheck = false;
	}
	if( doAICheck )
		Npcs->CheckAI( i );
	Movement->NpcMovement( i );
	setcharflag( i );		// possibly not... How many times do we want to set this? We've set it twice in the calling function

	if( cwmWorldState->GetShopRestockTime() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
		restockNPC( i, false );

	if( i->GetOwnerObj() != NULL && i->GetHunger() == 0 && i->GetNPCAiType() != 17 ) // tamed animals but not player vendors ;)=
	{
		Effects->tempeffect( i, i, 44, 0, 0, 0 ); // (go wild in some minutes ...)-effect
		i->SetHunger( -1 );
	}
	
	if( i->GetSummonTimer() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
	{
		if( i->GetSummonTimer() > 0 )
		{
			// Dupois - Added Dec 20, 1999
			// QUEST expire check - after an Escort quest is created a timer is set
			// so that the NPC will be deleted and removed from the game if it hangs around
			// too long without every having its quest accepted by a player so we have to remove 
			// its posting from the messageboard before icing the NPC
			// Only need to remove the post if the NPC does not have a follow target set
			if( i->GetQuestType() == ESCORTQUEST && i->GetFTarg() == INVALIDSERIAL )
			{
				MsgBoardQuestEscortRemovePost( i );
				MsgBoardQuestEscortDelete( i );
				return;
			}
			// Dupois - End
			if( i->GetNPCAiType() == 4 && i->IsAtWar() )
			{
				i->SetSummonTimer( BuildTimeValue( 25 ) );
				return;
			}
			Effects->PlaySound( i, 0x01FE );
			i->SetDead( true );
			Npcs->DeleteChar( i );
			return;
		}
	}
	
	if( i->GetFleeAt() == 0 ) 
		i->SetFleeAt( cwmWorldState->ServerData()->GetCombatNPCBaseFleeAt() );
	if( i->GetReattackAt() == 0 ) 
		i->SetReattackAt( cwmWorldState->ServerData()->GetCombatNPCBaseReattackAt() );
	
	if( i->GetNpcWander() != 5 && ( i->GetHP() < i->GetStrength() * i->GetFleeAt() / 100 ) )
	{
		i->SetOldNpcWander( i->GetNpcWander() );
		i->SetNpcWander( 5 );
		i->SetNpcMoveTime( BuildTimeValue( (R32)( cwmWorldState->ServerData()->GetNPCSpeed() / 2 ) ) );	// fleeing enemies are 2x faster
	}
	
	if( i->GetNpcWander() == 5 && (i->GetHP() > i->GetStrength() * i->GetReattackAt() / 100))
	{
		i->SetNpcWander( i->GetOldNpcWander() );
		i->SetNpcMoveTime( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetNPCSpeed() ) );
		i->SetOldNpcWander( 0 ); // so it won't save this at the wsc file
	}

	Magic->CheckFieldEffects2( i, 0 );

	if( !i->IsDead() )
	{
		if( i->GetSwingTarg() == INVALIDSERIAL )
			Combat->DoCombat( i );
		else if( Combat->TimerOk( i ) )
			Combat->CombatHit( i, &chars[i->GetSwingTarg()] );
	}
}

void checkItem( SubRegion *toCheck, UI32 checkitemstime )
{
	for( CItem *itemCheck = toCheck->FirstItem(); !toCheck->FinishedItems(); itemCheck = toCheck->GetNextItem() )
	{
		if( itemCheck == NULL )
			break;
		if( checkitemstime <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
		{
			Items->RespawnItem( itemCheck );
			if( itemCheck->GetType() == 51 || itemCheck->GetType() == 52 )
			{
				if( itemCheck->GetGateTime() <= cwmWorldState->GetUICurrentTime() )
				{
					Items->DeleItem( itemCheck );
					continue;
				}
			}
			if( Items->DecayItem( itemCheck ) )
				continue;

			if( itemCheck->GetType() == 88 && itemCheck->GetMoreY() < 25 )
			{
				if( (UI32)RandomNum( 1, 100 ) <= itemCheck->GetMoreZ() )
				{
					Network->PushConn();
					for( cSocket *eSock = Network->FirstSocket(); !Network->FinishedSockets(); eSock = Network->NextSocket() )
					{
						if( objInRange( eSock, itemCheck, itemCheck->GetMoreY() ) )
								Effects->PlaySound( eSock, (UI16)itemCheck->GetMoreX(), false );
					}
					Network->PopConn();
				}
			}
		} 
		if( itemCheck->GetType() == 117 && ( itemCheck->GetType2() == 1 || itemCheck->GetType2() == 2 ) && 
			( itemCheck->GetGateTime() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() ) )
		{
			Network->PushConn();
			for( cSocket *bSock = Network->FirstSocket(); !Network->FinishedSockets(); bSock = Network->NextSocket() )
			{
				if( objInRange( bSock, itemCheck, 18 ) )
				{
					if( itemCheck->GetType2() == 1 ) 
						Boats->MoveBoat( bSock, itemCheck->GetDir(), itemCheck );
					else 
					{
						UI08 dir = (UI08)( itemCheck->GetDir() + 4 );
						if( dir > 7 ) 
							dir -= 8;
						Boats->MoveBoat( bSock, dir, itemCheck );
					}
				}
			}
			Network->PopConn();
			itemCheck->SetGateTime( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetCheckBoatSpeed() ) );
		}
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	void checkauto( void )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Check automatic and timer controlled functions
//o---------------------------------------------------------------------------o
void checkauto( void )
{
	static UI32 checkspawnregions=0; 
	static UI32 checknpcs=0;
	static UI32 checkitemstime=0;
	static UI32 uiSetFlagTime = 0;
	static UI32 accountFlush = 0;
	static UI32 regionCheck = 0;
	static UI32 counter = 0;
	bool doWeather = false;
	ACCOUNTSBLOCK actbTemp;
	MAPUSERNAMEID_ITERATOR I;
	//
	Trigger->PeriodicTriggerCheck();
	// modify this stuff to take into account more variables
	if( /*cwmWorldState->ServerData()->GetAccountFlushTimer() != 0 &&*/ accountFlush <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
	{
		bool reallyOn = false;
		// time to flush our account status!
		//for( ourAccount = Accounts->FirstAccount(); !Accounts->FinishedAccounts(); ourAccount = Accounts->NextAccount() )
		for(I=Accounts->begin();I!=Accounts->end();I++)
		{
			ACCOUNTSBLOCK actbBlock;
			actbBlock=I->second;
			if( actbBlock.wAccountIndex==AB_INVALID_ID)
				continue;

			if(actbTemp.wFlags&AB_FLAGS_ONLINE)
			{
				reallyOn = false;	// to start with, there's no one really on
				Network->PushConn();
				for( cSocket *tSock = Network->FirstSocket(); !Network->FinishedSockets(); tSock = Network->NextSocket() )
				{
					CChar *tChar = tSock->CurrcharObj();
					if( tChar == NULL )
						continue;
					if( tChar->GetAccount().wAccountIndex == actbTemp.wAccountIndex)
						reallyOn = true;
				}
				Network->PopConn();
				if( !reallyOn )	// no one's really on, let's set that
					actbTemp.wFlags&=0xFFF7;
			}
		}
		accountFlush = BuildTimeValue( (R32)cwmWorldState->ServerData()->GetAccountFlushTimer() * 60 );
	}
	Network->On();

	if( cwmWorldState->GetWorldSaveProgress() == 0 )
	{
		for( SI32 ij = cwmWorldState->GetPlayersOnline() - 1; ij >= 0; ij-- )
		{
			cSocket *tSock = calcSocketObjFromSock( ij );
			if( tSock->IdleTimeout() != -1 && (UI32)tSock->IdleTimeout() <= cwmWorldState->GetUICurrentTime() )
			{
				CChar *tChar = tSock->CurrcharObj();
				if( tChar == NULL )
					continue;
				if( !tChar->IsGM() )
				{
					tSock->IdleTimeout( -1 );
					sysmessage( tSock, 1246 );
					Network->Disconnect( ij );
				}
			}
			else if( ( ( (UI32)( tSock->IdleTimeout()+300*CLOCKS_PER_SEC ) <= cwmWorldState->GetUICurrentTime() && (UI32)( tSock->IdleTimeout()+200*CLOCKS_PER_SEC ) >= cwmWorldState->GetUICurrentTime() ) || cwmWorldState->GetOverflow() ) && !tSock->WasIdleWarned()  )
			{//is their idle time between 3 and 5 minutes, and they haven't been warned already?
				CPIdleWarning warn( 0x07 );
				tSock->Send( &warn );
				tSock->WasIdleWarned( true );
			}
		}
	}
	else if( cwmWorldState->GetWorldSaveProgress() == 2 )	// if we've JUST saved, do NOT kick anyone off (due to a possibly really long save), but reset any offending players to 60 seconds to go before being kicked off
	{
		Network->PushConn();
		for( cSocket *wsSocket = Network->FirstSocket(); !Network->FinishedSockets(); wsSocket = Network->NextSocket() )
		{
			if( wsSocket != NULL )
			{
				if( (UI32)wsSocket->IdleTimeout() < cwmWorldState->GetUICurrentTime() )
				{
					wsSocket->IdleTimeout( BuildTimeValue( 60.0f ) );
					wsSocket->WasIdleWarned( true );//don't give them the message if they only have 60s
				}
			}
		}
		Network->PopConn();
		cwmWorldState->SetWorldSaveProgress( 0 );
	}
	Network->Off();
	if( regionCheck <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
	{
		for( UI16 regionCounter = 0; regionCounter < 256; regionCounter++ )
			region[regionCounter]->PeriodicCheck();
		regionCheck = BuildTimeValue( 10 );	// do checks every 10 seconds or so, rather than every single time
		JailSys->PeriodicCheck();
	}

	if( checkspawnregions <= cwmWorldState->GetUICurrentTime() && cwmWorldState->ServerData()->GetCheckSpawnRegionSpeed() != -1 )//Regionspawns
	{
		for( UI16 i = 1; i < cwmWorldState->GetTotalSpawnRegions(); i++ )
		{
			cSpawnRegion *spawnReg = spawnregion[i];
			if( spawnReg != NULL && (UI32)spawnReg->GetNextTime() <= cwmWorldState->GetUICurrentTime() )
			{
				spawnReg->doRegionSpawn();
				spawnReg->SetNextTime( BuildTimeValue( (R32)( 60 * RandomNum( spawnReg->GetMinTime(), spawnReg->GetMaxTime() ) ) ) );
			}
		}
		checkspawnregions = BuildTimeValue( (R32)cwmWorldState->ServerData()->GetCheckSpawnRegionSpeed() );//Don't check them TOO often (Keep down the lag)
	}
	
	HTMLTemplates->Poll( ETT_PERIODIC );

	UI32 saveinterval = cwmWorldState->ServerData()->GetServerSavesTimerStatus();
	if( saveinterval != 0 )
	{
		UI32 oldTime = cwmWorldState->GetOldTime();
		if( !cwmWorldState->GetAutoSaved() )
		{
			cwmWorldState->SetAutoSaved( true );
			time((time_t *)&oldTime);
			cwmWorldState->SetOldTime( oldTime );
		}
		UI32 newTime = cwmWorldState->GetNewTime();
		time((time_t *)&newTime);
		cwmWorldState->SetNewTime( newTime );

		if( difftime( cwmWorldState->GetNewTime(), cwmWorldState->GetOldTime() ) >= saveinterval || cwmWorldState->Saving() )
		{
			// Dupois - Added Dec 20, 1999
			// After an automatic world save occurs, lets check to see if
			// anyone is online (clients connected).  If nobody is connected
			// Lets do some maintenance on the bulletin boards.
			if( !cwmWorldState->GetPlayersOnline() && !cwmWorldState->Saving() )
			{
				Console << "No players currently online. Starting bulletin board maintenance" << myendl;
				Console.Log( "Bulletin Board Maintenance routine running (AUTO)", "server.log" );
				MsgBoardMaintenance();
			}

			cwmWorldState->SetAutoSaved( false );
			cwmWorldState->savenewworld( false );
		}
	}
	
	//Time functions
	if( cwmWorldState->GetUOTickCount() <= cwmWorldState->GetUICurrentTime() || ( cwmWorldState->GetOverflow() ) )
	{
		if( cwmWorldState->ServerData()->IncMinute() )
			Weather->NewDay();
		cwmWorldState->SetUOTickCount( BuildTimeValue( cwmWorldState->GetSecondsPerUOMinute() ) );
	}
	
	if( cwmWorldState->GetLightTime() <= cwmWorldState->GetUICurrentTime() || ( cwmWorldState->GetOverflow() ) )
	{
		counter = 0;		
		cwmWorldState->doWorldLight();  //Changes lighting, if it is currently time to.
		Weather->DoStuff();	// updates the weather types
		cwmWorldState->SetLightTime( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetSystemTimerStatus( WEATHER ) ) );	// for testing purposes
		doWeather = true;
	}
	else
		doWeather = false;

	std::set< SubRegion * > regionList;	// we'll get around our npc problem this way, hopefully
	Network->PushConn();
	for( cSocket *iSock = Network->FirstSocket(); !Network->FinishedSockets(); iSock = Network->NextSocket() )
	{
		if( iSock == NULL )
			continue;
		CChar *mChar = iSock->CurrcharObj();
		UI08 worldNumber = mChar->WorldNumber();
		if( isOnline( mChar ) && mChar->GetAccount().wAccountIndex == iSock->AcctNo() )
		{
			if( uiSetFlagTime <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
				setcharflag( mChar ); // only set flag on npcs every 60 seconds (save a little extra lag)
			genericCheck( mChar );
			checkPC( mChar, doWeather );

			int xOffset = MapRegion->GetGridX( mChar->GetX() );
			int yOffset = MapRegion->GetGridY( mChar->GetY() );
			for( SI08 counter = -1; counter <= 1; counter ++ ) // Check 3 x colums
			{
				for( SI08 ctr2 = -1; ctr2 <= 1; ctr2++ ) // Check 3 y colums
				{
					SubRegion *tC = MapRegion->GetGrid( xOffset + counter, yOffset + ctr2, worldNumber );
					if( tC == NULL )
						continue;
					regionList.insert( tC );
				}
			}
		}
	}
	Network->PopConn();
	std::set< SubRegion * >::iterator tcCheck = regionList.begin();
	while( tcCheck != regionList.end() )
	{
		SubRegion *toCheck = (*tcCheck);
		toCheck->PushChar();
		toCheck->PushItem();
		for( CChar *charCheck = toCheck->FirstChar(); !toCheck->FinishedChars(); charCheck = toCheck->GetNextChar() )
		{
			if( charCheck == NULL )//|| charCheck (int)charCheck == 0xCDCDCDCD )	// for some reason, 0xcdcdcdcd is always the bad thing here
				continue;
			if( charCheck->isFree() ) 
				continue;
			if( uiSetFlagTime <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() )
				setcharflag( charCheck ); // only set flag on npcs every 60 seconds (save a little extra lag)
			if( charCheck->IsNpc() ) 
			{
				bool delChar = genericCheck( charCheck );
				if( !delChar )
					checkNPC( charCheck );
			}
			else if( charCheck->GetLogout() != -1 )
			{
				actbTemp = charCheck->GetAccount();
				if(actbTemp.wAccountIndex != AB_INVALID_ID)
				{
					SERIAL oaiw = actbTemp.dwInGame;
					if( oaiw == INVALIDSERIAL )
					{
						charCheck->SetLogout( -1 );
						charCheck->Update();
					}
					else if( oaiw == charCheck->GetSerial() && ( (UI32)charCheck->GetLogout() <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() ) )
					{
						actbTemp.dwInGame = INVALIDSERIAL;
						charCheck->SetLogout( -1 );
						charCheck->Update();
					}
				}
			}
		}
		toCheck->PopChar();

		checkItem( toCheck, checkitemstime );

		toCheck->PopItem();
		tcCheck++;
	}
	Effects->checktempeffects();
	SpeechSys->Poll();
	if( uiSetFlagTime <= cwmWorldState->GetUICurrentTime() )
		uiSetFlagTime = BuildTimeValue( 30 ); // Slow down lag "needed" for setting flags, they are set often enough ;-)
	if( checknpcs <= cwmWorldState->GetUICurrentTime() ) 
		checknpcs = BuildTimeValue( (R32)cwmWorldState->ServerData()->GetCheckNpcSpeed() );
	if( checkitemstime <= cwmWorldState->GetUICurrentTime() ) 
		checkitemstime = BuildTimeValue( (R32)cwmWorldState->ServerData()->GetCheckItemsSpeed() );
	if( cwmWorldState->GetShopRestockTime() <= cwmWorldState->GetUICurrentTime() ) 
		cwmWorldState->SetShopRestockTime( BuildTimeValue( (R32)( cwmWorldState->ServerData()->GetSystemTimerStatus( SHOP_SPAWN ) ) ) );
	if( cwmWorldState->GetNextNPCAITime() <= cwmWorldState->GetUICurrentTime() ) 
		cwmWorldState->SetNextNPCAITime( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetCheckNpcAISpeed() ) );
	if( cwmWorldState->GetNextFieldEffectTime() <= cwmWorldState->GetUICurrentTime() ) 
		cwmWorldState->SetNextFieldEffectTime( BuildTimeValue( 0.5f ) );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void LoadJSEngine( void )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Loads JavaScript engine
//o---------------------------------------------------------------------------o
void LoadJSEngine( void )
{
	const SI32 DefEngineSize = 0x1000000;

	std::ifstream engineData( "engine.dat" );
	SI32 engineSize = DefEngineSize;
	if( engineData.is_open() )
	{
		char line[1024];
		engineData.getline( line, 1024 );
		if( strlen( line ) > 0 )
		{
			engineSize = makeNum( line );
			if( engineSize < DefEngineSize )
				engineSize = DefEngineSize;
		}
		engineData.close();
	}
	jsRuntime = JS_NewRuntime( engineSize );
	
	Console.PrintSectionBegin();
	Console << "Starting JavaScript Engine...." << myendl;
	
	if( jsRuntime == NULL )
		Shutdown( FATAL_UOX3_JAVASCRIPT );
	jsContext = JS_NewContext( jsRuntime, 0x2000 );
	if( jsContext == NULL )
		Shutdown( FATAL_UOX3_JAVASCRIPT );
	jsGlobal = JS_NewObject( jsContext, &global_class, NULL, NULL ); 
	if( jsGlobal == NULL )
		Shutdown( FATAL_UOX3_JAVASCRIPT );
	JS_InitStandardClasses( jsContext, jsGlobal ); 
	Console << "JavaScript engine startup complete." << myendl;
	Console.PrintSectionBegin();
}

//o---------------------------------------------------------------------------o
//|	Function	-	void InitClasses( void )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Initialize UOX classes
//o---------------------------------------------------------------------------o
void InitClasses( void )
{
	Console << "Initializing and creating class pointers... " << myendl;
	
	Boats = NULL;			Gumps = NULL;	
	Combat = NULL;			Commands = NULL;
	Items = NULL;			Map = NULL;
	Npcs = NULL;			Skills = NULL;	
	Weight = NULL;			Targ = NULL;
	Network = NULL;			Magic = NULL;		
	Races = NULL;			Weather = NULL;
	Movement = NULL;		TEffects = NULL;	
	WhoList = NULL;			OffList = NULL;
	Books = NULL;			GMQueue = NULL;	
	Dictionary = NULL;		Accounts = NULL;
	MapRegion = NULL;		SpeechSys = NULL;
	CounselorQueue = NULL;	GuildSys = NULL;
	HTMLTemplates = NULL;	Effects = NULL;

	// MAKE SURE IF YOU ADD A NEW ALLOCATION HERE THAT YOU FREE IT UP IN Shutdown(...)
	if(( Dictionary = new CDictionaryContainer ) == NULL ) Shutdown( FATAL_UOX3_ALLOC_DICTIONARY );
	if(( Boats = new cBoat )              == NULL ) Shutdown( FATAL_UOX3_ALLOC_BOATS );
	if(( Combat = new cCombat )           == NULL ) Shutdown( FATAL_UOX3_ALLOC_COMBAT );
	if(( Commands = new cCommands )       == NULL ) Shutdown( FATAL_UOX3_ALLOC_COMMANDS );
	if(( Gumps = new cGump )              == NULL ) Shutdown( FATAL_UOX3_ALLOC_GUMPS );
	if(( Items = new cItem )              == NULL ) Shutdown( FATAL_UOX3_ALLOC_ITEMS );
	if(( Map = new cMapStuff )            == NULL ) Shutdown( FATAL_UOX3_ALLOC_MAP );
	if(( Npcs = new cCharStuff )          == NULL ) Shutdown( FATAL_UOX3_ALLOC_NPCS );
	if(( Skills = new cSkills )           == NULL ) Shutdown( FATAL_UOX3_ALLOC_SKILLS );
	if(( Weight = new cWeight )           == NULL ) Shutdown( FATAL_UOX3_ALLOC_WEIGHT );
	if(( Targ = new cTargets )            == NULL ) Shutdown( FATAL_UOX3_ALLOC_TARG );
	if(( Network = new cNetworkStuff )    == NULL ) Shutdown( FATAL_UOX3_ALLOC_NETWORK );
	if(( Magic = new cMagic )             == NULL ) Shutdown( FATAL_UOX3_ALLOC_MAGIC );
	if(( Races = new cRaces )             == NULL ) Shutdown( FATAL_UOX3_ALLOC_RACES );
	if(( Weather = new cWeatherAb )       == NULL ) Shutdown( FATAL_UOX3_ALLOC_WEATHER );
	if(( Movement = new cMovement )       == NULL ) Shutdown( FATAL_UOX3_ALLOC_MOVE );
	if(( TEffects = new cTEffect )         == NULL ) Shutdown( FATAL_UOX3_ALLOC_TEMPEFFECTS );	// addition of TEffect class, memory reduction (Abaddon, 17th Feb 2000)
	if(( WhoList = new cWhoList )         == NULL ) Shutdown( FATAL_UOX3_ALLOC_WHOLIST );	// wholist
	if(( OffList = new cWhoList( false ) )== NULL ) Shutdown( FATAL_UOX3_ALLOC_WHOLIST );	// offlist
	if(( Books = new cBooks )			  == NULL ) Shutdown( FATAL_UOX3_ALLOC_BOOKS );	// temp value
	if(( GMQueue = new PageVector( "GM Queue" ) )		  == NULL ) Shutdown( FATAL_UOX3_ALLOC_PAGEVECTOR );
	if(( CounselorQueue = new PageVector( "Counselor Queue" ) )== NULL ) Shutdown( FATAL_UOX3_ALLOC_PAGEVECTOR );
	if(( Trigger = new Triggers )		  == NULL ) Shutdown( FATAL_UOX3_ALLOC_TRIGGERS );
	if(( MapRegion = new cMapRegion )	  == NULL ) Shutdown( FATAL_UOX3_ALLOC_MAPREGION );
	if(( Effects = new cEffects )		  == NULL )	Shutdown( FATAL_UOX3_ALLOC_EFFECTS );
	if(( FileIO = new cFileIO )			  == NULL ) Shutdown( FATAL_UOX3_ALLOC_FILEIO );
	
	HTMLTemplates = new cHTMLTemplates;

	//const char *path = cwmWorldState->ServerData()->GetAccountsDirectory();
	//char tbuffer[512];
	//UI32 slen = strlen( path );
	//if( path[slen-1] == '\\' || path[slen-1] == '/' )
	//	sprintf( tbuffer, "%saccounts.adm", path );
	//else
	//	sprintf( tbuffer, "%s/accounts.adm", path );
	
	if(( Accounts = new cAccountClass( cwmWorldState->ServerData()->GetAccountsDirectory()/*tbuffer*/ ) ) == NULL ) Shutdown( FATAL_UOX3_ALLOC_ACCOUNTS );
	if(( SpeechSys = new CSpeechQueue()	)  == NULL ) Shutdown( FATAL_UOX3_ALLOC_SPEECH );
	if(( GuildSys = new CGuildCollection() ) == NULL ) Shutdown( FATAL_UOX3_ALLOC_GUILDS );
	if(( FileLookup = new cServerDefinitions() ) == NULL )	Shutdown( FATAL_UOX3_ALLOC_SCRIPTS );
	if(( JailSys = new JailSystem() ) == NULL ) Shutdown( FATAL_UOX3_ALLOC_WHOLIST );
	Map->Cache = cwmWorldState->ServerData()->GetServerMulCachingStatus();	
	//Console << " Done loading classes!" << myendl;
	DefBase = new cBaseObject();
	DefChar = new CChar(INVALIDSERIAL,true);
	DefItem = new CItem(INVALIDSERIAL,true);
}

//o---------------------------------------------------------------------------o
//|	Function	-	void ParseArgs( int argc, char *argv[] )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Unknown
//o---------------------------------------------------------------------------o
void ParseArgs( int argc, char *argv[] )
{
	for( int i = 1; i < argc; i++ )
	{
		if( !strcmp( argv[i], "-xgm" ) ) 
		{
			Console.PrintSectionBegin();
			Console << "XGM Enabled! Initializing... ";
			cwmWorldState->SetXGMEnabled( true );
			Console << "Done!" << myendl;
		}
		else if( !strcmp( argv[i], "-ERROR" ) )
		{
			cwmWorldState->SetErrorCount( makeNum( argv[i+1] ) );
			i++;
		}
		else if( !strcmp( argv[i], "-cluox100" ) )
		{
			Console << "Using CLUOX Streaming-IO" << myendl;
			setvbuf( stdout, NULL, _IONBF, 0 );
			setvbuf( stderr, NULL, _IONBF, 0 );
			cluox_io = true;
			i++;
			if( i > argc )
			{
				Console.Error( 0, "Fatal error in CLUOX arguments" );
				Shutdown( 10 );
			}
			char *dummy;
			cluox_stdin_writeback = (void *)strtol( argv[i], &dummy, 16 );
		}
		else if( !strncmp( argv[i], "+add:", sizeof(char)*5 ) )
		{
			//	EviLDeD:	030902:	Added this so people could add an account at the command line when they started the server
			Console << "|  Importing Accounts command line \n"; 
			std::string username,password,email;
			char *left=strtok(argv[1],":");
			username=strtok(NULL,",");
			password=strtok(NULL,",");
			email=strtok(NULL,"\n");
			if( left==NULL || username.empty() || password.empty() || email.empty() )
			{
				// there is an error with the command line so we don't want to do anything
				break;
			}
			ACCOUNTSBLOCK actbTemp;
			if(!Accounts->GetAccountByName(username.c_str(),actbTemp))
			{
				Accounts->AddAccount( username, password, email );
				Console << "| AccountImport: Added: " << username << " @ " << email << "\n";
			}
			else
			{
				Console << "| AccountImport: Failure\n";
			}
		}
		else if( !strncmp( argv[i], "+import:", sizeof(char)*8 ) )
		{
			//	EviLDeD:	030902:	Added this so people could add accounts froma file that contains username/password/email format per line
			std::string  filename,username,password,email;
			char *left;
			left			= strtok(argv[1],":");
			filename	= strtok(NULL,",");
			std::ifstream inFile;
			inFile.open( filename.c_str() );
			char szBuffer[127];
			if(inFile.is_open())
			{
				// Ok the files is open lets read it. Otherwise just skip it
				Console << "|  Importing Accounts from \"" << filename << "\"\n"; 
				inFile.getline( szBuffer,127 );
				while(  !inFile.eof() )
				{
					if( (left = strtok(szBuffer,":")) == NULL)
					{
						inFile.getline(szBuffer,127);
						continue;
					}
					left			= strtok(szBuffer,":");
					username	= strtok(NULL,",");
					password	= strtok(NULL,",");
					email			= strtok(NULL,"\n");
					if( left==NULL || username.empty() || password.empty() || email.empty() )
					{
						inFile.getline(szBuffer,127);
						continue;
					}
					ACCOUNTSBLOCK actbTemp;
					if(!Accounts->GetAccountByName(username.c_str(),actbTemp))
					{
						Accounts->AddAccount(username, password, email );
						Console << "| AccountImport: Added: " << username << " @ " << email << "\n";
					}
					else
					{
						Console << "| AccountImport: Failure\n";
					}
					inFile.getline( szBuffer,127);
				};
			}
		}
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	void ResetVars( void )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Reset global variables	(Many are now set in CWorldMain())
//o---------------------------------------------------------------------------o
void ResetVars( void )
{
	Console.PrintSectionBegin();
	Console << "Initializing global variables  ";
	globalSent = 0;
	globalRecv = 0;
	
	strcpy( cwmWorldState->skill[ALCHEMY].madeword,			"mixed" );
	strcpy( cwmWorldState->skill[ANATOMY].madeword,			"made" );
	strcpy( cwmWorldState->skill[ANIMALLORE].madeword,		"made" );
	strcpy( cwmWorldState->skill[ITEMID].madeword,			"made" );
	strcpy( cwmWorldState->skill[ARMSLORE].madeword,		"made" );
	strcpy( cwmWorldState->skill[PARRYING].madeword,		"made" );
	strcpy( cwmWorldState->skill[BEGGING].madeword,			"made" );
	strcpy( cwmWorldState->skill[BLACKSMITHING].madeword,	"forged" );
	strcpy( cwmWorldState->skill[BOWCRAFT].madeword,		"bowcrafted" );
	strcpy( cwmWorldState->skill[PEACEMAKING].madeword,		"made" );
	strcpy( cwmWorldState->skill[CAMPING].madeword,			"made" );
	strcpy( cwmWorldState->skill[CARPENTRY].madeword,		"made" );
	strcpy( cwmWorldState->skill[CARTOGRAPHY].madeword,		"wrote" );
	strcpy( cwmWorldState->skill[COOKING].madeword,			"cooked" );
	strcpy( cwmWorldState->skill[DETECTINGHIDDEN].madeword, "made" );
	strcpy( cwmWorldState->skill[ENTICEMENT].madeword,		"made" );
	strcpy( cwmWorldState->skill[EVALUATINGINTEL].madeword, "made" );
	strcpy( cwmWorldState->skill[HEALING].madeword,			"made" );
	strcpy( cwmWorldState->skill[FISHING].madeword,			"made" );
	strcpy( cwmWorldState->skill[FORENSICS].madeword,		"made" );
	strcpy( cwmWorldState->skill[HERDING].madeword,			"made" );
	strcpy( cwmWorldState->skill[HIDING].madeword,			"made" );
	strcpy( cwmWorldState->skill[PROVOCATION].madeword,		"made" );
	strcpy( cwmWorldState->skill[INSCRIPTION].madeword,		"wrote" );
	strcpy( cwmWorldState->skill[LOCKPICKING].madeword,		"made" );
	strcpy( cwmWorldState->skill[MAGERY].madeword,			"envoked" );
	strcpy( cwmWorldState->skill[MAGICRESISTANCE].madeword, "made" );
	strcpy( cwmWorldState->skill[TACTICS].madeword,			"made" );
	strcpy( cwmWorldState->skill[SNOOPING].madeword,		"made" );
	strcpy( cwmWorldState->skill[MUSICIANSHIP].madeword,	"made" );
	strcpy( cwmWorldState->skill[POISONING].madeword,		"made" );
	strcpy( cwmWorldState->skill[ARCHERY].madeword,			"made" );
	strcpy( cwmWorldState->skill[SPIRITSPEAK].madeword,		"made" );
	strcpy( cwmWorldState->skill[STEALING].madeword,		"made" );
	strcpy( cwmWorldState->skill[TAILORING].madeword,		"sewn" );
	strcpy( cwmWorldState->skill[TAMING].madeword,			"made" );
	strcpy( cwmWorldState->skill[TASTEID].madeword,			"made" );
	strcpy( cwmWorldState->skill[TINKERING].madeword,		"made" );
	strcpy( cwmWorldState->skill[TRACKING].madeword,		"made" );
	strcpy( cwmWorldState->skill[VETERINARY].madeword,		"made" );
	strcpy( cwmWorldState->skill[SWORDSMANSHIP].madeword,	"made" );
	strcpy( cwmWorldState->skill[MACEFIGHTING].madeword,	"made" );
	strcpy( cwmWorldState->skill[FENCING].madeword,			"made" );
	strcpy( cwmWorldState->skill[WRESTLING].madeword,		"made" );
	strcpy( cwmWorldState->skill[LUMBERJACKING].madeword,	"made" );
	strcpy( cwmWorldState->skill[MINING].madeword,			"smelted" );
	strcpy( cwmWorldState->skill[MEDITATION].madeword,		"envoked" );
	strcpy( cwmWorldState->skill[STEALTH].madeword,			"made" );
	strcpy( cwmWorldState->skill[REMOVETRAPS].madeword,		"made" );

	Console.PrintDone();
}

//o---------------------------------------------------------------------------o
//|	Function	-	void InitMultis( void )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Initialize Multis
//o---------------------------------------------------------------------------o
void InitMultis( void )
{
	Console << "Initializing multis            ";

	CMultiObj *multi = NULL;
	for( CHARACTER c = 0; c < cwmWorldState->GetCharCount(); c++ )
	{
		if( !chars[c].isFree() )
		{
			multi = findMulti( &chars[c] );
			if( multi != NULL )
				chars[c].SetMulti( multi );
			else
				chars[c].SetMulti( INVALIDSERIAL );
		}
	}
	int fiveCount = cwmWorldState->GetItemCount() / 22;
	if( fiveCount == 0 )
		fiveCount = 1;
	for( ITEM i = 0; i < cwmWorldState->GetItemCount(); i++ )
	{
		if( !items[i].isFree() && items[i].GetCont() == NULL )
		{
			multi = findMulti( &items[i] );
			if( multi != NULL )
			{
				if( multi != &items[i] ) 
					items[i].SetMulti( multi );
				else 
					items[i].SetMulti( INVALIDSERIAL );
			}
			else
				items[i].SetMulti( INVALIDSERIAL );
		}
	}
	Console.PrintDone();
}

//o---------------------------------------------------------------------------o
//|	Function	-	void DisplayBanner( void )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	UOX startup stuff
//o---------------------------------------------------------------------------o
void DisplayBanner( void )
{
	Console.PrintSectionBegin();

	char idname[256];
	sprintf( idname, "%s v%s(%s) [%s]\n| Compiled by %s\n| Programmed by: %s", CVC.GetProductName(), CVC.GetVersion(), CVC.GetBuild(), OS_STR, CVC.GetName(), CVC.GetProgrammers() );
 
	/*Console << myendl;
	Console << "Configured for connections with Ignition support and 2.x compatability" << myendl;
	Console << "Copyright (C) 1997, 98 Marcus Rating (Cironian)" << myendl;//<< "|" << myendl;
	Console << "This program is free software; you can redistribute it and/or modify" << myendl;
	Console << "it under the terms of the GNU General Public License as published by" << myendl;
	Console << "the Free Software Foundation; either version 2 of the License, or" << myendl;
	Console << "(at your option) any later version." << myendl << "|" << myendl;
	Console << myendl;*/
	
	Console.TurnYellow();
	Console << "Compiled on ";
	Console.TurnNormal();
	Console << __DATE__ << " (" << __TIME__ << ")" << myendl;

	Console.TurnYellow();
	Console << "Compiled by ";
	Console.TurnNormal();
	Console << CVC.GetName() << myendl;

	Console.TurnYellow();
	Console << "Contact: ";
	Console.TurnNormal();
	Console << CVC.GetEmail() << myendl;
	
	Console.PrintSectionBegin();
}

//o---------------------------------------------------------------------------o
//|	Function	-	void SleepNiceness( void )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Based on server.scp entry, choose how much CPU UOX will hog
//o---------------------------------------------------------------------------o
void SleepNiceness( void )
{
	switch( cwmWorldState->ServerData()->GetNiceness() )
	{
	case 0:						break;  // very unnice - hog all cpu time
	case 1:
		if( cwmWorldState->GetPlayersOnline() != 0 ) 
			UOXSleep( 10 );
		else 
			UOXSleep( 90 );
		break;
	case 2: UOXSleep( 10 );		break;
	case 3: UOXSleep( 40 );		break;// very nice
	default: UOXSleep( 10 );	break;
	}

}

//o---------------------------------------------------------------------------o
//|	Function	-	void StartupClearTrades( void )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	UOX startup stuff
//o---------------------------------------------------------------------------o
void StartupClearTrades( void )
{
	Console.PrintSectionBegin();
	Console << "Clearing all trades            ";
	cwmWorldState->SetLoaded( true );
	clearTrades();
	Console.PrintDone();
}

//o---------------------------------------------------------------------------o
//|	Function	-	int main( int argc, char *argv[] )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	UOX startup stuff
//o---------------------------------------------------------------------------o
int main( int argc, char *argv[] )
{
	UI32 uiNextCheckConn=0;
	UI32 tempSecs, tempMilli, tempTime;
	UI32 loopSecs, loopMilli;

	// EviLDeD: 042102: I moved this here where it basically should be for any windows application or dll that uses WindowsSockets.
#if !defined(__unix__)
	wVersionRequested = MAKEWORD( 2, 0 );
	SI32 err = WSAStartup( wVersionRequested, &wsaData );
	if( err )
	{
		Console.Error( 0, "Winsock 2.0 not found on your system..." );
		return 1;
	}
#endif

	char temp[1024];
#ifdef _CRASH_PROTECT_
	try {//Crappy error trapping....
#endif
		UI32 currentTime = getclock();
		
#if !defined(__unix__)
		sprintf( temp, "%s v%s(%s)", CVC.GetProductName(), CVC.GetVersion(), CVC.GetBuild() );
		Console.Start( temp );
#else
		signal( SIGPIPE, SIG_IGN ); // This appears when we try to write to a broken network connection
		signal( SIGTERM, &endmessage );
		signal( SIGQUIT, &endmessage );
		signal( SIGINT, &endmessage ); 
		signal( SIGILL, &illinst );
		signal( SIGFPE, &aus );
		
#endif
		
		Console.PrintSectionBegin();
		Console << "UOX Server start up!" << myendl << "Welcome to " << CVC.GetProductName() << " v" << CVC.GetVersion() << "(" << CVC.GetBuild() << ")" << myendl;
		Console.PrintSectionBegin();

		LoadJSEngine();

		if(( cwmWorldState = new CWorldMain ) == NULL ) 
			Shutdown( FATAL_UOX3_ALLOC_WORLDSTATE );
		FileIO->LoadINIFile();
		InitClasses();
		cwmWorldState->SetUICurrentTime( currentTime );

		ParseArgs( argc, argv );
		ResetVars();
		
		Console << "Loading skill advancement      ";
		FileIO->LoadSkills();
		Console.PrintDone();
		
		cwmWorldState->SetKeepRun( Network->kr ); // for some technical reasons global varaibles CANT be changed in constructors in c++.
		cwmWorldState->SetError( Network->faul );  // i hope i can find a cleaner solution for that, but this works !!!
		// has to here and not at the cal cause it would get overriten later

		// Moved BulkStartup here, dunno why that function was there...
		Console << "Loading dictionaries...        " << myendl ;
		Console.PrintBasedOnVal( Dictionary->LoadDictionary() >= 0 );

		Console << "Loading teleport               ";
		FileIO->LoadTeleportLocations();
		Console.PrintDone();
		
		srand( cwmWorldState->GetUICurrentTime() ); // initial randomization call
		
		// moved all the map loading into cMapStuff - fur
		Map->Load();
		
		Skills->Load();
		
		Console << "Loading Spawn Regions          ";
		FileIO->LoadSpawnRegions();
		Console.PrintDone();

		Console << "Loading Regions                ";
		FileIO->LoadRegions();
		Console.PrintDone();

		Magic->LoadScript();
		
		Console << "Loading Races                  ";
		Races->load();
		Console.PrintDone();

		Console << "Loading Weather                ";
		Weather->Load();
		Weather->NewDay();
		Console.PrintDone();

		Console << "Loading Commands               ";
		Commands->Load();
		Console.PrintDone();
		
		// Rework that...
		Console << "Loading World now              ";
		FileIO->LoadNewWorld();

		StartupClearTrades();
		InitMultis();
		
		cwmWorldState->SetStartTime( cwmWorldState->GetUICurrentTime() );
		doGCollect();

		FD_ZERO( &conn );
		cwmWorldState->SetEndTime( 0 );
		cwmWorldState->SetLClock( 0 );

		// no longer Que, because that's taken care of by PageVector
		Console << "Initializing Jail system       ";	
		JailSys->ReadSetup();
		JailSys->ReadData();
		Console.PrintDone();
		
		Console << "Initializing Status system     ";	
		HTMLTemplates->Load();
		Console.PrintDone();

		Console << "Loading custom titles          ";
		FileIO->LoadCustomTitle();
		Console.PrintDone();

		Console << "Loading temporary Effects      ";
		Effects->LoadEffects();
		Console.PrintDone();
			

		
		if( cwmWorldState->ServerData()->GetServerAnnounceSavesStatus() )
			cwmWorldState->announce( 1 );
		else
			cwmWorldState->announce( 0 );

		FileIO->LoadCreatures();
		DisplayBanner();
		//DisplaySettings(); << Moved that to the configuration
		item_test();

		Console << "Loading Accounts               ";
		Accounts->Load();
		//Console.PrintDone(); 

		Console.Log( "-=Server Startup=-\n=======================================================================", "server.log" );
		cwmWorldState->SetUICurrentTime( getclock() );

		Console << myendl << "Initialize Console Thread      ";
#if defined(__unix__)
		int conthreadok = pthread_create(&cons,NULL,CheckConsoleKeyThread , NULL );
#else
		int conthreadok = _beginthread( CheckConsoleKeyThread , 0 , NULL );
#endif
#ifdef __LOGIN_THREAD__
 #if defined(__unix__)
		pthread_create(&netw,NULL, NetworkPollConnectionThread,  NULL );
 #else
		_beginthread( NetworkPollConnectionThread, 0, NULL );
 #endif //linux
#endif
		Console.PrintDone();

		Console.PrintSectionBegin();
		Console << "UOX: Startup Complete" << myendl;
		Console.PrintSectionBegin();
		
		// MAIN SYSTEM LOOP
		while( cwmWorldState->GetKeepRun() )
		{
			//	EviLDeD	-	February 27, 2000
			//	Just in case the thread doesn't start then use the main threaded copy
			if( conthreadok == -1 )
				checkkey();
			SleepNiceness();
			//	EviLDeD	-	End
			if(loopTimeCount >= 1000)       
			{
				loopTimeCount = 0;
				loopTime = 0;
			}
			loopTimeCount++;
			
			StartMilliTimer( loopSecs, loopMilli );
			
			if( networkTimeCount >= 1000 )
			{
				networkTimeCount = 0;
				networkTime = 0;
			}
			
			StartMilliTimer( tempSecs, tempMilli );
#ifndef __LOGIN_THREAD__
			if( uiNextCheckConn <= cwmWorldState->GetUICurrentTime() || cwmWorldState->GetOverflow() ) // Cut lag on CheckConn by not doing it EVERY loop.
			{
				Network->CheckConnections();
				uiNextCheckConn = BuildTimeValue( 1.0f );
			}
			Network->CheckMessages();
#else
			Network->CheckMessage();
//			Network->CheckXGM();
#endif
			
			tempTime = CheckMilliTimer( tempSecs, tempMilli );
			networkTime += tempTime;
			networkTimeCount++;
			
			if( timerTimeCount >= 1000 )
			{
				timerTimeCount = 0;
				timerTime = 0;
			}
			
			StartMilliTimer( tempSecs, tempMilli );
			
			cwmWorldState->CheckTimers();
			cwmWorldState->SetUICurrentTime( getclock() );
			tempTime = CheckMilliTimer( tempSecs, tempMilli );
			timerTime += tempTime;
			timerTimeCount++;
			
			if( autoTimeCount >= 1000 )
			{
				autoTimeCount = 0;
				autoTime = 0;
			}
			StartMilliTimer( tempSecs, tempMilli );
			
			checkauto();
			
			tempTime = CheckMilliTimer( tempSecs, tempMilli );
			autoTime  += tempTime;
			autoTimeCount++;
			StartMilliTimer( tempSecs, tempMilli );
			Network->ClearBuffers();
			tempTime = CheckMilliTimer( tempSecs, tempMilli );
			networkTime += tempTime;
			tempTime = CheckMilliTimer( loopSecs, loopMilli );
			loopTime += tempTime;
			DoMessageLoop();
		}

		
		Console.PrintSectionBegin();
		sysbroadcast( "The server is shutting down." );
		Console.PrintDone();
		Console << "Closing sockets...";
		netpollthreadclose = true;
		UOXSleep( 1000 );
		Network->SockClose();
		Console.PrintDone();

		if( !cwmWorldState->Saving() )
		{
			do 
			{
				cwmWorldState->savenewworld( true );
			} while( cwmWorldState->Saving() );
		}
		
		cwmWorldState->ServerData()->save();

		Console.Log( "Server Shutdown!\n=======================================================================\n" , "server.log" );

		conthreadcloseok = true;	//	This will signal the console thread to close
		Shutdown( 0 );

#ifdef _CRASH_PROTECT_
	} catch ( ... ) 
	{//Crappy error handling...
		Console << "Unknown exception caught, hard crash avioded!" << myendl;
		Shutdown( UNKNOWN_ERROR );
	}
#endif
	
	return( 0 );	
}

//o---------------------------------------------------------------------------o
//|            Function     - Restart()
//|            Date         - 1/7/00
//|            Programmer   - Zippy
//o---------------------------------------------------------------------------o
//|            Purpose      - Restarts the server, passes the server number of 
//|								Number of crashes so far, if < 10 then the
//|								Server will restart itself.
//o---------------------------------------------------------------------------o
void Restart( UI16 ErrorCode = UNKNOWN_ERROR )
{
	if( !ErrorCode )
		return;
	char temp[1024];
	if( cwmWorldState->ServerData()->GetServerCrashProtectionStatus() > 1 )
	{		
		if( cwmWorldState->GetErrorCount() < 10 )
		{
			cwmWorldState->IncErrorCount();
			
			sprintf( temp, "Server crash #%i from unknown error, restarting.", cwmWorldState->GetErrorCount() );
			Console.Log( temp, "server.log" );
			Console << temp << myendl;
			
			sprintf(temp, "uox.exe -ERROR %i", cwmWorldState->GetErrorCount() );
			
			if( cwmWorldState->GetXGMEnabled() )
				strcat( temp, " -xgm" );
			
			delete cwmWorldState;
			system( temp );
			exit(ErrorCode); // Restart successful Don't give them key presses or anything, just go out.
		} 
		else 
		{
			Console.Log( "10th Server crash, server shutting down.", "server.log" );
			Console << "10th Server crash, server shutting down" << myendl;
		}
	} 
	else 
		Console.Log( "Server crash!", "server.log" );
}

//o---------------------------------------------------------------------------o
//|            Function     - void Shutdown( int retCode )
//|            Date         - Oct. 09, 1999
//|            Programmer   - Krazyglue
//o---------------------------------------------------------------------------o
//|            Purpose      - Handled deleting / free() ing of pointers as neccessary
//|                                   as well as closing open file handles to avoid file
//|                                   file corruption.
//|                                   Exits with proper error code.
//o---------------------------------------------------------------------------o
void Shutdown( SI32 retCode )
{
	Console.PrintSectionBegin();
	Console << "Beginning UOX final shut down sequence..." << myendl;

	if (HTMLTemplates)
	{
		Console << "HTMLTemplates object detected. Writing Offline HTML Now..." << myendl;
	  HTMLTemplates->Poll( true /*ETT_OFFLINE */);
	}
	else
		Console << "HTMLTemplates object not found." << myendl;


	if( cwmWorldState->ServerData()->GetServerCrashProtectionStatus() >= 1 && retCode && cwmWorldState->GetLoaded() && cwmWorldState && !cwmWorldState->Saving() )
	{//they want us to save, there has been an error, we have loaded the world, and WorldState is a valid pointer.
		do
		{
			cwmWorldState->savenewworld( true );
		} while( cwmWorldState->Saving() );
	}
	
	Console << "Cleaning up item and character memory... ";
	items.Cleanup();
	chars.Cleanup();
	Console.PrintDone();

	Console << "Destroying class objects and pointers... ";
	// delete any objects that were created (delete takes care of NULL check =)
	delete DefBase;
	delete DefChar;
	delete DefItem;

	delete Boats;
	delete Combat;
	delete Commands;
	delete Gumps;
	delete Items;
	delete Map;
	delete Npcs;
	delete Skills;
	delete Weight;
	delete Targ;
	delete Magic;
	delete Races;
	delete Weather;
	delete Movement;
	delete Network;
	delete TEffects;
	delete WhoList;
	delete OffList;
	delete Books;
	delete GMQueue;
	delete HTMLTemplates;
	delete CounselorQueue;
	delete Dictionary;
	delete Accounts;
	if (Trigger)
	        Trigger->Cleanup();//must be called to delete some things the still reference Trigger.
	delete Trigger;
	delete MapRegion;
	delete SpeechSys;
	delete GuildSys;
	delete FileLookup;
	delete JailSys;
	delete Effects;
	delete FileIO;
	Console.PrintDone();

	//Lets wait for console thread to quit here
#if defined(__unix__)
	pthread_join( cons, NULL );
#ifdef __LOGIN_THREAD__
	pthread_join( newt, NULL );
#endif
#endif

	// don't leave file pointers open, could lead to file corruption
	if( loscache )			
		delete [] loscache;
	if( itemids )			
		delete [] itemids;
	Console << "Destroying JS instances... ";
	JS_DestroyContext( jsContext );
	JS_DestroyRuntime( jsRuntime );
	Console.PrintDone();

	Console.PrintSectionBegin();
	if( retCode && cwmWorldState->GetLoaded() )//do restart unless we have crashed with some error.
		Restart( (UI16)retCode );
	else
		delete cwmWorldState;

	Console.TurnGreen();
	Console << "Server shutdown complete!" << myendl;
	Console << "Thank you for supporting " << CVC.GetName() << myendl;
	Console.TurnNormal();
	Console.PrintSectionBegin();
	
	// dispay what error code we had
	// don't report errorlevel for no errors, this is confusing ppl - fur
	if( retCode )
	{
		Console.TurnRed();
		Console << "Exiting UOX with errorlevel " << retCode << myendl;
		Console.TurnNormal();
#if !defined(__unix__)
		Console << "Press Return to exit " << myendl;
		std::string throwAway;
		std::getline(std::cin, throwAway);
#endif
	}
	else
	{
		Console.TurnGreen();
		Console << "Exiting UOX with no errors..." << myendl;
		Console.TurnNormal();
	}
	
	Console.PrintSectionBegin();
	exit(retCode);
}

//o---------------------------------------------------------------------------o
//|	Function	-	void objTeleporters( CChar *s )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Called when a character walks into/on an object teleporter
//o---------------------------------------------------------------------------o
void objTeleporters( CChar *s )
{
	SI16 x = s->GetX(), y = s->GetY();
	
	int xOffset = MapRegion->GetGridX( s->GetX() );
	int yOffset = MapRegion->GetGridY( s->GetY() );
	UI08 worldNumber = s->WorldNumber();
	for( SI08 counter1 = -1; counter1 <= 1; counter1++ )
	{
		for( SI08 counter2 = -1; counter2 <= 1; counter2++ )
		{
			SubRegion *toCheck = MapRegion->GetGrid( xOffset + counter1, yOffset + counter2, worldNumber );
			if( toCheck == NULL )
				continue;
			toCheck->PushItem();
			for( CItem *itemCheck = toCheck->FirstItem(); !toCheck->FinishedItems(); itemCheck = toCheck->GetNextItem() )
			{
				if( itemCheck == NULL )
					continue;
				if( itemCheck->GetX() == x && itemCheck->GetY() == y && 
					( abs( itemCheck->GetZ() ) + 10 >= abs( s->GetZ() ) ) && 
					( abs( itemCheck->GetZ() ) - 10 <= abs( s->GetZ() ) ) )
				{
					switch( itemCheck->GetType() )
					{
					case 60:														// teleporters
						if( itemCheck->GetMoreX() + itemCheck->GetMoreY() + itemCheck->GetMoreZ() > 0 )
						{
							s->SetLocation( (UI16)itemCheck->GetMoreX(), (UI16)itemCheck->GetMoreY(), (SI08)itemCheck->GetMoreZ() );
							s->Teleport();
						}
						break;
					case 80:														// advancement gates
					case 81:
						if( !s->IsNpc() )
						{
							if( itemCheck->GetMore() != 0 )
							{
								if( s->GetSerial() == itemCheck->GetMore() )
									advanceObj( s, (UI16)itemCheck->GetMoreX(), ( itemCheck->GetType() == 81 ) );
							}
							else
								advanceObj( s, (UI16)itemCheck->GetMoreX(), ( itemCheck->GetType() == 81 ) );
						}
						break;
					case 82:	MonsterGate( s, itemCheck->GetMoreX() );	break;	// monster gates
					case 83:														// race gates
						Races->gate( s, static_cast<RACEID>(itemCheck->GetMoreX()), itemCheck->GetMoreY() != 0 );
						break;
					case 85:														// damage objects
						if( !s->IsInvulnerable() )
						{
							s->SetHP( (SI16)( s->GetHP() - ( itemCheck->GetMoreX() + RandomNum( itemCheck->GetMoreY(), itemCheck->GetMoreZ() ) ) ) );
							if( s->GetHP() < 1 ) 
								s->SetHP( 0 );
							updateStats( s, 0 );
							if( s->GetHP() <= 0 ) 
								doDeathStuff( s );
						}
						break;
					case 86:														// sound objects
						if( (UI32)RandomNum(1,100) <= itemCheck->GetMoreZ() )
							Effects->PlaySound( itemCheck, (UI16)( (itemCheck->GetMoreX()<<8) + itemCheck->GetMoreY() ) );
						break;

					case 89:
						SocketMapChange( calcSocketObjFromChar( s ), s, itemCheck );
						break;
					case 111:	s->SetKills( 0 );		break;			// zero kill gate
					case 234:	
						if( itemCheck->GetType() == 234 && !s->IsNpc() )				// world change gate
							SendWorldChange( (WorldType)itemCheck->GetMoreX(), calcSocketObjFromChar( s ) );
						break;
					}
				}
			}
			toCheck->PopItem();
		}
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	bool checkBoundingBox( SI16 xPos, SI16 yPos, SI16 fx1, SI16 fy1, SI08 fz1, SI16 fx2, SI16 fy2 )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Check bounding box
//o---------------------------------------------------------------------------o
bool checkBoundingBox( SI16 xPos, SI16 yPos, SI16 fx1, SI16 fy1, SI08 fz1, SI16 fx2, SI16 fy2, UI08 worldNumber )
{
	if( xPos >= ( ( fx1 < fx2 ) ? fx1 : fx2 ) && xPos <= ( ( fx1 < fx2 ) ? fx2 : fx1 ) )
	{
		if( yPos >= ( ( fy1 < fy2 ) ? fy1 : fy2 ) && yPos <= ( ( fy1 < fy2 ) ? fy2 : fy1 ) )
		{
			if( fz1 == -1 || abs( fz1 - Map->Height( xPos, yPos, fz1, worldNumber ) ) <= 5 )
				return true;
		}
	}
	return false;
}

//o---------------------------------------------------------------------------o
//|	Function	-	bool checkBoundingCircle( SI16 xPos, SI16 yPos, SI16 fx1, SI16 fy1, SI08 fz1, int radius )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Check bounding circle
//o---------------------------------------------------------------------------o
bool checkBoundingCircle( SI16 xPos, SI16 yPos, SI16 fx1, SI16 fy1, SI08 fz1, int radius, UI08 worldNumber )
{
	if( ( xPos - fx1 ) * ( xPos - fx1 ) + ( yPos - fy1 ) * ( yPos - fy1 ) <= radius * radius )
	{
		if( fz1 == -1 || abs( fz1 - Map->Height( xPos, yPos, fz1, worldNumber ) ) <= 5 )
			return true;
	}
	return false;
}

//o---------------------------------------------------------------------------o
//|	Function	-	UI32 getclock( void )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Return CPU time used, Emulates clock()
//o---------------------------------------------------------------------------o
#if defined(__unix__)
UI32 getclock( void )
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	// We want to keep the value within 32 bits; we could also substract
	// startup time I suppose
	return ((tv.tv_sec - 900000000) * CLOCKS_PER_SEC) +
		tv.tv_usec / (1000000 / CLOCKS_PER_SEC);
}
#endif

//o---------------------------------------------------------------------------o
//|	Function	-	void doLight( cSocket *s, char level )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Send light level to players client
//o---------------------------------------------------------------------------o
void doLight( cSocket *s, char level )
{
	if( s == NULL )
		return;

	CChar *mChar = s->CurrcharObj();
	CPLightLevel toSend( level );

	if( mChar->GetFixedLight() != 255 )
	{
		toSend.Level( 0 );
		s->Send( &toSend );
		return;
	}

	UI08 curRegion = mChar->GetRegion();
	CWeather *wSys =  Weather->Weather( region[curRegion]->GetWeather() );
	LIGHTLEVEL toShow;

	SI16 dunLevel = cwmWorldState->ServerData()->GetDungeonLightLevel();
	// we have a valid weather system
	if( wSys != NULL )
	{
		R32 lightMin = wSys->LightMin();
		R32 lightMax = wSys->LightMax();
		if( lightMin < 300 && lightMax < 300 )
		{
			R32 i = wSys->CurrentLight();
			if( Races->VisLevel( mChar->GetRace() ) > i )
				toShow = 0;
			else
				toShow = static_cast<LIGHTLEVEL>(i - Races->VisLevel( mChar->GetRace() ));
			toSend.Level( level );
		}
		else
			toSend.Level( level );
	}
	else
	{
		if( mChar->inDungeon() )
		{
			if( Races->VisLevel( mChar->GetRace() ) > dunLevel )
				toShow = 0;
			else
				toShow = static_cast<LIGHTLEVEL>(dunLevel - Races->VisLevel( mChar->GetRace() ));
			toSend.Level( toShow );
		}
	}
	s->Send( &toSend );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void telltime( cSocket *s )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Get in-game time
//o---------------------------------------------------------------------------o
void telltime( cSocket *s )
{
	char tstring[100];
	char tstring2[100];
	UI08 hour			= cwmWorldState->ServerData()->GetServerTimeHours();
	UI08 minute			= cwmWorldState->ServerData()->GetServerTimeMinutes();
	bool ampm			= cwmWorldState->ServerData()->GetServerTimeAMPM();
	UnicodeTypes sLang	= s->Language();

	int lhour = hour;
	
	if( minute <= 14 ) 
		strcpy( tstring, Dictionary->GetEntry( 1248, sLang ) );
	else if( minute >= 15 && minute <= 30 ) 
		strcpy( tstring, Dictionary->GetEntry( 1249, sLang ) );
	else if( minute >= 30 && minute <= 45 ) 
		strcpy( tstring, Dictionary->GetEntry( 1250, sLang ) );
	else
	{
		strcpy( tstring, Dictionary->GetEntry( 1251, sLang ) );
		lhour++;
		if( lhour == 12 ) 
			lhour = 0;
	}
	if( lhour >= 1 && lhour <= 11 )
		sprintf( tstring2, Dictionary->GetEntry( 1252, sLang ), tstring );
	else if( lhour == 1 && ampm )
		sprintf( tstring2, Dictionary->GetEntry( 1263, sLang ), tstring );
	else
		sprintf( tstring2, Dictionary->GetEntry( 1264, sLang ), tstring );
	
	if( lhour == 0 ) 
		strcpy( tstring, tstring2 );
	else if( ampm )
	{
		if( lhour >= 1 && lhour < 6 ) 
		{
			sysmessage( s, 1265, tstring2 );
			return;
		}
		else if( lhour >= 6 && lhour < 9 ) 
		{
			sysmessage( s, 1266, tstring2 );
			return;
		}
		else 
		{
			sysmessage( s, 1267, tstring2 );
			return;
		}
	}
	else
	{
		if( lhour >= 1  && lhour < 5 ) 
		{
			sysmessage( s, 1268, tstring2 );
			return;
		}
		else 
		{
			sysmessage( s, 1269, tstring2 );
			return;
		}
	}
	
	sysmessage( s, tstring );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void updateskill( cSocket *mSock, UI08 skillnum )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Update a certain skill
//o---------------------------------------------------------------------------o
void updateskill( cSocket *mSock, UI08 skillnum )
{
	CChar *mChar = mSock->CurrcharObj();
	CPUpdIndSkill toSend( (*mChar), (UI08)skillnum );
	mSock->Send( &toSend );
}

//o---------------------------------------------------------------------------o
//|	Function	-	UI08 getCharDir( CChar *a, SI16 x, SI16 y )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Get characters direction
//o---------------------------------------------------------------------------o
UI08 getCharDir( CChar *a, SI16 x, SI16 y )
{
	SI16 xdif = (SI16)( x - a->GetX() );
	SI16 ydif = (SI16)( y - a->GetY() );
	
	if( xdif == 0 && ydif < 0 ) 
		return NORTH;
	else if( xdif > 0 && ydif < 0 ) 
		return NORTHEAST;
	else if( xdif > 0 && ydif ==0 ) 
		return EAST;
	else if( xdif > 0 && ydif > 0 ) 
		return SOUTHEAST;
	else if( xdif ==0 && ydif > 0 ) 
		return SOUTH;
	else if( xdif < 0 && ydif > 0 ) 
		return SOUTHWEST;
	else if( xdif < 0 && ydif ==0 ) 
		return WEST;
	else if( xdif < 0 && ydif < 0 ) 
		return NORTHWEST;
	else 
		return UNKNOWNDIR;
}

//o---------------------------------------------------------------------------o
//|	Function	-	UI08 getFieldDir( CChar *s, SI16 x, SI16 y )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Get field direction
//o---------------------------------------------------------------------------o
UI08 getFieldDir( CChar *s, SI16 x, SI16 y )
{
	UI08 fieldDir = 0;
	switch( getCharDir( s, x, y ) )
	{
	case NORTH:
	case SOUTH:
		break;
	case EAST:
	case WEST:
		fieldDir = 1;
		break;
	case NORTHEAST:
	case SOUTHEAST:
	case SOUTHWEST:
	case NORTHWEST:
	case UNKNOWNDIR:
		switch( s->GetDir() )
		{
		case NORTH:
		case SOUTH:
			break;
		case NORTHEAST:
		case EAST:
		case SOUTHEAST:
		case SOUTHWEST:
		case WEST:
		case NORTHWEST:
			fieldDir = 1;
			break;
		default:
			Console.Error( 2, " Fallout of switch statement without default. uox3.cpp, getFieldDir()" );
			break;
		}
		break;
	default:
		Console.Error( 2, " Fallout of switch statement without default. uox3.cpp, getFieldDir()" );
		break;
	}
	return fieldDir;
}

//o---------------------------------------------------------------------------o
//|	Function	-	void openBank( cSocket *s, CChar *i )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Opens players bank box
//o---------------------------------------------------------------------------o
void openBank( cSocket *s, CChar *i )
{
	char temp[1024];
	ITEMLIST *ownedItems = i->GetOwnedItems();

	for( UI32 ci = 0; ci < ownedItems->size(); ci++ )
	{
		CItem *oItem = (*ownedItems)[ci];
		if( oItem != NULL )
		{
			if( oItem->GetType() == 1 && oItem->GetMoreX() == 1 )
			{
				if( cwmWorldState->ServerData()->GetWildernessBankStatus() )
				{
					if( oItem->GetMoreY() == 0 && oItem->GetMoreZ() == 0 ) // if not initialized yet for the special bank
						oItem->SetMoreY( 123 ); // convert to new special bank
				}
				if( cwmWorldState->ServerData()->GetWildernessBankStatus() )
				{
					if( oItem->GetMoreY() == 123 ) // check if a goldbank
					{
						CPWornItem toWear = (*oItem);
						s->Send( &toWear );
						openPack( s, oItem );
						return;
					}
				} 
				else// else if not using specialbank
				{ // don't check for goldbank
					CPWornItem toWearO = (*oItem);
					s->Send( &toWearO );
					openPack(s, oItem );
					return;
				}
				
			}
		}
	}
	
	sprintf( temp, Dictionary->GetEntry( 1283 ), i->GetName() );
	CItem *c = Items->SpawnItem( NULL, i, 1, temp, false, 0x09AB, 0, false, false );
	
	c->SetLayer( 0x1D );
	c->SetOwner( (cBaseObject *)i );
	if( !c->SetCont( i ) )
		return;
	c->SetMoreX( 1 );
	if( cwmWorldState->ServerData()->GetWildernessBankStatus() ) // Special Bank
		c->SetMoreY( 123 ); // gold only bank
	c->SetType( 1 );
	CPWornItem toWear = (*c);
	s->Send( &toWear );
	openPack( s, c );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void openSpecialBank( cSocket *s, CChar *i )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Opens players special bank box (Can only hold items on a
//|					town by town basis)
//o---------------------------------------------------------------------------o
void openSpecialBank( cSocket *s, CChar *i )
{
	CChar *mChar = s->CurrcharObj();
	ITEMLIST *ownedItems = i->GetOwnedItems();
	for( UI32 ci = 0; ci < ownedItems->size(); ci++ )
	{
		CItem *oItem = (*ownedItems)[ci];
		if( oItem != NULL )
		{
			if( oItem->GetType() == 1 && oItem->GetMoreX() == 1 && oItem->GetMoreY() != 123 ) // specialbank and the current region
			{
				if( oItem->GetMoreZ() == 0 ) // convert old banks into new banks
					oItem->SetMoreZ( mChar->GetRegion() );
				if( oItem->GetMoreZ() == mChar->GetRegion() )
				{
					CPWornItem toWear = (*oItem);
					s->Send( &toWear );
					openPack( s, oItem );
					return;
				}
			}
		}
	}

	char temp[1024];
	sprintf( temp, Dictionary->GetEntry( 1284 ), i->GetName() );
	CItem *c = Items->SpawnItem( NULL, i, 1, temp, false, 0x09AB, 0, false, false );
	
	c->SetLayer( 0x1D );
	c->SetOwner( (cBaseObject *)i );
	if( !c->SetCont( i ) )
		return;
	c->SetMoreX( 1 );
	c->SetMoreY( 0 ); // this's an all-items bank
	c->SetMoreZ( mChar->GetRegion() ); // let's store the region
	c->SetType( 1 );
	
	CPWornItem toWear = (*c);
	s->Send( &toWear );
	openPack( s, c );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void gettokennum( char *s, int num )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Unknown
//o---------------------------------------------------------------------------o
void gettokennum( const char * s, int num, char *gettokenstr )
{
	memset( gettokenstr, 0, 255 );
	
	int i = 0;
	while( num != 0 )
	{
		if( s[i] == 0 )
			num--;
		else
		{
			if( s[i] == ' ' && i != 0 && s[i-1] != ' ' )
				num--;
			i++;
		}
	}
	int j = 0;
	while( num != -1 )
	{
		if( s[i] == 0 )
			num--;
		else
		{
			if( s[i] == ' ' && i != 0 && s[i-1] != ' ')
				num--;
			else
			{
				gettokenstr[j] = s[i];
				j++;
			}
			i++;
		}
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	CItem *getRootPack( CItem *item )
//|	Programmer	-	UOX3 DevTeam
//o---------------------------------------------------------------------------o
//|	Purpose		-	Gets the root container an item is in (if any)
//o---------------------------------------------------------------------------o
CItem *getRootPack( CItem *item )
{
	if( item == NULL || item->GetCont() == NULL || item->GetCont( 1 ) < 0x40 )	// Item has no containing item
		return NULL;

	while( item != NULL )
	{
		if( item->GetCont() == NULL || item->GetCont( 1 ) < 0x40 )		// Item is on the ground or on a character
			break;
		item = (CItem *)item->GetCont();
	}
	return item;
}

//o---------------------------------------------------------------------------o
//|	Function	-	CChar *getPackOwner( CItem *p )
//|	Programmer	-	UOX3 DevTeam
//o---------------------------------------------------------------------------o
//|	Purpose		-	Returns a containers owner
//o---------------------------------------------------------------------------o
CChar *getPackOwner( CItem *p )
{
	if( p == NULL || p->GetCont() == NULL )
		return NULL;

	if( p->GetCont( 1 ) < 0x40 )								// Items container is a character, return it
		return (CChar *)p->GetCont();

	CItem *rootPack = getRootPack( p );							// Find the root container for the item
	if( rootPack != NULL && rootPack->GetCont( 1 ) < 0x40 )		// Ensure the root packs container is a character
		return (CChar *)rootPack->GetCont();					// Return the root packs owner
	return NULL;
}

//o---------------------------------------------------------------------------o
//|	Function	-	int getTileName( CItem *i, char* itemname )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Returns the lenght of an items name from tiledata.mul and
//|					sets itemname to the name
//o---------------------------------------------------------------------------o
int getTileName( CItem *i, char *itemname )
{
	if( i->GetName()[0] != '#' )
	{
		strcpy( itemname, i->GetName() );
		return strlen(itemname)+1;
	}
	CTile tile;
	Map->SeekTile( i->GetID(), &tile);
	if( tile.DisplayAsAn() ) 
		strcpy( itemname, "an " );
	else if( tile.DisplayAsA() ) 
		strcpy( itemname, "a " );
	else 
		itemname[0] = 0;

	int namLen = strlen( itemname );
	const char *tName = tile.Name();
	UI08 mode = 0;
	for( UI32 j = 0; j < strlen( tName ); j++ )
	{
		if( tName[j] == '%' )
		{
			if( mode )
				mode = 0;
			else
				mode = 2;
		}
		else if( tName[j] == '/' && mode == 2 )
			mode = 1;
		else if( mode == 0 || ( mode == 1 && i->GetAmount() == 1 ) || ( mode == 2 && i->GetAmount() > 1 ) )
		{
			itemname[namLen++] = tName[j];
			itemname[namLen  ] = '\0';
		}
	}
	return strlen(itemname)+1;
}

//o---------------------------------------------------------------------------o
//|	Function	-	void usePotion( CChar *p, CItem *i )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Character uses a potion
//o---------------------------------------------------------------------------o
void usePotion( CChar *p, CItem *i )
{
	if( p == NULL || i == NULL )
		return;
	int x;

	cSocket *mSock = calcSocketObjFromChar( p );
	if( cwmWorldState->ServerData()->GetPotionDelay() != 0 )
		Effects->tempeffect( p, p, 26, 0, 0, 0 );
	switch( i->GetMoreY() )
	{
	case 1: // Agility Potion
		Effects->staticeffect( p, 0x373A, 0, 15 );
		switch( i->GetMoreZ() )
		{
		case 1:
			Effects->tempeffect( p, p, 6, (UI16)RandomNum( 6, 15 ), 0, 0 );
			sysmessage( mSock, 1608 );
			break;
		case 2:
			Effects->tempeffect( p, p, 6, (UI16)RandomNum( 11, 30 ), 0, 0 );
			sysmessage( mSock, 1609 );
			break;
		default:
			Console.Error( 2, " Fallout of switch statement without default. uox3.cpp, usepotion()" );
			return;
		}
		Effects->PlaySound( p, 0x01E7 );
		if( mSock != NULL ) 
			updateStats( p, 2 );
		break;
	case 2: // Cure Potion
		if( p->GetPoisoned() < 1 ) 
			sysmessage( mSock, 1344 );
		else
		{
			switch( i->GetMoreZ() )
			{
			case 1:
				x = RandomNum( 1, 100 );
				if( p->GetPoisoned() == 1 && x < 81 ) 
					p->SetPoisoned( 0 );
				else if( p->GetPoisoned() == 2 && x < 41 ) 
					p->SetPoisoned( 0 );
				else if( p->GetPoisoned() == 3 && x < 21 ) 
					p->SetPoisoned( 0 );
				else if( p->GetPoisoned() == 4 && x < 6 )  
					p->SetPoisoned( 0 );
				break;
			case 2:
				x = RandomNum( 1, 100 );
				if( p->GetPoisoned() == 1 ) 
					p->SetPoisoned( 0 );
				else if( p->GetPoisoned() == 2 && x < 81 ) 
					p->SetPoisoned( 0 );
				else if( p->GetPoisoned() == 3 && x < 41 ) 
					p->SetPoisoned( 0 );
				else if( p->GetPoisoned() == 4 && x < 21 )  
					p->SetPoisoned( 0 );
				break;
			case 3:
				x = RandomNum( 1, 100 );
				if( p->GetPoisoned() == 1 ) 
					p->SetPoisoned( 0 );
				else if( p->GetPoisoned() == 2 ) 
					p->SetPoisoned( 0 );
				else if( p->GetPoisoned() == 3 && x < 81 ) 
					p->SetPoisoned( 0 );
				else if( p->GetPoisoned() == 4 && x < 61 ) 
					p->SetPoisoned( 0 );
				break;
			default:
				Console.Error( 2, " Fallout of switch statement without default. uox3.cpp, usepotion()" );
				return;
			}
			
			if( p->GetPoisoned() ) 
				sysmessage( mSock, 1345 ); 
			else
			{
				Effects->staticeffect( p, 0x373A, 0, 15 );
				Effects->PlaySound( p, 0x01E0 ); 
				sysmessage( mSock, 1346 );
			} 
		}
		p->SendToSocket( mSock, true, p );
		break;
	case 3: // Explosion Potion
		if( region[p->GetRegion()]->IsGuarded() )
		{
			sysmessage( mSock, 1347 );
			return;
		}
		mSock->AddID( i->GetSerial() );
		sysmessage( mSock, 1348 );
		Effects->tempeffect( p, p, 16, 0, 1, 3 );
		Effects->tempeffect( p, p, 16, 0, 2, 2 );
		Effects->tempeffect( p, p, 16, 0, 3, 1 );
		Effects->tempeffect( p, i, 17, 0, 4, 0 );
		target( mSock, 0, 207, "" );
		return;
	case 4: // Heal Potion
		switch( i->GetMoreZ() )
		{
		case 1:
			p->SetHP( min( (SI16)(p->GetHP() + 5 + RandomNum( 1, 5 ) + p->GetSkill( 17 ) / 100 ), p->GetMaxHP() ) );
			sysmessage( mSock, 1349 );
			break;
		case 2:
			p->SetHP( min( (SI16)(p->GetHP() + 15 + RandomNum( 1, 10 ) + p->GetSkill( 17 ) / 50 ), p->GetMaxHP() ) );
			sysmessage( mSock, 1350 );
			break;
		case 3:
			p->SetHP( min( (SI16)(p->GetHP() + 20 + RandomNum( 1, 20 ) + p->GetSkill( 17 ) / 40 ), p->GetMaxHP() ) );
			sysmessage( mSock, 1351 );
			break;
		default:
			Console.Error( 2, " Fallout of switch statement without default. uox3.cpp, usepotion()" );
			return;
		}
		if( mSock != NULL ) 
			updateStats( p, 0 );
		Effects->staticeffect( p, 0x376A, 0x09, 0x06); // Sparkle effect
		Effects->PlaySound( p, 0x01F2 ); //Healing Sound - SpaceDog
		break;
	case 5: // Night Sight Potion
		//{
		Effects->staticeffect( p, 0x376A, 0x09, 0x06 );
		Effects->tempeffect( p, p, 2, 0, 0, 0 );
		Effects->PlaySound( p, 0x01E3 );
		break;
		//}
	case 6: // Poison Potion
		if( p->GetPoisoned() < (SI08)i->GetMoreZ() ) 
			p->SetPoisoned( (SI08)i->GetMoreZ() );
		if( i->GetMoreZ() > 4 ) 
			i->SetMoreZ( 4 );
		p->SetPoisonWearOffTime( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetSystemTimerStatus( POISON ) ) );
		p->SendToSocket( mSock, true, p );
		Effects->PlaySound( p, 0x0246 );
		sysmessage( mSock, 1352 );
		break;
	case 7: // Refresh Potion
		switch( i->GetMoreZ() )
		{
		case 1:
			p->SetStamina( min( (SI16)(p->GetStamina() + 20 + RandomNum( 1, 10 )), p->GetMaxStam() ) );
			sysmessage( mSock, 1353 );
			break;
		case 2:
			p->SetStamina( min( (SI16)(p->GetStamina() + 40 + RandomNum( 1, 30 )), p->GetMaxStam() ) );
			sysmessage( mSock, 1354 );
			break;
		default:
			Console.Error( 2, " Fallout of switch statement without default. uox3.cpp, usepotion()" );
			return;
		}
		if( mSock != NULL ) 
			updateStats( p, 2 );
		Effects->staticeffect( p, 0x376A, 0x09, 0x06); // Sparkle effect
		Effects->PlaySound( p, 0x01F2 ); //Healing Sound
		break;
	case 8: // Strength Potion
		Effects->staticeffect( p, 0x373A, 0, 15 );
		switch( i->GetMoreZ() )
		{
		case 1:
			Effects->tempeffect( p, p, 8, (UI16)( 5 + RandomNum( 1, 10 ) ), 0, 0);
			sysmessage( mSock, 1355 );
			break;
		case 2:
			Effects->tempeffect( p, p, 8, (UI16)( 10 + RandomNum( 1, 20 ) ), 0, 0);
			sysmessage( mSock, 1356 );
			break;
		default:
			Console.Error( 2, " Fallout of switch statement without default. uox3.cpp, usepotion()" );
			return;
		}
		Effects->PlaySound( p, 0x01EE );     
		break;
	case 9: // Mana Potion
		switch( i->GetMoreZ() )
		{
		case 1:
			p->SetMana( min( (SI16)(p->GetMana() + 10 + i->GetMoreX()/100), p->GetMaxMana() ) );
			break;
		case 2:
			p->SetMana( min( (SI16)(p->GetMana() + 20 + i->GetMoreX()/50), p->GetMaxMana() ) );
			break;
		default:
			Console.Error( 2, " Fallout of switch statement without default. uox3.cpp, usepotion()" );
			return;
		}
		if( mSock != NULL ) 
			updateStats( p, 1 );
		Effects->staticeffect( p, 0x376A, 0x09, 0x06); // Sparkle effect
		Effects->PlaySound( p, 0x01E7); //agility sound - SpaceDog
		break;
	default:
		Console.Error( 2, " Fallout of switch statement without default. uox3.cpp, usepotion()" );
		return;
	}
	Effects->PlaySound( p, 0x0030 );
	if( p->GetID( 1 ) >= 1 && p->GetID( 2 )>90 && !p->IsOnHorse() ) 
		npcAction( p, 0x22);
	DecreaseItemAmount( i );
	CItem *bPotion = Items->SpawnItem( NULL, p, 1, "#", true, 0x0F0E, 0, true, false );
	if( bPotion != NULL )
	{
		if( bPotion->GetCont() == NULL )
			bPotion->SetLocation( p );
		bPotion->SetDecayable( true );
		RefreshItem( bPotion );
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	void checkRegion( CChar *i )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Check what region a character is in
//o---------------------------------------------------------------------------o
void checkRegion( CChar *i )
{
	if( i == NULL )
		return;
	int j;
	UI08 calcReg = calcRegionFromXY( i->GetX(), i->GetY(), i->WorldNumber() );
	if( calcReg != i->GetRegion() )
	{
		UI08 iRegion = i->GetRegion();
		cSocket *s = calcSocketObjFromChar( i );
		if( s != NULL )
		{
			if( region[iRegion] != NULL && region[calcReg] != NULL )
			{
				// Drake 08-30-99 If region name are the same, do not display message
				//                for playing music when approaching Tavern
				j = strcmp( region[iRegion]->GetName(), region[calcReg]->GetName() );
				if( j )
				{
					const char *iRegionName = region[iRegion]->GetName();
					//Changed this and four others to compare the first character of the string to '\0' rather than
					//to NULL.
					//-Shadowlord Nov-3-2001
					if( iRegionName != NULL && iRegionName[0] != '\0')
						sysmessage( s, 1358, iRegionName );

					const char *calcRegName = region[calcReg]->GetName();
					if( calcRegName != NULL && calcRegName[0] != '\0' )
						sysmessage( s, 1359, calcRegName );
				}
				j = strcmp( region[calcReg]->GetOwner(), region[iRegion]->GetOwner() );
				if( ( region[calcReg]->IsGuarded() && j ) || ( !( region[calcReg]->IsGuarded() && region[iRegion]->IsGuarded() ) ) )
				{
					if( region[calcReg]->IsGuarded() )
					{
						const char *calcRegOwner = region[calcReg]->GetOwner();
						if( calcRegOwner != NULL )
						{
							if( calcRegOwner[0] == '\0' )
								sysmessage( s, 1360 );
							else
								sysmessage( s, 1361, calcRegOwner );
						}
					} 
					else
					{
						const char *iRegionOwner = region[iRegion]->GetOwner();
						if( iRegionOwner != NULL )
						{
							if( iRegionOwner[0] == '\0' )
								sysmessage( s, 1362 );
							else
								sysmessage( s, 1363, iRegionOwner );
						}
					}
				}
				if( region[calcReg]->GetAppearance() != region[iRegion]->GetAppearance() )	 // if the regions look different
					SendWorldChange( (WorldType)region[calcReg]->GetAppearance(), s );
				if( calcReg == i->GetTown() )	// enter our home town
				{
					sysmessage( s, 1364 );
					CItem *packItem = getPack( i );
					if( packItem != NULL )
					{
						for( CItem *toScan = packItem->FirstItemObj(); !packItem->FinishedItems(); toScan = packItem->NextItemObj() )
						{
							if( toScan != NULL )
							{
								if( toScan->GetType() == 35 )
								{
									UI08 targRegion = (UI08)toScan->GetMoreX();
									sysmessage( s, 1365, region[targRegion]->GetName() );
									region[targRegion]->DoDamage( region[targRegion]->GetHealth() );	// finish it off
									region[targRegion]->Possess( calcReg );
									i->SetFame( (SI16)( i->GetFame() + i->GetFame() / 5 ) );	// 20% fame boost
									break;
								}
							}
						}
					}
				}
			}
		}
		UI16 leaveScript = i->GetScriptTrigger();
		cScript *tScript = Trigger->GetScript( leaveScript );
		if( tScript != NULL )
		{
			tScript->OnLeaveRegion( i, i->GetRegion() );
			tScript->OnEnterRegion( i, calcReg );
		}
		i->SetRegion( calcReg );
		if( s != NULL ) 
		{
			Effects->dosocketmidi( s );
			doLight( s, (SI08)cwmWorldState->ServerData()->GetWorldLightCurrentLevel() );	// force it to update the light level straight away
			Weather->DoPlayerStuff( i );	// force a weather update too
		}
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	int getStringValue( char *string )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Pulls tokens from a string, if one value returns it, if two
//|					values it finds a random number between the values
//o---------------------------------------------------------------------------o
int getStringValue( const char *string ) 
{
	char temp[256], temp2[256];
	gettokennum( string, 0, temp );
	SI32 lovalue = makeNum( temp );
	gettokennum( string, 1, temp2 );
	SI32 hivalue = makeNum( temp2 );
	
	if( hivalue ) 
		return RandomNum( lovalue, hivalue );
	else 
		return lovalue;
}

//o---------------------------------------------------------------------------o
//|	Function	-	void advanceObj( CChar *s, UI16 x, bool multiUse )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Handle advancement objects (stat / skill gates)
//o---------------------------------------------------------------------------o
void advanceObj( CChar *s, UI16 x, bool multiUse )
{
	char sect[512];
	int val = 0, num;

	CItem *i = NULL;
	CItem *retitem = NULL;
	if( s->GetAdvObj() == 0 || multiUse )
	{
		Effects->staticeffect( s, 0x373A, 0, 15);
		Effects->PlaySound( s, 0x01E9 );
		s->SetAdvObj( x );
		sprintf( sect, "ADVANCEMENT %i", x );
		ScriptSection *Advancement = FileLookup->FindEntry( sect, advance_def );
		if( Advancement == NULL )
		{
			Console << "ADVANCEMENT OBJECT: Script section not found, Aborting" << myendl;
			s->SetAdvObj( 0 );
			return;
		}
		else
		{
			const char *tag = NULL;
			const char *data = NULL;
			for( tag = Advancement->First(); !Advancement->AtEnd(); tag = Advancement->Next() )
			{
				data = Advancement->GrabData();
				switch( tag[0] )
				{
				case 'a':
				case 'A':
					if( !strcmp( "ALCHEMY", tag ) ) 
						val = ALCHEMY;
					else if( !strcmp( "ANATOMY", tag ) ) 
						val = ANATOMY;
					else if( !strcmp( "ANIMALLORE", tag ) ) 
						val = ANIMALLORE;
					else if( !strcmp( "ARMSLORE", tag ) ) 
						val = ARMSLORE;
					else if( !strcmp( "ARCHERY", tag ) ) 
						val = ARCHERY;
					else if( !strcmp( "ADVOBJ", tag ) )
						s->SetAdvObj( static_cast<UI16>(makeNum( data )) );
					break;
					
				case 'b':
				case 'B':
					if( !strcmp( "BEGGING", tag ) ) 
						val = BEGGING;
					else if( !strcmp( "BLACKSMITHING", tag ) ) 
						val = BLACKSMITHING;
					else if( !strcmp( "BOWCRAFT", tag ) ) 
						val = BOWCRAFT;
					break;
					
				case 'c':
				case 'C':
					if( !strcmp( "CAMPING", tag ) ) 
						val = CAMPING;
					else if( !strcmp( "CARPENTRY", tag ) ) 
						val = CARPENTRY;
					else if( !strcmp( "CARTOGRAPHY", tag ) ) 
						val = CARTOGRAPHY;
					else if( !strcmp( "COOKING", tag ) ) 
						val = COOKING;
					break;
					
				case 'd':
				case 'D':
					if( !strcmp( "DEX", tag ) || !strcmp( "DEXTERITY", tag ) )
						s->SetDexterity( (UI16)getStringValue( data ) );
					else if( !strcmp( "DETECTINGHIDDEN", tag ) ) 
						val = DETECTINGHIDDEN;
					else if( !strcmp( "DYEHAIR", tag ) ) 
					{
						CItem *hairobject = s->GetItemAtLayer( 0x0B );
						if( hairobject != NULL )
						{
							num = makeNum( data );
							hairobject->SetColour( (UI16)num );
							RefreshItem( hairobject );
						}
					}
					else if( !strcmp( "DYEBEARD", tag ) ) 
					{
						CItem *beardobject = s->GetItemAtLayer( 0x10 );
						if( beardobject != NULL )
						{
							num = makeNum( data );
							beardobject->SetColour( (UI16)num );
							RefreshItem( beardobject );
						}
					}
					break;
					
				case 'e':
				case 'E':
					if( !strcmp( "ENTICEMENT", tag ) ) 
						val = ENTICEMENT;
					else if( !strcmp( "EVALUATINGINTEL", tag ) ) 
						val = EVALUATINGINTEL;
					break;
					
				case 'f':
				case 'F':
					if( !strcmp( "FAME", tag ) ) 
						s->SetFame( (SI16)makeNum( data ) );
					else if( !strcmp( "FENCING", tag ) ) 
						val = FENCING;
					else if( !strcmp( "FISHING", tag ) ) 
						val = FISHING;
					else if( !strcmp( "FORENSICS", tag ) ) 
						val = FORENSICS;
					break;
					
				case 'h':
				case 'H':
					if( !strcmp( "HEALING", tag ) ) 
						val = HEALING;
					else if( !strcmp( "HERDING", tag ) ) 
						val = HERDING;
					else if( !strcmp( "HIDING", tag ) ) 
						val = HIDING;
					break;
					
				case 'i':
				case 'I':
					if( !strcmp( "INT", tag ) || !strcmp( "INTELLIGENCE", tag ) )
						s->SetIntelligence( (SI16)getStringValue( data ) );
					else if( !strcmp( "ITEMID", tag ) ) 
						val = ITEMID;
					else if( !strcmp( "INSCRIPTION", tag ) ) 
						val = INSCRIPTION;
					else if( !strcmp( "ITEM", tag ) )
					{
						retitem = Items->CreateScriptItem( NULL, data, false, s->WorldNumber() );
						CItem *packnum = getPack( s );
						if( retitem != NULL )
						{
							retitem->SetX( (UI16)( 50 + (RandomNum( 0, 79 )) ) );
							retitem->SetY( (UI16)( 50 + (RandomNum( 0, 79 )) ) ); 
							retitem->SetZ( 9 );
							if( retitem->GetLayer() == 0x0B || retitem->GetLayer() == 0x10 )
							{
								if( !retitem->SetCont( s ) )
									retitem = NULL;
							}
							else
								retitem->SetCont( packnum );
							if( retitem != NULL )
								RefreshItem( retitem );
						}
					}
					break;
					
				case 'k':
				case 'K':
					if( !strcmp( "KARMA", tag ) ) 
						s->SetKarma( (SI16)makeNum( data ) );
					else if( !strcmp( "KILLHAIR", tag ) )
					{
						i = s->GetItemAtLayer( 0x0B );
						if( i != NULL )
							Items->DeleItem( i );
					}
					else if( !strcmp( "KILLBEARD", tag ) )
					{
						i = s->GetItemAtLayer( 0x10 );
						if( i != NULL )
							Items->DeleItem( i );

					}
					else if( !strcmp( "KILLPACK", tag ) )
					{
						i = s->GetItemAtLayer( 0x15 );
						if( i != NULL )
							Items->DeleItem( i );

					}
					break;
					
				case 'l':
				case 'L':
					if( !strcmp( "LOCKPICKING", tag ) ) 
						val = LOCKPICKING;
					else if( !strcmp( "LUMBERJACKING", tag ) ) 
						val = LUMBERJACKING;
					break;
					
				case 'm':
				case 'M':
					if( !strcmp( "MAGERY", tag ) ) 
						val = MAGERY;
					else if( !strcmp( "MAGICRESISTANCE", tag ) ) 
						val = MAGICRESISTANCE;
					else if( !strcmp( "MACEFIGHTING", tag ) ) 
						val = MACEFIGHTING;
					else if( !strcmp( "MEDITATION", tag ) ) 
						val = MEDITATION;
					else if( !strcmp( "MINING", tag ) ) 
						val = MINING;
					else if( !strcmp( "MUSICIANSHIP", tag ) ) 
						val = MUSICIANSHIP;
					break;
					
				case 'p':
				case 'P':
					if( !strcmp( "PARRYING", tag ) ) 
						val = PARRYING;
					else if( !strcmp( "PEACEMAKING", tag ) ) 
						val = PEACEMAKING;
					else if( !strcmp( "POISONING", tag ) ) 
						val = POISONING;
					else if( !strcmp( "PROVOCATION", tag ) ) 
						val = PROVOCATION;
					else if( !strcmp( "POLY", tag ) )
					{
						num = makeNum( data );
						s->SetID( (UI16)num );
						s->SetxID( (UI16)num );
						s->SetOrgID( (UI16)num );
					}
					break;
				case 'r':
				case 'R':
					if( !strcmp( "REMOVETRAPS", tag ) ) 
						val = REMOVETRAPS;
					break;
					
				case 's':
				case 'S':
					if( !strcmp( "STR", tag ) || !strcmp( "STRENGTH", tag ) )
						s->SetStrength( (SI16)getStringValue( data ) );
					else if( !strncmp( "SKILL", tag, 5 ) )	// get number code
						val = makeNum( tag + 5 );
					else if( !strcmp( tag, "SKIN" ) )
					{
						num = makeNum( data );
						s->SetSkin( (UI16)num );
						s->SetxSkin( (UI16)num );
					}
					else if( !strcmp( "SNOOPING", tag ) ) 
						val = SNOOPING;
					else if( !strcmp( "SPIRITSPEAK", tag ) ) 
						val = SPIRITSPEAK;
					else if( !strcmp( "STEALING", tag ) ) 
						val = STEALING;
					else if( !strcmp( "STEALTH", tag ) ) 
						val = STEALTH;
					else if( !strcmp( "SWORDSMANSHIP", tag ) ) 
						val = SWORDSMANSHIP;
					break;
					
				case 't':
				case 'T':
					if( !strcmp( "TACTICS", tag ) ) 
						val = TACTICS;
					else if( !strcmp( "TAILORING", tag ) ) 
						val = TAILORING;
					else if( !strcmp( "TAMING", tag ) ) 
						val = TAMING;
					else if( !strcmp( "TASTEID", tag ) ) 
						val = TASTEID;
					else if( !strcmp( "TINKERING", tag ) ) 
						val = TINKERING;
					else if( !strcmp( "TRACKING", tag ) ) 
						val = TRACKING;
					break;
					
				case 'v':
				case 'V':
					if( !strcmp( "VETERINARY", tag ) ) 
						val = VETERINARY;
					break;

				case 'w':
				case 'W':
					if( !strcmp( "WRESTLING", tag ) ) 
						val = WRESTLING;
					break;
				}
				
				if( val > 0 )
				{
					s->SetBaseSkill( (UI16)getStringValue( data ), (UI08)val );
					val = 0;	// reset for next time through
				}
			}
			s->Teleport();	// don't need to teleport.  Why?? because our location never changes!
//			s->Update();
			
		}
	}
	else 
		sysmessage( calcSocketObjFromChar( s ), 1366 );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void DumpCreatures( void )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Dumps creatures to npc.dat
//o---------------------------------------------------------------------------o
void DumpCreatures( void )
{
	std::ofstream toWrite( "npc.dat" );
	if( !toWrite.is_open() )
		return;
	for( int i = 0; i < 2048; i++ )
	{
		toWrite << "[CREATURE " << i << "]" << std::endl << "{" << std::endl;
		toWrite << "BASESOUND=" << creatures[i].BaseSound() << std::endl;
		toWrite << "ICON=" << (int)creatures[i].Icon() << std::endl;
		toWrite << "SOUNDFLAG=" << (SI32)creatures[i].SoundFlag() << std::endl;
		if( creatures[i].CanFly() )
			toWrite << "FLIES" << std::endl;
		if( creatures[i].AntiBlink() )
			toWrite << "ANTIBLINK" << std::endl;
		if( creatures[i].IsAnimal() )
			toWrite << "ANIMAL" << std::endl;
		if( creatures[i].IsWater() )
			toWrite << "WATERCREATURE" << std::endl;
		toWrite << "}" << std::endl << std::endl;
	}
	toWrite.close();
}

//o---------------------------------------------------------------------------o
//|	Function	-	void enlist( cSocket *mSock, SI32 listnum )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Handle enlistment objects
//o---------------------------------------------------------------------------o
void enlist( cSocket *mSock, SI32 listnum )
{
	char sect[50];
	
	char realSect[50];
	sprintf( sect, "ITEMLIST %i", listnum );
	strcpy( realSect, sect );
	ScriptSection *Enlist = FileLookup->FindEntry( realSect, items_def );
	if( Enlist == NULL )
	{
		Console << "ITEMLIST " << listnum << " not found, aborting" << myendl;
		return;
	}
	
	const char *tag = NULL;
	CItem *j = NULL;
	for( tag = Enlist->First(); !Enlist->AtEnd(); tag = Enlist->Next() )
	{
		j = Items->SpawnItemToPack( mSock, mSock->CurrcharObj(), tag, false );
		RefreshItem( j );
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	void criminal( CChar *c )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Make character a criminal
//o---------------------------------------------------------------------------o
void criminal( CChar *c )
{
	if( !c->IsCriminal() )
	{
		c->SetCrimFlag( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetRepCrimTime() ) );
		sysmessage( calcSocketObjFromChar( c ), 1379 );
		setcharflag( c );
		if( region[c->GetRegion()]->IsGuarded() && cwmWorldState->ServerData()->GetGuardsStatus() )
			Combat->SpawnGuard( c, c, c->GetX(), c->GetY(), c->GetZ() );
	}
	else
	{		// let's update their flag, as another criminal act will reset the timer
		c->SetCrimFlag( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetRepCrimTime() ) );
		// chcek to see if there is a guard nearby, otherwise spawn us a new one
		if( !region[c->GetRegion()]->IsGuarded() || !cwmWorldState->ServerData()->GetGuardsStatus() )
			return;
		int xOffset = MapRegion->GetGridX( c->GetX() );
		int yOffset = MapRegion->GetGridY( c->GetY() );
		UI08 worldNumber = c->WorldNumber();
		for( SI08 counter1 = -1; counter1 <= 1; counter1++ )
		{
			for( SI08 counter2 = -1; counter2 <= 1; counter2++ )
			{
				SubRegion *toCheck = MapRegion->GetGrid( xOffset + counter1, yOffset + counter2, worldNumber );
				if( toCheck == NULL )
					continue;
				toCheck->PushChar();
				for( CChar *charCheck = toCheck->FirstChar(); !toCheck->FinishedChars(); charCheck = toCheck->GetNextChar() )
				{
					if( charCheck == NULL )
						continue;
					if( charCheck->GetNPCAiType() == 0x04 )
					{
						if( objInRange( c, charCheck, cwmWorldState->ServerData()->GetCombatMaxRange() ) )
						{
							npcAttackTarget( c, charCheck );
							toCheck->PopChar();
							return;
						}
					}
				}
				toCheck->PopChar();
			}
		}
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	void setcharflag( CChar *c )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Set characters flag
//o---------------------------------------------------------------------------o
void setcharflag( CChar *c )
{
	if( c == NULL )
		return;
	UI08 oldFlag = c->GetFlag();
	if( !c->IsNpc() )
	{
		if( c->GetKills() > cwmWorldState->ServerData()->GetRepMaxKills() ) 
			c->SetFlagRed();
		else if( c->GetCrimFlag() > 0 || c->GetCrimFlag() < -1 )
			c->SetFlagGray();
		else 
			c->SetFlagBlue();
	} 
	else 
	{
		switch( c->GetNPCAiType() )
		{
		case 2://evil
		case 0x50://?
		case 666://evil healer
		case 0x58://bs/ev
			c->SetFlagRed();
			break;
		case 1://good
		case 17://Player Vendor
		case 4://healer
		case 30://?
		case 40://?
		case 0x08://banker
			c->SetFlagBlue();
			break;
		default:
			if( c->GetID() == 0x0190 || c->GetID() == 0x0191 )
			{
				c->SetFlagBlue();
				break;
			}
			else if( cwmWorldState->ServerData()->GetCombatAnimalsGuarded() && creatures[c->GetID()].IsAnimal() )
			{
				if( region[c->GetRegion()]->IsGuarded() )	// in a guarded region, with guarded animals, animals == blue
					c->SetFlagBlue();
				else
					c->SetFlagGray();
			}
			else	// if it's not a human form, and animal's aren't guarded, then they're gray
				c->SetFlagGray();
			if( c->GetOwnerObj() != NULL && c->IsTamed() )
			{
				CChar *i = (CChar *)c->GetOwnerObj();
				if( i != NULL )
					c->SetFlag( i->GetFlag() );
				else
					c->SetFlagBlue();
				if( c->IsInnocent() && !cwmWorldState->ServerData()->GetCombatAnimalsGuarded() )
					c->SetFlagBlue();
			}
			break;
		}
	}
	UI08 newFlag = c->GetFlag();
	if( oldFlag != newFlag )
	{
		UI16 targTrig = c->GetScriptTrigger();
		cScript *toExecute = Trigger->GetScript( targTrig );
		if( toExecute != NULL )
			toExecute->OnFlagChange( c, newFlag, oldFlag );
	}
}

//o---------------------------------------------------------------------------o
//|	Function	-	void RefreshItem( CItem *i )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Send this item to all online people in range
//o---------------------------------------------------------------------------o
void RefreshItem( CItem *i )
{
	if( i == NULL ) 
		return;

	if( i->GetCont() == i )
	{
		Console << myendl << "ALERT! item " << i->GetName() << " [serial: " << i->GetSerial() << "] has dangerous container value, autocorrecting" << myendl;
		i->SetCont( NULL );
	}

	cBaseObject *iCont = i->GetCont();
	if( iCont == NULL )
	{
		Network->PushConn();
		for( cSocket *aSock = Network->FirstSocket(); !Network->FinishedSockets(); aSock = Network->NextSocket() ) // send this item to all the sockets in range
		{
			CChar *aChar = aSock->CurrcharObj();
			if( aChar == NULL )
				continue;

			if( objInRange( aChar, i, aSock->Range() + Races->VisRange( aChar->GetRace() ) ) )
				sendItem( aSock, i );
		}
		Network->PopConn();
		return;
	}
	else if( iCont->GetObjType() == OT_CHAR )
	{
		if( i->GetGlow() > 0 )
			Items->GlowItem( i );

		CChar *charCont = (CChar *)iCont;
		if( charCont != NULL )
		{
			CPWornItem toWear = (*i);
			Network->PushConn();
			for( cSocket *cSock = Network->FirstSocket(); !Network->FinishedSockets(); cSock = Network->NextSocket() )
			{
				if( charInRange( cSock->CurrcharObj(), charCont ) )
					cSock->Send( &toWear );
			}
			Network->PopConn();
			return;
		}
	}
	else
	{
		CItem *itemCont = (CItem *)iCont;
		if( itemCont != NULL )
		{
			Network->PushConn();
			for( cSocket *bSock = Network->FirstSocket(); !Network->FinishedSockets(); bSock = Network->NextSocket() )
				sendPackItem( bSock, i );
			Network->PopConn();
			return;
		}
	}
	Console.Error( 2, " RefreshItem(%i): cannot determine container type!", i );
}

//o---------------------------------------------------------------------------o
//|   Function    -  void doDeathStuff( CChar *i )
//|   Date        -  UnKnown
//|   Programmer  -  UnKnown  (Touched tabstops by Tauriel Dec 28, 1998)
//o---------------------------------------------------------------------------o
//|   Purpose     -  Performs death stuff. I.E.- creates a corpse, moves items
//|                  to it, take out of war mode, does animation and sound, etc.
//o---------------------------------------------------------------------------o
void doDeathStuff( CChar *i )
{
	if( i == NULL || i->IsDead() || i->IsInvulnerable() )	// don't kill them if they are dead or invulnerable!
		return;

	cSocket *pSock = calcSocketObjFromChar( i );
	UI08 nType = 0;

	if( i->GetID() != i->GetOrgID() )
	{
		i->SetID( i->GetOrgID() );
		i->Teleport();
	}

	i->SetxID( i->GetID() );
	i->SetxSkin( i->GetSkin() );
	if( ( i->GetID( 1 ) == 0x00 ) && ( i->GetID( 2 ) == 0x0C || ( i->GetID( 2 ) >= 0x3B && i->GetID( 2 ) <= 0x3D ) ) ) // If it's a dragon, 50/50 chance you can carve it
		nType = RandomNum( 0, 1 );

	CChar *murderer = NULL;
	if( i->GetAttacker() != INVALIDSERIAL ) 
		murderer = &chars[i->GetAttacker()]; 

	if( pSock != NULL )
		DismountCreature( i );
	killTrades( i );

	if( !i->IsNpc() ) 
	{
	//	i->SetID( 0x01, 1 ); // Character is a ghost
	if( i->GetxID( 2 ) == 0x91)
		i->SetID( 0x193 );  // Male or Female
	else
		i->SetID( 0x192 );
	}
	Effects->playDeathSound( i );
	i->SetSkin( 0x0000 );
	i->SetDead( true );
	i->StopSpell();
	i->SetHP( 0 );
	i->SetPoisoned( 0 );
	i->SetPoison( 0 );

	CItem *corpsenum = GenerateCorpse( i, nType, murderer );

	if( !i->IsNpc() )
	{ 
		CItem *c = Items->SpawnItem( NULL, i, 1, Dictionary->GetEntry( 1610 ), 0, 0x204E, 0, false, false );

		if( c == NULL ) 
			return;
		i->SetRobe( c->GetSerial() );
		c->SetLayer( 0x16 );
		if( c->SetCont( i ) )
			c->SetDef( 1 );
	}
	if( cwmWorldState->ServerData()->GetDeathAnimationStatus() )
		Effects->deathAction( i, corpsenum );
	if( i->GetAccount().wAccountIndex != AB_INVALID_ID )
	{
		i->Teleport();
		if( pSock != NULL )
		{
			CPResurrectMenu toSend( 0 );
			pSock->Send( &toSend );
		}
	}		
	
	RefreshItem( corpsenum );

	UI16 targTrig = i->GetScriptTrigger();
	cScript *toExecute = Trigger->GetScript( targTrig );
	if( toExecute != NULL )
		toExecute->OnDeath( i );

	if( i->IsNpc() ) 
		Npcs->DeleteChar( i );
}

//o---------------------------------------------------------------------------o
//|	Function	-	CItem *GenerateCorpse( CChar *i, UI08 nType, char *murderername )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Generates a corpse based on skin of the character killed
//o---------------------------------------------------------------------------o
CItem *GenerateCorpse( CChar *i, UI08 nType, CChar *murderer )
{
	char temp[512];
	CItem *k, *c, *p = getPack( i );
	sprintf( temp, Dictionary->GetEntry( 1612 ), i->GetName() );
	bool corpse = !( ( i->GetxID() >= 13 && i->GetxID() <= 16 && i->GetxID() != 14 ) || i->GetxID() == 574 );

	if( corpse )
	{
		c = Items->SpawnItem( NULL, i, 1, temp, false, 0x2006, i->GetxSkin(), false, false );
		if( c == NULL ) 
			return NULL;
		c->SetCorpse( true );
		c->SetCarve( i->GetCarve() );
		c->SetMovable( 2 );//non-movable
		c->SetAmount( i->GetxID() );
		c->SetDir( i->GetDir() );
	} 
	else 
	{
		c = Items->SpawnItem( NULL, i, 1, Dictionary->GetEntry( 1611 ), false, 0x09B2, 0x0000, false, false );
		if( c== NULL ) 
			return NULL;
		c->SetCorpse( false );
	}
	c->SetMoreY( i->isHuman() );
	c->SetName2( i->GetName() );

	c->SetType( 1 );
	c->SetLocation( i );

	c->SetMore( (UI08)nType, 1 );
	c->SetDecayTime( BuildTimeValue( (R32)cwmWorldState->ServerData()->GetSystemTimerStatus( DECAY ) ) );
	
	CMultiObj *iMulti = findMulti( c );
	if( iMulti != NULL ) 
		c->SetMulti( iMulti );

	if( !i->IsNpc() )
	{
		c->SetOwner( (cBaseObject *)i );
		c->SetMore( char( cwmWorldState->ServerData()->GetSystemTimerStatus( PLAYER_CORPSE )&0xff ), 4 ); // how many times longer for the player's corpse to decay
	}
	c->SetMoreZ( i->GetFlag() );
	if( murderer == NULL )
		c->SetMurderer( INVALIDSERIAL );
	else
		c->SetMurderer( murderer->GetSerial() );
	c->SetMurderTime( cwmWorldState->GetUICurrentTime() );
	
	for( CItem *j = i->FirstItem(); !i->FinishedItems(); j = i->NextItem() )
	{
		if( j == NULL ) 
			continue;
		
		if( j->GetLayer() != 0x0B && j->GetLayer() != 0x10 )
		{
			if( j->GetType() == 1 && j->GetLayer() != 0x1A && j->GetLayer() != 0x1B && j->GetLayer() != 0x1C && j->GetLayer() != 0x1D )
			{
				for( k = j->FirstItemObj(); !j->FinishedItems(); k = j->NextItemObj() )
				{
					if( k == NULL ) 
						continue;
				
					if( !k->isNewbie() && k->GetType() != 9 )
					{
						k->SetCont( c );
						k->SetX( (UI16)( 20 + ( RandomNum( 0, 49 ) ) ) );
						k->SetY( (UI16)( 85 + ( RandomNum( 0, 75 ) ) ) );
						k->SetZ( 9 );
						RefreshItem( k );
					}
				}
			}

			if( j != p && j->GetLayer() != 0x1D )
			{
				if( j->isNewbie() && p != NULL )
					j->SetCont( p );
				else
					j->SetCont( c );
			}

			if( j->GetLayer() == 0x15 && !i->IsShop() && corpse )
				j->SetLayer( 0x1A );
			j->SetX( (UI16)( 20 + ( RandomNum( 0, 49 ) ) ) );
			j->SetY( (UI16)( 85 + ( RandomNum( 0, 74 ) ) ) );
			j->SetZ( 9 );

			CPRemoveItem toRemove( (*j) );
			Network->PushConn();
			for( cSocket *kSock = Network->FirstSocket(); !Network->FinishedSockets(); kSock = Network->NextSocket() )
				kSock->Send( &toRemove );
			Network->PopConn();
			if( j != p )
				RefreshItem( j );
		}

		if( j->GetLayer() == 0x0B || j->GetLayer() == 0x10 )
		{
			j->SetName( "Hair/Beard" );
			j->SetX( 0x47 );
			j->SetY( 0x93 );
			j->SetZ( 0 );
		}
	}
	return c;
}

//o---------------------------------------------------------------------------o
//|	Function	-	void SendWorldChange( WorldType season, cSocket *sock )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Send world changes to socket
//o---------------------------------------------------------------------------o
void SendWorldChange( WorldType season, cSocket *sock )
{
	CPWorldChange wrldChange( season, 1 );
	sock->Send( &wrldChange );
}

void SendMapChange( UI08 worldNumber, cSocket *sock, bool initialLogin )
{
	if( sock == NULL )
		return;
	CMapChange mapChange( worldNumber );
	if( !initialLogin && worldNumber > 1 )
	{
		switch( sock->ClientType() )
		{
		case CV_UO3D:
		case CV_KRRIOS:
			break;
		default:
			//mapChange.SetMap( 0 );
			break;
		}
	}
	sock->Send( &mapChange );
	CChar *mChar = sock->CurrcharObj();
	mChar->Teleport();
}

void SocketMapChange( cSocket *sock, CChar *charMoving, CItem *gate )
{
	if( gate == NULL || ( sock == NULL && charMoving == NULL ) )
		return;
	UI08 tWorldNum = (UI08)gate->GetMore();
	if( !Map->MapExists( tWorldNum ) )
		return;
	CChar *toMove = charMoving;
	if( sock != NULL && charMoving == NULL )
		toMove = sock->CurrcharObj();
	if( toMove == NULL )
		return;
	switch( sock->ClientType() )
	{
	case CV_UO3D:
	case CV_KRRIOS:
		toMove->SetLocation( (SI16)gate->GetMoreX(), (SI16)gate->GetMoreY(), (SI08)gate->GetMoreZ(), tWorldNum );
		break;
	default:
		//if( tWorldNum <= 1 )
			toMove->SetLocation( (SI16)gate->GetMoreX(), (SI16)gate->GetMoreY(), (SI08)gate->GetMoreZ(), tWorldNum );
//		else
//			toMove->SetLocation( (SI16)gate->GetMoreX(), (SI16)gate->GetMoreY(), (SI08)gate->GetMoreZ(), 0 );
		break;
	}
	toMove->RemoveFromSight();
	SendMapChange( tWorldNum, sock );
	toMove->Teleport();

}

//o---------------------------------------------------------------------------o
//|	Function	-	void UseHairDye( cSocket *s, UI16 colour, CItem *x )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Dye hair certain color based on hair dye used
//o---------------------------------------------------------------------------o
void UseHairDye( cSocket *s, UI16 colour, CItem *x )
{
	CChar *dest			= s->CurrcharObj();
	CItem *beardobject	= dest->GetItemAtLayer( 0x10 );
	CItem *hairobject	= dest->GetItemAtLayer( 0x0B );

	if( hairobject != NULL )
	{
		hairobject->SetColour( colour );
		RefreshItem( hairobject );
	}
	if( beardobject != NULL )
	{
		beardobject->SetColour( colour );
		RefreshItem( beardobject );
	}
	Items->DeleItem( x );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void Bounce( cSocket *bouncer, CItem *bouncing )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Bounce items back from where they came
//o---------------------------------------------------------------------------o
void Bounce( cSocket *bouncer, CItem *bouncing )
{
	if( bouncer == NULL || bouncing == NULL )
		return;
	CPBounce bounce( 5 );
	PickupLocations from = bouncer->PickupSpot();
	SERIAL spot = bouncer->PickupSerial();
	switch( from )
	{
	default:
	case PL_NOWHERE:	break;
	case PL_GROUND:		
		SI16 x, y;
		SI08 z;
		x = bouncer->PickupX();
		y = bouncer->PickupY();
		z = bouncer->PickupZ();
		if( bouncing->GetX() != x || bouncing->GetY() != y || bouncing->GetZ() != z )
			bouncing->SetLocation( x, y, z );
		break;
	case PL_OWNPACK:
	case PL_OTHERPACK:
	case PL_PAPERDOLL:
		if( bouncing->GetContSerial() != spot )
			bouncing->SetContSerial( spot );	
		break;
	}
	bouncer->Send( &bounce );
	bouncer->PickupSpot( PL_NOWHERE );
}

//o---------------------------------------------------------------------------o
//|	Function	-	void NetworkPollConnectionThread( void *params )
//|	Programmer	-	Unknown
//o---------------------------------------------------------------------------o
//|	Purpose		-	Watch for new connections
//o---------------------------------------------------------------------------o
#pragma note( "Param Warning: in NetworkPollConnectionThread(), params is unrefrenced" )
void NetworkPollConnectionThread( void *params )
{
	messageLoop << "Thread: NetworkPollConnection has started";
	netpollthreadclose = false;
	while( !netpollthreadclose )
	{
		Network->CheckConnections();
		Network->CheckLoginMessage();
		UOXSleep( 20 );
	}
#if defined(__unix__)
	pthread_exit( NULL );
#else
	_endthread();
#endif
	messageLoop << "Thread: NetworkPollConnection has Closed";
}

CMultiObj *findMulti( cBaseObject *i )
{
	if( i == NULL )
		return NULL;
	return findMulti( i->GetX(), i->GetY(), i->GetZ(), i->WorldNumber() );
}

//o---------------------------------------------------------------------------o
//|	Function	-	CMultiObj *findMulti( SI16 x, SI16 y, SI08 z, UI08 worldNumber )
//|	Programmer	-	Zippy
//o---------------------------------------------------------------------------o
//|	Purpose		-	Find a multi at x,y,z
//o---------------------------------------------------------------------------o
CMultiObj *findMulti( SI16 x, SI16 y, SI08 z, UI08 worldNumber )
{
	int lastdist=30;
	CMultiObj *multi = NULL;
	int ret, dx, dy;

	int xOffset = MapRegion->GetGridX( x );
	int yOffset = MapRegion->GetGridY( y );
	for( SI08 counter1 = -1; counter1 <= 1; counter1++ )
	{
		for( SI08 counter2 = -1; counter2 <= 1; counter2++ )
		{
			SubRegion *toCheck = MapRegion->GetGrid( xOffset + counter1, yOffset + counter2, worldNumber );
			if( toCheck == NULL )
				continue;
			toCheck->PushItem();
			for( CItem *itemCheck = toCheck->FirstItem(); !toCheck->FinishedItems(); itemCheck = toCheck->GetNextItem() )
			{
				if( itemCheck == NULL )
					continue;
				if( itemCheck->GetID( 1 ) >= 0x40 )
				{
					dx = abs( x - itemCheck->GetX() );
					dy = abs( y - itemCheck->GetY() );
					ret = (int)( hypot( dx, dy ) );
					if( ret <= lastdist )
					{
						lastdist = ret;
						if( inMulti( x, y, z, itemCheck, worldNumber ) )
						{
							multi = static_cast<CMultiObj *>(itemCheck);
							toCheck->PopItem();
							return multi;
						}
					}
				}
			}
			toCheck->PopItem();
		}
	}
	return multi;
}

//o---------------------------------------------------------------------------o
//|	Function	-	bool inMulti( SI16 x, SI16 y, SI08 z, CItem *m, UI08 worldNumber )
//|	Programmer	-	Zippy
//o---------------------------------------------------------------------------o
//|	Purpose		-	Check if item is in a multi
//|						z is currently unrefrenced, but may be used in the future
//o---------------------------------------------------------------------------o
bool inMulti( SI16 x, SI16 y, SI08 z, CItem *m, UI08 worldNumber )
{
	if( m == NULL )
		return false;
	SI32 length;
	st_multi *multi = NULL;
	UI16 multiID = (UI16)(( m->GetID() ) - 0x4000);
	Map->SeekMulti( multiID, &length );

	if( length == -1 || length >= 17000000)
	{
		Console << "inmulti() - Bad length in multi file, avoiding stall. Item Name: " << m->GetName() << " " << m->GetSerial() << myendl;
		length = 0;
	}
	for( SI32 j = 0; j < length; j++ )
	{
		multi = Map->SeekIntoMulti( multiID, j );

		if( multi->visible && ( m->GetX() + multi->x == x ) && ( m->GetY() + multi->y == y ) )
			return true;
	}
	return false;
}

