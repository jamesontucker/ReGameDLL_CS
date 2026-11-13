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

const float updateTimesliceDuration = 0.5f;

unsigned int _generationIndex = 0; // used for iterating nav areas during generation process

inline CNavNode *LadderEndSearch(CBaseEntity *pEntity, const Vector *pos, NavDirType mountDir)
{
	Vector center = *pos;
	AddDirectionVector(&center, mountDir, HalfHumanWidth);

	// Test the ladder dismount point first, then each cardinal direction one and two steps away
	for (int dir = (-1); dir < 2 * NUM_DIRECTIONS; dir++)
	{
		Vector tryPos = center;

		if (dir >= NUM_DIRECTIONS)
			AddDirectionVector(&tryPos, (NavDirType)(dir - NUM_DIRECTIONS), GenerationStepSize * 2.0f);
		else if (dir >= 0)
			AddDirectionVector(&tryPos, (NavDirType)dir, GenerationStepSize);

		// step up a rung, to ensure adjacent floors are below us
		tryPos.z += GenerationStepSize;
		SnapToGrid(&tryPos);

		// adjust height to account for sloping areas
		Vector tryNormal;
		if (GetGroundHeight(&tryPos, &tryPos.z, &tryNormal) == false)
			continue;

		// make sure this point is not on the other side of a wall
		const float fudge = 2.0f;
		TraceResult result;
		UTIL_TraceLine(center + Vector(0, 0, fudge), tryPos + Vector(0, 0, fudge), ignore_monsters, dont_ignore_glass, ENT(pEntity->pev), &result);

		if (result.flFraction != 1.0f
#ifdef REGAMEDLL_FIXES
			|| result.fStartSolid
#endif
		)
			continue;

		// if no node exists here, create one and continue the search
		if (!CNavNode::GetNode(&tryPos))
		{
			return new CNavNode(&tryPos, &tryNormal, nullptr);
		}
	}

	return nullptr;
}

// Loner-Strike: a spawn point the search never reached (a wall, cage or barrier between it
// and the rest, as on Base Builder's zombie side or a Deathrun booth) becomes a seed of its own,
// so every spawn gets a nav. Of the four grid points round the spawn, one already on the nav
// means the spawn is reached; otherwise the first with the spawn's own floor under it (a
// harmless drop below, not a pit far down or a crate or wall top) and a clear line
// to it from the spawn (not the room through the wall) is the seed. A seeded spawn has its node
// next time, so each spawn seeds at most once and the search still ends.
inline CNavNode *SpawnSeedSearchPass(bool report)
{
	static const char *const spawnClasses[] = { "info_player_start", "info_player_deathmatch", "info_vip_start", "info_spawn_point" };
	for (auto cls : spawnClasses)
	{
		CBaseEntity *spot = nullptr;
		while ((spot = UTIL_FindEntityByClassname(spot, cls)) != nullptr)
		{
			const Vector origin = spot->pev->origin;
			const float cx = floorf(origin.x / GenerationStepSize) * GenerationStepSize;
			const float cy = floorf(origin.y / GenerationStepSize) * GenerationStepSize;

			Vector seed, seedNormal;
			bool reached = false, found = false;
			char why[4][48];
			for (int k = 0; k < 4 && !reached; k++)
			{
				Vector pos(cx + (k & 1) * GenerationStepSize, cy + (k >> 1) * GenerationStepSize, origin.z);
				Q_snprintf(why[k], sizeof(why[k]), "ok");

				Vector normal;
				if (!GetGroundHeight(&pos, &pos.z, &normal))
				{
					Q_snprintf(why[k], sizeof(why[k]), "no floor");
					continue;
				}

				// the spawn's own floor: below its origin by no more than a fall a player survives with
				// little or no harm (mappers float spawns 50-100 units up, and Deathrun maps stack them
				// 240-340 up so a crowd drops in; a pit is far deeper)
				float drop = origin.z - pos.z;
				if (drop < 0.0f || drop > 400.0f)
				{
					Q_snprintf(why[k], sizeof(why[k]), "floor %.0f below", drop);
					continue;
				}

				if (CNavNode::GetNode(&pos))
				{
					reached = true;
					break;
				}

				// in the spawn's own room: nothing between the spawn and the point
				// (to a step above the floor: a crouched spawn in a vent has its ceiling at head height)
				// (doors and breakables don't count, as for the ground below: a Deathrun crowd waits on a
				// trapdoor that opens at round start)
				TraceResult result;
				Vector from = origin;
				edict_t *skip = nullptr;
				for (int tries = 0; tries < 4; tries++)
				{
					UTIL_TraceLine(from, pos + Vector(0, 0, StepHeight), ignore_monsters, dont_ignore_glass, skip, &result);
					if (result.flFraction == 1.0f || result.fStartSolid || !result.pHit
						|| !IsEntityWalkable(VARS(result.pHit), WALK_THRU_DOORS | WALK_THRU_BREAKABLES))
						break;
					skip = result.pHit;
					from = result.vecEndPos;
				}
				if (result.flFraction != 1.0f || result.fStartSolid)
				{
					Q_snprintf(why[k], sizeof(why[k]), "%s (%s)", result.fStartSolid ? "spawn in solid" : "blocked",
						result.pHit ? STRING(result.pHit->v.classname) : "world");
					continue;
				}

				if (!found)
				{
					seed = pos;
					seedNormal = normal;
					found = true;
				}
			}

			if (!reached && found && !report)
				return new CNavNode(&seed, &seedNormal, nullptr);

			// (the last pass, nothing left to seed: why a spawn still has no nav, for the log)
			if (report && !reached && !found)
				CONSOLE_ECHO("Loner-Strike: no nav at %s %.0f %.0f %.0f: %s / %s / %s / %s\n", cls, origin.x, origin.y, origin.z, why[0], why[1], why[2], why[3]);
		}
	}

	return nullptr;
}

