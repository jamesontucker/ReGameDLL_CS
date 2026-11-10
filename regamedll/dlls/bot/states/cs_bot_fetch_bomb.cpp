/*
*
*   This program is free software; you can redistribute it and/or modify it
*   under the terms of the GNU General Public License as published by the
*   Free Software Foundation; either version 2 of the License, or (at
*   your option) any later version.
*
*   This program is distributed in the hope that it will be useful, but
*   WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*   General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software Foundation,
*   Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*   In addition, as a special exception, the author gives permission to
*   link the code of this program with the Half-Life Game Engine ("HL
*   Engine") and Modified Game Libraries ("MODs") developed by Valve,
*   L.L.C ("Valve").  You must obey the GNU General Public License in all
*   respects for all of the code used other than the HL Engine and MODs
*   from Valve.  If you modify this file, you may extend this exception
*   to your version of the file, but you are not obligated to do so.  If
*   you do not wish to do so, delete this exception statement from your
*   version.
*
*/

#include "precompiled.h"

// Move to the bomb on the floor and pick it up
void FetchBombState::OnEnter(CCSBot *me)
{
	me->DestroyPath();
}

// Move to the bomb on the floor and pick it up
void FetchBombState::OnUpdate(CCSBot *me)
{
	if (me->IsCarryingBomb())
	{
		me->PrintIfWatched("I picked up the bomb\n");
		me->Idle();
		return;
	}

	CBaseEntity *pBomb = TheCSBots()->GetLooseBomb();
	if (pBomb)
	{
		if (!me->HasPath())
		{
			// build a path to the bomb
			// (Loner-Strike: a bomb dropped against a wall or on a ledge often isn't over any area
			// of the map's walking mesh: then the nearest one, instead of giving up straight away;
			// 41% of dropped bombs were never picked up)
			CNavArea *pArea = TheNavAreaGrid.GetNavArea(&pBomb->pev->origin);
			if (!pArea && LsFight())
				pArea = TheNavAreaGrid.GetNearestNavArea(&pBomb->pev->origin);
			if (me->ComputePath(pArea, &pBomb->pev->origin, SAFEST_ROUTE) == false)
			{
				me->PrintIfWatched("Fetch bomb pathfind failed\n");

				// go Hunt instead of Idle to prevent continuous re-pathing to inaccessible bomb
				me->Hunt();
				//return;
			}
		}
	}
	else
	{
		// someone picked up the bomb
		me->PrintIfWatched("Bomb not loose\n");
		me->Idle();
		return;
	}

	// look around
	me->UpdateLookAround();

	if (me->UpdatePathMovement() != CCSBot::PROGRESSING)
		me->Idle();
}
