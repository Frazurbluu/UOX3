// cHTMLSystem.h: Schnittstelle f�r die Klasse cHTMLSystem.
//
//////////////////////////////////////////////////////////////////////

#ifndef __CHTMLSYSTEM_H__
#define __CHTMLSYSTEM_H__

#if defined(_MSC_VER)
#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000
#endif

enum ETemplateType {
	ETT_PERIODIC = 1,
	ETT_OFFLINE,
	ETT_GUILD,
	ETT_PLAYER
};

class cHTMLTemplate
{
protected:
	UI32	UpdateTimer;
	char	InputFile[MAX_PATH];
	bool	Loaded;
	UI08	Type;
	std::string	Content;
	char	OutputFile[MAX_PATH];
	std::string	Name;
	UI32	ScheduledUpdate;

public:
	cHTMLTemplate();
	virtual ~cHTMLTemplate();
	void Process( void );
	void Poll( bool Force = false );
	void LoadTemplate( void );
	void UnloadTemplate( void );
	void Load( ScriptSection *found );

	// Some Getters
	std::string	GetName( void );
	std::string	GetOutput( void );
	std::string	GetInput( void );
	UI08	GetTemplateType( void );	
	UI32	GetScheduledUpdate( void );
	UI32	GetUpdateTimer( void );
};

class cHTMLTemplates
{
protected:
	std::vector < cHTMLTemplate* > Templates;

public:
	cHTMLTemplates();
	virtual ~cHTMLTemplates();

	void Load( void );
	void Unload( void );
	void Poll( UI08 TemplateType );
	bool Refresh( UI32 TemplateID );
	void TemplateInfoGump( cSocket *mySocket );
};

#endif
