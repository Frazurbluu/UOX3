#ifndef __GUMP_H__
#define __GUMP_H__

struct GumpInfo
{
	std::string name;
	long int value;
	UI08 type;
	std::string stringValue;
	// acceptable type values
	// 0 -> straight plain long int
	// 1 -> hex long int
	// 2 -> long int separated into 4 parts, decimal
	// 3 -> long int separated into 4 parts, hex
	// 4 -> string
	// 5 -> 2 byte hex display
	// 6 -> 2 byte decimal display
};

void MultiGumpCallback( cSocket *mySocket, SERIAL GumpSerial, UI32 Button );

class CGump
{
private:
	STRINGLIST TagList;
	STRINGLIST TextList;
	bool NoMove;
	bool NoClose;
	UI32 Serial;
	UI32 PageCount;
	UI32 Type;
public:
	CGump( bool myNoMove, bool myNoClose );
	virtual ~CGump( );

	void Add( std::string Tag, std::string Text );
	void Send( cSocket *target );
	
	// Common add functions
	void AddBackground( UI16 x, UI16 y, UI16 GumpID, UI16 width, UI16 height );
	void AddGump( UI16 x, UI16 y, UI16 GumpID );
	void AddButton( UI16 x, UI16 y, UI16 ImageUp, UI16 ImageDown, UI16 Behaviour, UI16 Page, UI32 UniqueID  );
	void AddText( UI16 x, UI16 y, UI16 hue, std::string Text );
	UI32 StartPage( void );

	void SetNoMove( bool myNoMove );
	void SetNoClose( bool myNoClose );

	void SetType( UI32 newType ); // Gump Type (dont ask me...)
	void SetSerial( UI32 newSerial ); // Gump Type (dont ask me...)
};

class GumpDisplay
{
private:
	std::vector< GumpInfo * > gumpData;
	UI16 width, height;	// gump width / height
	cSocket *toSendTo;
	STRINGLIST one, two;
	std::string title;
public:
	void AddData( GumpInfo *toAdd );
	void AddData( const char *toAdd, long int value, UI08 type = 0 );
	void AddData( const char *toAdd, const char *toSet, UI08 type = 4 );
	GumpDisplay( cSocket *target );
	GumpDisplay( cSocket *target, UI16 gumpWidth, UI16 gumpHeight );
	virtual ~GumpDisplay();
	void SetTitle( const char *newTitle );
	void Send( long gumpNum, bool isMenu, SERIAL serial );
	void Delete( void );
};

class CPIGumpMenuSelect;

class cGump
{
public:
	void Button( CPIGumpMenuSelect *packet );
	void Input( cSocket *s );
	void Menu( cSocket *s, int m );
	void Open( cSocket *s, CChar *i, UI16 gumpNum );
};

#endif

