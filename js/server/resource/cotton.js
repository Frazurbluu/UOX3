// Cotton-Picking Script
// 19/02/2003 Xuri; xuri@sensewave.com
// When a (dynamic) cotton plant is double-clicked, it may yield some cotton.
// Then a timer will start, and no more cotton can be picked until it runs out.

function onUse( pUser, iUsed )
{
	var isInRange = pUser.InRange( iUsed, 3 );
	if( !isInRange )
 	{
		pUser.SysMessage( "You are too far away to reach that." );
		return;
	}

	if( !iUsed.GetTag("initialized")) // Unless cotton have been picked before, initialize settings
	{
		iUsed.SetTag("initialized",true); 	// Marks tree as initialized
		iUsed.SetTag("Cotton",1); 		// If set to 1, there is cotton to be picked
	}
	var Cotton = iUsed.GetTag("Cotton");
	if (Cotton == 0)
	{	
		pUser.SysMessage( "You find no cotton to pick. Try again later." );
	}
	if( Cotton == 1 )
	{
		iUsed.SoundEffect( 0x004F, true );
		var loot = RollDice( 1, 3, 0 );
		if( loot == 2 )
			pUser.SysMessage( "You fail to pick any cotton." );
		if( loot == 3 || loot == 1 )
	 	{
			pUser.SysMessage( "You harvest some cotton." );
			var itemMade = CreateDFNItem( pUser.socket, pUser, "0x0df9", false, 1, true, true );
			iUsed.SetTag( "Cotton", 0 );
			iUsed.StartTimer( 30000, 1, true ); // Puts in a delay of 30 seconds until next time more cotton respawns
		}
	}
}

function onTimer( iUsed, timerID )
{
	if( timerID == 1 )
	{
		iUsed.SetTag("Cotton", 1);
	}
}