inline CNavNode *SpawnSeedSearch()
{
	CNavNode *node = SpawnSeedSearchPass(false);
	if (!node)
		SpawnSeedSearchPass(true);
	return node;
}

CNavNode *CCSBot::AddNode(const Vector *destPos, const Vector *normal, NavDirType dir, CNavNode *source)
{
	// check if a node exists at this location
	CNavNode *node = const_cast<CNavNode *>(CNavNode::GetNode(destPos));

	// if no node exists, create one
	bool useNew = false;
	if (!node)
	{
		node = new CNavNode(destPos, normal, source);
		useNew = true;
	}

	// connect source node to new node
	source->ConnectTo(node, dir);

	// optimization: if deltaZ changes very little, assume connection is commutative
	const float zTolerance = 10.0f; // 50.0f;
	if (Q_fabs(source->GetPosition()->z - destPos->z) < zTolerance)
	{
		node->ConnectTo(source, OppositeDirection(dir));
		node->MarkAsVisited(OppositeDirection(dir));
	}

	if (useNew)
	{
		// new node becomes current node
		m_currentNode = node;
	}

	Vector ceiling;
	Vector floor;
	TraceResult result;

	bool crouch = false;
	const float epsilon = 0.1f;

	for (float y = -16.0f; y <= 16.0f + epsilon; y += 16.0f)
	{
		for (float x = -16.0f; x <= 16.0f + epsilon; x += 16.0f)
		{
			floor = *destPos + Vector(x, y, 5.0f);
			ceiling = *destPos + Vector(x, y, 72.0f - epsilon);

			UTIL_TraceLine(floor, ceiling, ignore_monsters, dont_ignore_glass, ENT(pev), &result);

			if (result.flFraction != 1.0f)
			{
				crouch = true;
				break;
			}
		}

		if (crouch)
		{
			node->SetAttributes(NAV_CROUCH);
			break;

		}
	}

	return node;
}

void drawProgressMeter(float progress, char *title)
{
	MESSAGE_BEGIN(MSG_ALL, gmsgBotProgress);
		WRITE_BYTE(BOT_PROGGRESS_DRAW);
		WRITE_BYTE(int(progress * 100.0f));
		WRITE_STRING(title);
	MESSAGE_END();
}

void startProgressMeter(const char *title)
{
	MESSAGE_BEGIN(MSG_ALL, gmsgBotProgress);
		WRITE_BYTE(BOT_PROGGRESS_START);
		WRITE_STRING(title);
	MESSAGE_END();
}

void hideProgressMeter()
{
	MESSAGE_BEGIN(MSG_ALL, gmsgBotProgress);
		WRITE_BYTE(BOT_PROGGRESS_HIDE);
	MESSAGE_END();
}

void CCSBot::StartLearnProcess()
{
	startProgressMeter("#CZero_LearningMap");
	drawProgressMeter(0, "#CZero_LearningMap");
	BuildLadders();
	LsCourseLearnStart();	// Loner-Strike: obstacle courses (cs_bot_ls_deathrun.cpp)

	Vector normal;
	Vector pos = pev->origin;

	SnapToGrid(&pos.x);
	SnapToGrid(&pos.y);

	if (!GetGroundHeight(&pos, &pos.z, &normal))
	{
		CONSOLE_ECHO("ERROR: Start position invalid\n\n");
		m_processMode = PROCESS_NORMAL;
		return;
	}

	m_currentNode = new CNavNode(&pos, &normal);
	m_goalPosition = pev->origin;
	m_processMode = PROCESS_LEARN;
}

// Search the world and build a map of possible movements.
// The algorithm begins at the bot's current location, and does a recursive search
// outwards, tracking all valid steps and generating a directed graph of CNavNodes.
// Sample the map one "step" in a cardinal direction to learn the map.
// Returns true if sampling needs to continue, or false if done.
bool CCSBot::LearnStep()
{
	// take a step
	while (true)
	{
		if (!m_currentNode)
		{
			// search is exhausted - continue search from ends of ladders
			for (auto ladder : TheNavLadderList)
			{
				// check ladder bottom
				if ((m_currentNode = LadderEndSearch(ladder->m_entity, &ladder->m_bottom, ladder->m_dir)) != 0)
					break;

				// check ladder top
				if ((m_currentNode = LadderEndSearch(ladder->m_entity, &ladder->m_top, ladder->m_dir)) != 0)
					break;
			}

			// then from spawn points nothing reached (Loner-Strike)
			if (!m_currentNode)
				m_currentNode = SpawnSeedSearch();

			// then past gaps a running jump clears, and where teleports go (Loner-Strike: obstacle
			// courses, bot_ls_nav_course)
			if (!m_currentNode)
				m_currentNode = LsCourseSeedSearch();

			if (!m_currentNode)
			{
				// all seeds exhausted, sampling complete
				GenerateNavigationAreaMesh();
				return false;
			}
		}

		// Take a step from this node
		for (int dir = NORTH; dir < NUM_DIRECTIONS; dir++)
		{
			if (!m_currentNode->HasVisited((NavDirType)dir))
			{
				float feetOffset = pev->origin.z - GetFeetZ();

				// start at current node position
				Vector pos = *m_currentNode->GetPosition();

				// snap to grid
				int cx = SnapToGrid(pos.x);
				int cy = SnapToGrid(pos.y);

				// attempt to move to adjacent node
				switch (dir)
				{
				case NORTH: cy -= GenerationStepSize; break;
				case SOUTH: cy += GenerationStepSize; break;
				case EAST:  cx += GenerationStepSize; break;
				case WEST:  cx -= GenerationStepSize; break;
				}

				pos.x = cx;
				pos.y = cy;

				m_generationDir = (NavDirType)dir;

				// mark direction as visited
				m_currentNode->MarkAsVisited(m_generationDir);

				// test if we can move to new position
				TraceResult result;
				Vector from, to;

				// modify position to account for change in ground level during step
				to.x = pos.x;
				to.y = pos.y;
				Vector toNormal;
				if (GetGroundHeight(&pos, &to.z, &toNormal) == false)
				{
					return true;
				}

				from = *m_currentNode->GetPosition();

				Vector fromOrigin = from + Vector(0, 0, feetOffset);
				Vector toOrigin = to + Vector(0, 0, feetOffset);

				UTIL_SetOrigin(pev, toOrigin);
				UTIL_TraceLine(fromOrigin, toOrigin, ignore_monsters, dont_ignore_glass, ENT(pev), &result);

				bool walkable;

				if (result.flFraction == 1.0f && !result.fStartSolid)
				{
					// the trace didnt hit anything - clear
					float toGround = to.z;
					float fromGround = from.z;

					float epsilon = 0.1f;

					// check if ledge is too high to reach or will cause us to fall to our death
					if (toGround - fromGround > JumpCrouchHeight + epsilon || fromGround - toGround > DeathDrop)
					{
						walkable = false;
					}
					else
					{
						// check surface normals along this step to see if we would cross any impassable slopes
						Vector delta = to - from;
						const float inc = 2.0f;
						float along = inc;
						bool done = false;
						float ground;
						Vector normal;

						walkable = true;

						while (!done)
						{
							Vector p;

							// need to guarantee that we test the exact edges
							if (along >= GenerationStepSize)
							{
								p = to;
								done = true;
							}
							else
							{
								p = from + delta * (along / GenerationStepSize);
							}

							if (GetGroundHeight(&p, &ground, &normal) == false)
							{
								walkable = false;
								break;
							}

							// check for maximum allowed slope
							if (normal.z < 0.7f)
							{
								walkable = false;
								break;
							}

							along += inc;
						}
					}
				}
				// TraceLine hit something...
				else
				{
					if (IsEntityWalkable(VARS(result.pHit), WALK_THRU_EVERYTHING))
					{
						walkable = true;
					}
					else
					{
						walkable = false;
					}
				}
#ifdef REGAMEDLL_FIXES
				// if we're incrementally generating, don't overlap existing nav areas
				CNavArea *overlap = TheNavAreaGrid.GetNavArea(&to, HumanHeight);
				if (overlap)
				{
					walkable = false;
				}
#endif
				if (walkable)
				{
					// we can move here
					// create a new navigation node, and update current node pointer
					CNavNode *newNode = AddNode(&to, &toNormal, m_generationDir, m_currentNode);
				}

				return true;
			}
		}

		// all directions have been searched from this node - pop back to its parent and continue
		m_currentNode = m_currentNode->GetParent();
	}
}

void CCSBot::UpdateLearnProcess()
{
	float startTime = g_engfuncs.pfnTime();
	while (g_engfuncs.pfnTime() - startTime < updateTimesliceDuration)
	{
		if (LearnStep() == false)
		{
			StartAnalyzeAlphaProcess();
			return;
		}
	}
}

void CCSBot::StartAnalyzeAlphaProcess()
{
	m_processMode    = PROCESS_ANALYZE_ALPHA;
	_generationIndex = 0;

	ApproachAreaAnalysisPrep();
	DestroyHidingSpots();

	startProgressMeter("#CZero_AnalyzingHidingSpots");
	drawProgressMeter(0, "#CZero_AnalyzingHidingSpots");
}

bool CCSBot::AnalyzeAlphaStep()
{
	_generationIndex++;

	if (_generationIndex < 0 || _generationIndex >= TheNavAreaList.size())
		return false;

	// TODO: Pretty ugly and very slow way to access element by index
	// There is no reason not to use a vector instead of a linked list
	const NavAreaList::const_iterator &iter = std::next(TheNavAreaList.begin(), _generationIndex - 1);

	CNavArea *area = (*iter);
	area->ComputeHidingSpots();
	area->ComputeApproachAreas();
	return true;
}

void CCSBot::UpdateAnalyzeAlphaProcess()
{
	float startTime = g_engfuncs.pfnTime();
	while (g_engfuncs.pfnTime() - startTime < updateTimesliceDuration)
	{
		if (AnalyzeAlphaStep() == false)
		{
			drawProgressMeter(0.5f, "#CZero_AnalyzingHidingSpots");
			CleanupApproachAreaAnalysisPrep();
			StartAnalyzeBetaProcess();
			return;
		}
	}

	float progress = (double(_generationIndex) / double(TheNavAreaList.size())) * 0.5f;
	drawProgressMeter(progress, "#CZero_AnalyzingHidingSpots");
}

void CCSBot::StartAnalyzeBetaProcess()
{
	m_processMode    = PROCESS_ANALYZE_BETA;
	_generationIndex = 0;
}

bool CCSBot::AnalyzeBetaStep()
{
	_generationIndex++;

	if (_generationIndex < 0 || _generationIndex >= TheNavAreaList.size())
		return false;

	// TODO: Pretty ugly and very slow way to access element by index
	// There is no reason not to use a vector instead of a linked list
	const NavAreaList::const_iterator &iter = std::next(TheNavAreaList.begin(), _generationIndex - 1);

	CNavArea *area = (*iter);
	area->ComputeSpotEncounters();
	area->ComputeSniperSpots();

	return true;
}

void CCSBot::UpdateAnalyzeBetaProcess()
{
	float startTime = g_engfuncs.pfnTime();
	while (g_engfuncs.pfnTime() - startTime < updateTimesliceDuration)
	{
		if (AnalyzeBetaStep() == false)
		{
			drawProgressMeter(1, "#CZero_AnalyzingApproachPoints");
			StartSaveProcess();
			return;
		}
	}

	float progress = (double(_generationIndex) / double(TheNavAreaList.size()) + 1.0f) * 0.5f;
	drawProgressMeter(progress, "#CZero_AnalyzingApproachPoints");
}

void CCSBot::StartSaveProcess()
{
	m_processMode = PROCESS_SAVE;
}

void CCSBot::UpdateSaveProcess()
{
	char gd[64]{};
	GET_GAME_DIR(gd);

	char filename[MAX_OSPATH];
	Q_snprintf(filename, sizeof(filename), "%s\\%s", gd, TheBots->GetNavMapFilename());
	SaveNavigationMap(filename);

	char msg[256]{};
	Q_snprintf(msg, sizeof(msg), "Navigation file '%s' saved.", filename);
	HintMessageToAllPlayers(msg);
	CONSOLE_ECHO("%s\n", msg);

	hideProgressMeter();
	StartNormalProcess();

	// tell bot manager that the analysis is completed
	if (TheCSBots())
		TheCSBots()->AnalysisCompleted();
}

void CCSBot::StartNormalProcess()
{
	m_processMode = PROCESS_NORMAL;
}
