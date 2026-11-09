/*
*
*   Loner-Strike additions to the CZ bots: grenades and economy: team flashes, pop flashes, smokes, newbie grenade dumps, smoke rules, saving and force buys.
*   (cs_bot_ls.cpp has the cvars and the rest; the habits here follow the same rules: bot_ls_fight,
*   LsSolo / LsMatch for the mode, LsTrait for each bot's own leanings)
*
*   Grenades: stock bots pull a grenade out during the quiet start of the round and throw it
*   round the first corner when that's over. Here only the newer pub players do that (and
*   they throw the lot in the first half minute); everyone else keeps them for a reason:
*     - a flash round a corner before walking onto a site or into enemies heard or seen
*       there, banked off a wall to go off out of its own sight when it can, never into a
*       teammate's face, and a look away while it goes off
*     - Ts smoke the way CTs come into the site they're heading for
*     - an HE at enemies heard round a corner
*   one at a time, with a pause between them. None of it in deathmatch.
*   Smoke: stock bots can't see through smoke at all; an enemy they were already shooting
*   at who goes into smoke still gets a moment's spray, and one shooting at them from in
*   there gets shot back.
*   Economy: the team saves when it can't buy, forces now and then after losing, and kits
*   get spread round the CTs. A bot alone against three or more late in a round it's lost
*   hides and keeps its gun.
*
*   Traits used here (LsTrait): 40 throws everything early, 41 how early, 42 likes grenades,
*   43 looks away from its own flash, 44 sprays smoke, 45 buys whatever it can, 46 a pistol
*   on a save, 47 always has a kit, 48 plays the hero in a lost round
*
*/

#include "precompiled.h"

// ---- helpers --------------------------------------------------------------------------------

// t happened within the last few seconds (and on this map: the clock restarts on a new one)
static bool LsWithin(float t, float seconds)
{
	float now = gpGlobals->time;
	return t <= now && now - t < seconds;
}

static const char *LsNadeName(int id)
{
	switch (id)
	{
	case WEAPON_HEGRENADE: return "weapon_hegrenade";
	case WEAPON_FLASHBANG: return "weapon_flashbang";
	case WEAPON_SMOKEGRENADE: return "weapon_smokegrenade";
	}
	return nullptr;
}

static int LsTeamSlot(int team)
{
	return (team == CT) ? 1 : 0;
}

// Last grenade of each kind a team threw (so teammates don't all throw at the same moment)
static float s_lsTeamNadeAt[2][3] = { { -100.0f, -100.0f, -100.0f }, { -100.0f, -100.0f, -100.0f } };
enum { LS_FLASH, LS_SMOKE, LS_HE };

bool CCSBot::LsHasNade(int id)
{
	const char *name = LsNadeName(id);
	return name && HasNamedPlayerItem(name);
}

void CCSBot::LsNadeReset()
{
	m_lsNadePhase = 0;
	m_lsNadeNextAt = 0.0f;
	m_lsNadeNextThink = 0.0f;
	m_lsNadeSmokeRolled = false;
	m_lsNadeCornerRolled = false;
	m_lsSaving = false;
	m_lsSaveDecided = false;
	m_lsSmokeUntil = 0.0f;
}

// A newer pub player who throws every grenade in the first half minute or so (a few
// middling ones have the habit too); never in a match
bool CCSBot::LsNadeDumper()
{
	if (LsMatch())
		return false;
	float skill = LsSkill();
	return (skill < 0.45f && LsTrait(40) < 0.75f) || (skill < 0.6f && LsTrait(40) < 0.15f);
}

// How much it uses its grenades well: skill and its own taste for them, everyone in a match
float CCSBot::LsNadeUse()
{
	float use = LsMatch() ? 0.85f : 0.2f + 0.7f * LsSkill();
	return use * (0.6f + 0.8f * LsTrait(42));
}

// ---- where a grenade goes -----------------------------------------------------------------

// Where a grenade thrown with the view at (pitch, yaw) goes off: its arc and bounces, roughly
// as the engine moves it (half gravity, walls take most of the push out of it, a roll along
// the floor). Flash and HE go off after 1.5 s; a smoke once it's down after that
static Vector LsNadeFlight(CCSBot *me, int id, float pitch, float yaw)
{
	// (as the grenade code turns the view into a throw: aimed up a little, faster aimed higher)
	float tp = (pitch < 0.0f) ? -10.0f + pitch * (80.0f / 90.0f) : -10.0f + pitch * (100.0f / 90.0f);
	float speed = Q_min(750.0f, (90.0f - tp) * 6.0f);
	UTIL_MakeVectors(Vector(tp, yaw, 0.0f));
	Vector fwd = gpGlobals->v_forward;

	Vector eye = me->GetEyePosition();
	Vector pos = eye + fwd * 16.0f;
	TraceResult tr;
	UTIL_TraceLine(eye, pos, ignore_monsters, me->edict(), &tr);
	if (tr.flFraction < 1.0f)
		return eye;

	Vector vel = fwd * speed;
	const float gravity = (id == WEAPON_HEGRENADE ? 0.55f : 0.5f) * 800.0f;
	const float backoff = (id == WEAPON_HEGRENADE) ? 1.3f : 1.2f;	// 2 - friction
	const float dt = 0.07f;
	bool rolling = false;

	for (float t = 0.0f; t < 4.0f; t += dt)
	{
		if (t >= 1.5f && (id != WEAPON_SMOKEGRENADE || rolling))
			break;

		if (rolling)
		{
			float sp = float(vel.Length2D());
			if (sp < 10.0f)
				break;
			float slower = Q_max(0.0f, sp - (2.0f * sp + 60.0f) * dt);
			vel = vel * (slower / sp);
		}
		else
			vel.z -= gravity * dt;

		Vector next = pos + vel * dt;
		UTIL_TraceLine(pos, next, ignore_monsters, me->edict(), &tr);
		if (tr.fStartSolid)
			return pos;

		if (tr.flFraction < 1.0f)
		{
			Vector n = tr.vecPlaneNormal;
			pos = tr.vecEndPos + n * 1.0f;
			float into = float(DotProduct(vel, n));
			vel = vel - n * (backoff * into);
			if (n.z > 0.7f)
			{
				vel = vel * 0.8f;
				if (vel.z < 60.0f)
				{
					rolling = true;
					vel.z = 0.0f;
				}
			}
		}
		else
		{
			pos = next;
			if (rolling)
			{
				// rolled off an edge: falling again
				UTIL_TraceLine(pos, pos - Vector(0, 0, 6.0f), ignore_monsters, me->edict(), &tr);
				if (tr.flFraction >= 1.0f)
					rolling = false;
			}
		}
	}
	return pos;
}

static float LsYawTo(const Vector &from, const Vector &to)
{
	return float(atan2f(to.y - from.y, to.x - from.x) * (180.0f / 3.14159265f));
}

// The view that lands a grenade nearest the spot (within reach): straight at it, from a
// low throw to a lob
bool CCSBot::LsPlanThrow(int id, const Vector &target, float within, Vector *aim, Vector *burst)
{
	Vector eye = GetEyePosition();
	float yaw = LsYawTo(eye, target);
	float best = within;
	bool found = false;
	for (float pitch = 15.0f; pitch >= -55.0f; pitch -= 5.0f)
	{
		Vector end = LsNadeFlight(this, id, pitch, yaw);
		float d = float((end - target).Length());
		if (d < best)
		{
			best = d;
			found = true;
			*burst = end;
			UTIL_MakeVectors(Vector(pitch, yaw, 0.0f));
			*aim = eye + gpGlobals->v_forward * 300.0f;
		}
	}
	return found;
}

// A flash for the corner ahead: it should go off in sight of the spot past the corner and,
// if it can, out of the thrower's own sight (banked off a wall: a pop flash)
bool CCSBot::LsPlanFlash(const Vector &corner, const Vector &beyond, Vector *aim, Vector *burst, bool *inMySight)
{
	Vector eye = GetEyePosition();
	float baseYaw = LsYawTo(eye, corner);
	float bestScore = -99999.0f;
	bool found = false;
	TraceResult tr;

	static const float yaws[] = { 0.0f, -15.0f, 15.0f, -30.0f, 30.0f };
	static const float pitches[] = { 0.0f, -10.0f, -20.0f };
	for (float dy : yaws)
	{
		for (float pitch : pitches)
		{
			Vector end = LsNadeFlight(this, WEAPON_FLASHBANG, pitch, baseYaw + dy);
			Vector at = end + Vector(0, 0, 8.0f);

			// no use going off where nobody past the corner sees it, or right by the thrower
			float gap = float((at - beyond).Length());
			if (gap > 700.0f || (at - eye).IsLengthLessThan(250.0f))
				continue;
			UTIL_TraceLine(at, beyond, ignore_monsters, ignore_glass, edict(), &tr);
			if (tr.flFraction < 1.0f)
				continue;

			UTIL_TraceLine(eye, at, ignore_monsters, ignore_glass, edict(), &tr);
			bool seen = (tr.flFraction >= 1.0f);

			float score = -gap - (seen ? 400.0f : 0.0f);
			if (score > bestScore)
			{
				bestScore = score;
				found = true;
				*burst = at;
				*inMySight = seen;
				UTIL_MakeVectors(Vector(pitch, baseYaw + dy, 0.0f));
				*aim = eye + gpGlobals->v_forward * 300.0f;
			}
		}
	}
	return found;
}

// No teammate (no human one, for the careless) in sight of the burst and facing it
bool CCSBot::LsFlashSafe(const Vector &burst, bool careless)
{
	TraceResult tr;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!pPlayer || pPlayer == this || !pPlayer->IsAlive() || BotRelationship(pPlayer) != BOT_TEAMMATE)
			continue;
		if (careless && pPlayer->IsBot())
			continue;

		Vector eye = pPlayer->pev->origin + pPlayer->pev->view_ofs;
		if ((burst - eye).IsLengthGreaterThan(1500.0f))
			continue;
		UTIL_TraceLine(eye, burst, ignore_monsters, ignore_glass, pPlayer->edict(), &tr);
		if (tr.flFraction < 1.0f)
			continue;

		UTIL_MakeVectors(pPlayer->pev->v_angle);
		Vector to = (burst - eye).Normalize();
		if (float(DotProduct(to, gpGlobals->v_forward)) > 0.3f)
			return false;
	}
	return true;
}

// No teammate where the HE goes off
static bool LsHeSafe(CCSBot *me, const Vector &burst)
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!pPlayer || !pPlayer->IsAlive() || me->BotRelationship(pPlayer) != CCSBot::BOT_TEAMMATE)
			continue;
		if ((pPlayer->pev->origin - burst).IsLengthLessThan(350.0f))
			return false;
	}
	return true;
}

// The corner ahead on its path: the last spot it can see (corner) and a spot round it,
// some way further along (beyond), both at head height
bool CCSBot::LsNadeCorner(Vector *corner, Vector *beyond)
{
	if (!HasPath() || m_pathIndex >= m_pathLength)
		return false;

	int i;
	for (i = m_pathIndex; i < m_pathLength; i++)
	{
		if (!FVisible(m_path[i].pos + Vector(0, 0, HalfHumanHeight)))
			break;
	}
	if (i == m_pathIndex || i >= m_pathLength)
		return false;

	// where on that leg it goes out of sight
	Vector a = m_path[i - 1].pos;
	Vector dir = m_path[i].pos - a;
	float length = dir.NormalizeInPlace();
	Vector seen = a + Vector(0, 0, HalfHumanHeight);
	for (float t = 0.0f; t < length; t += 25.0f)
	{
		Vector p = a + dir * t + Vector(0, 0, HalfHumanHeight);
		if (!FVisible(p))
			break;
		seen = p;
	}
	*corner = seen;

	// and some 250 units round it
	Vector last = seen;
	float left = 250.0f;
	*beyond = m_path[i].pos + Vector(0, 0, HalfHumanHeight);
	for (int k = i; k < m_pathLength; k++)
	{
		Vector p = m_path[k].pos + Vector(0, 0, HalfHumanHeight);
		left -= float((p - last).Length());
		last = p;
		*beyond = p;
		if (left <= 0.0f)
			break;
	}
	return true;
}

// The bomb site a T is heading for before the plant (-1: none)
int CCSBot::LsNadeSite()
{
	if (!TheCSBots() || TheCSBots()->GetScenario() != CCSBotManager::SCENARIO_DEFUSE_BOMB || m_iTeam != TERRORIST
		|| TheCSBots()->IsBombPlanted() || !HasPath())
		return -1;

	const Vector &end = GetPathEndpoint();
	int best = -1;
	float bestDist = 900.0f;
	for (int z = 0; z < TheCSBots()->GetZoneCount(); z++)
	{
		float d = float((TheCSBots()->GetZone(z)->m_center - end).Length2D());
		if (d < bestDist)
		{
			bestDist = d;
			best = z;
		}
	}
	return best;
}

// Somewhere enemies are likely to be: seen or heard there lately, the site the Ts are
// walking onto, or the planted bomb the CTs are going back for
bool CCSBot::LsNadeHot(const Vector &spot)
{
	if (GetTimeSinceLastSawEnemy() < 10.0f && (GetLastKnownEnemyPosition() - spot).IsLengthLessThan(900.0f))
		return true;
	if (IsNoiseHeard() && LsWithin(m_noiseTimestamp, 6.0f) && GetNoisePosition() && (*GetNoisePosition() - spot).IsLengthLessThan(900.0f))
		return true;

	int site = LsNadeSite();
	if (site >= 0 && (TheCSBots()->GetZone(site)->m_center - spot).IsLengthLessThan(1000.0f))
		return true;

	if (m_iTeam == CT && TheCSBots()->IsBombPlanted())
	{
		const Vector *bomb = GetGameState()->GetBombPosition();
		if (bomb && (*bomb - spot).IsLengthLessThan(1000.0f))
			return true;
	}
	return false;
}

// Where the CTs walk into a bomb site from their spawn (the shortest way, some 450 units
// out from the middle of the site): the T smoke goes there. Worked out once per site a map
static bool LsCtLine(int zoneIndex, Vector *spot)
{
	static char s_map[64] = "";
	static bool s_tried[CCSBotManager::MAX_ZONES];
	static bool s_ok[CCSBotManager::MAX_ZONES];
	static Vector s_spot[CCSBotManager::MAX_ZONES];

	if (zoneIndex < 0 || zoneIndex >= CCSBotManager::MAX_ZONES)
		return false;

	if (Q_strcmp(s_map, STRING(gpGlobals->mapname)) != 0)
	{
		Q_strlcpy(s_map, STRING(gpGlobals->mapname));
		for (int z = 0; z < CCSBotManager::MAX_ZONES; z++)
			s_tried[z] = s_ok[z] = false;
	}

	if (!s_tried[zoneIndex])
	{
		const CCSBotManager::Zone *zone = TheCSBots()->GetZone(zoneIndex);
		CBaseEntity *pSpawn = UTIL_FindEntityByClassname(nullptr, "info_player_start");
		CNavArea *from = pSpawn ? TheNavAreaGrid.GetNearestNavArea(&pSpawn->pev->origin) : nullptr;
		CNavArea *to = TheNavAreaGrid.GetNearestNavArea(&zone->m_center);
		// (tried for good once the map's nav is there to try it on)
		s_tried[zoneIndex] = from && to;
		ShortestPathCost cost;
		if (from && to && from != to && NavAreaBuildPath(from, to, &zone->m_center, cost))
		{
			// back from the site towards the CT spawn, to the first area well out of it
			for (CNavArea *area = to; area; area = area->GetParent())
			{
				if ((*area->GetCenter() - zone->m_center).Length2D() > 450.0f)
				{
					s_spot[zoneIndex] = *area->GetCenter() + Vector(0, 0, 20.0f);
					s_ok[zoneIndex] = true;
					break;
				}
			}
		}
	}

	if (s_ok[zoneIndex])
		*spot = s_spot[zoneIndex];
	return s_ok[zoneIndex];
}

// ---- throwing ---------------------------------------------------------------------------

// Gets the grenade out and throws it at the view (aim) it worked out
void CCSBot::LsNadeStart(int id, const Vector &aim, float pause)
{
	if (!IsUsingGrenade() || GetActiveWeapon()->m_iId != id)
		SelectItem(LsNadeName(id));
	m_lsNadeId = id;
	m_lsNadeAim = aim;
	// (kept as a direction from here: it keeps walking while the pin comes out)
	m_lsNadeFrom = pev->origin;
	m_lsNadeAng = UTIL_VecToAngles(aim - GetEyePosition());
	m_lsNadeAng.x = -m_lsNadeAng.x;
	m_lsNadePhase = 1;
	m_lsNadePhaseAt = gpGlobals->time;
	m_lsNadeNextAt = gpGlobals->time + pause;
	LsWhy("grenade");
}

void CCSBot::LsNadeStop()
{
	m_lsNadePhase = 0;
	if (IsUsingGrenade() && !m_isWaitingToTossGrenade)
		EquipBestWeapon(MUST_EQUIP);
}

// An enemy turned up while a flash or smoke is out (not yet being thrown): the gun instead
bool CCSBot::LsNadeGunUp()
{
	if (!LsFight() || m_isWaitingToTossGrenade || IsUsingHEGrenade())
		return false;
	m_lsNadePhase = 0;
	EquipBestWeapon(MUST_EQUIP);
	return true;
}

void CCSBot::LsNadeUpdate()
{
	float now = gpGlobals->time;
	CBasePlayerWeapon *pWeapon = GetActiveWeapon();

	switch (m_lsNadePhase)
	{
	case 1:
	{
		// getting it out: the stock update pulls the pin as soon as it's in hand; the throw
		// waits till the pin's been out long enough for the release to throw it
		if (now - m_lsNadePhaseAt > 3.0f)
		{
			LsNadeStop();
			return;
		}
		if (!pWeapon || pWeapon->m_iId != m_lsNadeId)
		{
			if (!LsHasNade(m_lsNadeId))
				m_lsNadePhase = 0;
			return;
		}
		if (!m_isWaitingToTossGrenade && (pWeapon->m_flStartThrow <= 0.0f || now - pWeapon->m_flStartThrow < 0.55f))
			return;

		// walked on too far since it planned this (the flight, and who'd be facing a flash,
		// were worked out from there): called off; otherwise the same direction from here
		if ((pev->origin - m_lsNadeFrom).IsLengthGreaterThan(96.0f))
		{
			LsNadeStop();
			return;
		}
		UTIL_MakeVectors(m_lsNadeAng);
		m_lsNadeAim = GetEyePosition() + gpGlobals->v_forward * 300.0f;

		m_lsNadeAmmoType = pWeapon->m_iPrimaryAmmoType;
		m_lsNadeAmmo = m_rgAmmo[m_lsNadeAmmoType];
		if (!m_isWaitingToTossGrenade)
			ThrowGrenade(&m_lsNadeAim);
		m_lsNadePhase = 2;
		m_lsNadePhaseAt = now;
		return;
	}
	case 2:
	{
		// thrown once it has one fewer
		int left = LsHasNade(m_lsNadeId) ? m_rgAmmo[m_lsNadeAmmoType] : 0;
		if (left < m_lsNadeAmmo)
		{
			EquipBestWeapon(MUST_EQUIP);

			// a flash: a look away while it goes off (the careless don't bother)
			bool turns = LsSkill() >= 0.45f || LsMatch() || LsTrait(43) > 0.4f;
			if (m_lsNadeId == WEAPON_FLASHBANG && turns)
			{
				Vector away = GetEyePosition() - (m_lsNadeBurst - GetEyePosition()).Normalize() * 300.0f;
				away.z = GetEyePosition().z - 40.0f;
				ClearLookAt();
				SetLookAt("Loner-Strike: away from its flash", &away, PRIORITY_HIGH, 1.4f);
				m_lsNadePhase = 3;
				m_lsNadePhaseAt = now;
			}
			else
				m_lsNadePhase = 0;
			return;
		}
		if (now - m_lsNadePhaseAt > 4.5f || !pWeapon || pWeapon->m_iId != m_lsNadeId)
		{
			LsNadeStop();
			return;
		}
		// released before the pin had been out long enough: again
		if (!m_isWaitingToTossGrenade)
			ThrowGrenade(&m_lsNadeAim);
		return;
	}
	case 3:
		if (now - m_lsNadePhaseAt > 1.6f)
			m_lsNadePhase = 0;
		return;
	}

	if (now < m_lsNadeNextThink)
		return;
	m_lsNadeNextThink = now + RANDOM_FLOAT(0.35f, 0.6f);

	float elapsed = TheCSBots()->GetElapsedRoundTime();
	bool dumper = LsNadeDumper();
	bool dumping = dumper && elapsed < 25.0f + 10.0f * LsTrait(41);

	// a grenade in hand it isn't throwing (out in the quiet start of the round): put away
	// once that's over, unless it's one of the early dumpers
	if (IsUsingGrenade() && !m_isWaitingToTossGrenade && !dumping && !IsSafe())
	{
		EquipBestWeapon(MUST_EQUIP);
		return;
	}

	if (!HasGrenade() || now < m_lsNadeNextAt || IsAttacking() || IsEnemyVisible() || GetTimeSinceLastSawEnemy() < 1.5f
		|| IsUsingLadder() || IsBlind() || IsDefusingBomb() || GetTask() == PLANT_BOMB || GetHostageEscortCount() > 0
		|| IsActiveWeaponReloading() || m_isWaitingToTossGrenade)
		return;

	// ---- the newer players: every grenade, early, round the first corner (or just ahead)
	if (dumping)
	{
		if (elapsed < 2.0f + 4.0f * LsTrait(41))
			return;

		static const int order[] = { WEAPON_HEGRENADE, WEAPON_SMOKEGRENADE, WEAPON_FLASHBANG };
		int id = (IsUsingGrenade() && pWeapon) ? pWeapon->m_iId : 0;
		Vector target;
		if (!FindGrenadeTossPathTarget(&target))
		{
			UTIL_MakeVectors(pev->v_angle);
			target = GetEyePosition() + gpGlobals->v_forward * 500.0f + Vector(0, 0, 80.0f);
		}
		for (int k = 0; !id && k < 3; k++)
		{
			if (LsHasNade(order[k]))
				id = order[k];
		}
		// (whatever else they do, not a flash in a human teammate's face)
		if (id == WEAPON_FLASHBANG && !LsFlashSafe(target, true))
		{
			m_lsNadeNextAt = now + 2.0f;
			return;
		}
		if (id)
		{
			m_lsNadeBurst = target;
			LsNadeStart(id, target, RANDOM_FLOAT(1.5f, 4.0f));
		}
		return;
	}

	// ---- with a purpose (none in deathmatch: the hook is only there outside it)
	float use = LsNadeUse();
	int team = LsTeamSlot(m_iTeam);

	// a T smoke on the way CTs come into the site it's heading for
	int site = LsNadeSite();
	if (site >= 0 && !m_lsNadeSmokeRolled && now >= m_lsNadeRetryAt && LsHasNade(WEAPON_SMOKEGRENADE) && elapsed > 5.0f && !LsWithin(s_lsTeamNadeAt[team][LS_SMOKE], 12.0f))
	{
		// (planning simulates a lot of throws: not again for a while if this one doesn't fit)
		m_lsNadeRetryAt = now + RANDOM_FLOAT(3.0f, 5.0f);
		Vector line;
		const Vector &center = TheCSBots()->GetZone(site)->m_center;
		if (LsCtLine(site, &line))
		{
			float toLine = float((line - pev->origin).Length());
			float meToSite = float((center - pev->origin).Length2D());
			float lineToSite = float((center - line).Length2D());
			if (toLine > 350.0f && toLine < 1400.0f && meToSite > lineToSite && !TheCSBots()->IsInsideSmokeCloud(&line))
			{
				Vector aim, burst;
				if (LsPlanThrow(WEAPON_SMOKEGRENADE, line, 200.0f, &aim, &burst))
				{
					// decided once a round (a smoke there is either its job or not)
					m_lsNadeSmokeRolled = true;
					if (RANDOM_FLOAT(0.0f, 1.0f) < use)
					{
						s_lsTeamNadeAt[team][LS_SMOKE] = now;
						m_lsNadeBurst = burst;
						LsNadeStart(WEAPON_SMOKEGRENADE, aim, RANDOM_FLOAT(6.0f, 12.0f));
						return;
					}
				}
			}
		}
	}

	if (IsSafe())
		return;

	// a flash round the corner ahead, where enemies are likely (decided once a corner)
	Vector corner, beyond;
	if (LsHasNade(WEAPON_FLASHBANG) && LsNadeCorner(&corner, &beyond))
	{
		float dist = float((corner - GetEyePosition()).Length());
		bool newCorner = !m_lsNadeCornerRolled || (corner - m_lsNadeCornerAt).IsLengthGreaterThan(200.0f);
		if (newCorner && dist > 150.0f && dist < 900.0f && LsNadeHot(beyond) && !LsWithin(s_lsTeamNadeAt[team][LS_FLASH], 2.5f))
		{
			m_lsNadeCornerRolled = true;
			m_lsNadeCornerAt = corner;

			// more often with teammates to go in with (it's for them too); a match always
			// flashes in with the team
			float chance = use * (GetNearbyFriendCount() > 0 ? 0.8f : 0.45f);
			Vector aim, burst;
			bool seen = true;
			if (RANDOM_FLOAT(0.0f, 1.0f) < chance && LsPlanFlash(corner, beyond, &aim, &burst, &seen)
				&& LsFlashSafe(burst, false))
			{
				s_lsTeamNadeAt[team][LS_FLASH] = now;
				m_lsNadeBurst = burst;
				LsNadeStart(WEAPON_FLASHBANG, aim, RANDOM_FLOAT(5.0f, 10.0f));
				return;
			}
		}
	}

	// an HE at enemies heard (or just lost) round a corner
	if (LsHasNade(WEAPON_HEGRENADE) && !LsWithin(s_lsTeamNadeAt[team][LS_HE], 3.0f) && RANDOM_FLOAT(0.0f, 1.0f) < 0.3f * use)
	{
		const Vector *target = nullptr;
		if (IsNoiseHeard() && LsWithin(m_noiseTimestamp, 4.0f) && GetNoisePosition())
			target = GetNoisePosition();
		else if (GetTimeSinceLastSawEnemy() < 6.0f)
			target = &GetLastKnownEnemyPosition();

		if (target)
		{
			float dist = float((*target - pev->origin).Length());
			Vector aim, burst;
			if (dist > 350.0f && dist < 1300.0f && now >= m_lsNadeRetryAt && (m_lsNadeRetryAt = now + RANDOM_FLOAT(2.0f, 4.0f), LsPlanThrow(WEAPON_HEGRENADE, *target, 250.0f, &aim, &burst)) && LsHeSafe(this, burst))
			{
				s_lsTeamNadeAt[team][LS_HE] = now;
				m_lsNadeBurst = burst;
				LsNadeStart(WEAPON_HEGRENADE, aim, RANDOM_FLOAT(4.0f, 8.0f));
			}
		}
	}
}

// ---- smoke --------------------------------------------------------------------------------
// The stock bots can't see through smoke (IsLineBlockedBySmoke in IsVisible). On top of that:
// an enemy it was on before the smoke cut it off still gets a moment's spray, and one
// shooting at it from in there gets shot back.

void CCSBot::LsSawClear(CBasePlayer *pPlayer)
{
	if (pPlayer == GetEnemy())
		m_lsClearSawAt = gpGlobals->time;
}

// How long it keeps shooting into smoke where its enemy went: a moment for match players,
// longer for the pub sprayers
float CCSBot::LsSmokeSprayTime()
{
	if (LsMatch())
		return 0.5f + 0.5f * LsTrait(44);
	return 0.6f + 2.2f * LsTrait(44) * (1.2f - LsSkill());
}

// In sight but for smoke (FOV and walls as usual)
bool CCSBot::LsClearLine(const Vector &spot, bool testFOV)
{
	if (testFOV && !FInViewCone(&spot))
		return false;
	TraceResult tr;
	UTIL_TraceLine(GetEyePosition(), spot, ignore_monsters, ignore_glass, edict(), &tr);
	return tr.flFraction >= 1.0f;
}

bool CCSBot::LsSeeIntoSmoke(CBasePlayer *pPlayer, bool testFOV, unsigned char *visParts)
{
	if (!LsFight() || IsBlind() || !pPlayer->IsAlive())
		return false;

	float now = gpGlobals->time;

	// being shot from in there: shoot back
	bool shotBy = (m_attacker == pPlayer && GetTimeSinceAttacked() < 1.5f)
		|| (LsWithin(m_lsShotAtAt, 1.0f) && (m_lsShotFrom - pPlayer->pev->origin).IsLengthLessThan(200.0f));

	// its enemy, lost just now: a moment on them (once each time it loses them)
	bool tracked = false;
	if (pPlayer == GetEnemy())
	{
		if (m_lsSmokeLostAt < m_lsClearSawAt)
		{
			m_lsSmokeLostAt = now;
			m_lsSmokeUntil = (now - m_lsClearSawAt < 0.5f) ? now + LsSmokeSprayTime() : 0.0f;
		}
		tracked = now < m_lsSmokeUntil;
	}
	if (!shotBy && !tracked)
		return false;

	unsigned char parts = 0;
	Vector spot = pPlayer->pev->origin;
	if (LsClearLine(spot, testFOV))
		parts |= CHEST;
	spot.z += 25.0f;
	if (LsClearLine(spot, testFOV))
		parts |= HEAD;

	if (visParts)
		*visParts = parts;
	return parts != 0;
}

// ---- economy --------------------------------------------------------------------------------

enum { LS_BUY = 0, LS_SAVE = 1, LS_FORCE = 2 };

// What a rifle and armour cost each side (an AK or M4 and a vest)
static int LsDecentBuy(int team)
{
	return (team == CT) ? 3750 : 3150;
}

// The team's call for this round: buy, save or force (worked out once, by the first of
// them to buy, so the team agrees)
static int LsTeamPlan(int team)
{
	static int s_round[2] = { -1, -1 };
	static float s_at[2] = { -100.0f, -100.0f };
	static int s_plan[2] = { LS_BUY, LS_BUY };

	int slot = LsTeamSlot(team);
	int round = CSGameRules()->m_iTotalRoundsPlayed;
	if (s_round[slot] == round && LsWithin(s_at[slot], 20.0f))
		return s_plan[slot];

	int n = 0, afford = 0, money = 0;
	int decent = LsDecentBuy(team);
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!pPlayer || pPlayer->m_iTeam != team || FStrEq(STRING(pPlayer->pev->netname), ""))
			continue;
		n++;
		money += pPlayer->m_iAccount;
		CBasePlayerItem *pPrimary = pPlayer->m_rgpPlayerItems[PRIMARY_WEAPON_SLOT];
		if (pPlayer->m_iAccount >= decent || (pPrimary && pPlayer->IsAlive()))
			afford++;
	}

	int plan = LS_BUY;
	if (n > 0 && afford < 0.6f * n)
	{
		int losses = (team == CT) ? CSGameRules()->m_iNumConsecutiveCTLoses : CSGameRules()->m_iNumConsecutiveTerroristLoses;
		int avg = money / n;
		bool pistolLoss = (round == 1 && losses >= 1);
		float force = 0.0f;
		if (pistolLoss)
			force = LsMatch() ? 0.3f : 0.6f;				// the round after losing the pistols
		else if (losses >= 3 && avg >= 2000)
			force = LsMatch() ? 0.35f : 0.5f;				// enough of losing
		else if (avg >= 2600)
			force = 0.2f;
		plan = (RANDOM_FLOAT(0.0f, 1.0f) < force) ? LS_FORCE : LS_SAVE;
	}

	s_round[slot] = round;
	s_at[slot] = gpGlobals->time;
	s_plan[slot] = plan;
	return plan;
}

// A mode that hands out or limits the guns (Loner-Strike's gm_weapons: knife, pistols,
// AWP, scoutz...; or the bot cvars that keep bots to one kind): stock buying only
static bool LsWeaponMode()
{
	static cvar_t *s_weapons = nullptr;
	if (!s_weapons)
		s_weapons = CVAR_GET_POINTER("gm_weapons");
	if (s_weapons && s_weapons->value != 0.0f)
		return true;
	return cv_bot_allow_rifles.value == 0.0f || (cv_bot_allow_snipers.value == 0.0f && cv_bot_allow_pistols.value == 0.0f);
}

// A scrim (gm_scrim) runs its own team economy and calls the buys in chat: the bots follow
// that money (it caps it on an eco) rather than rolling their own save or force
static bool LsScrimEconomy()
{
	static cvar_t *s_phase = nullptr;
	if (!s_phase)
		s_phase = CVAR_GET_POINTER("gm_scrim_phase");
	return s_phase && s_phase->value > 0.0f;
}

// The pistol round (the first, or after a reset or a half: everyone on next to nothing)
static bool LsPistolRound()
{
	if (CSGameRules()->m_iTotalRoundsPlayed == 0)
		return true;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (pPlayer && (pPlayer->m_iTeam == CT || pPlayer->m_iTeam == TERRORIST) && pPlayer->m_iAccount > 1000)
			return false;
	}
	return true;
}

// BuyState::OnEnter: save, force or the usual buy this round
void CCSBot::LsPlanBuy()
{
	m_lsBuyPlan = LS_BUY;
	if (!LsFight() || !LsTeamGame() || !CSGameRules() || !TheCSBots() || LsWeaponMode() || LsScrimEconomy())
		return;

	// the pistol round, or still holding a gun from last round: the usual
	if (LsPistolRound() || m_bHasPrimary)
		return;

	int decent = LsDecentBuy(m_iTeam);
	int teamPlan = LsTeamPlan(m_iTeam);

	if (m_iAccount >= decent)
	{
		// on a team save a match player saves too unless there's plenty; in a pub whoever
		// has the money buys
		if (teamPlan == LS_SAVE && LsMatch() && m_iAccount < decent + 2500)
			m_lsBuyPlan = LS_SAVE;
		return;
	}

	if (teamPlan == LS_FORCE)
		m_lsBuyPlan = LS_FORCE;
	else if (teamPlan == LS_BUY)
		// the team's buying but it can't: something cheaper, or a save if there's next to nothing
		m_lsBuyPlan = (m_iAccount >= (LsMatch() ? 2000 : 1500)) ? LS_FORCE : LS_SAVE;
	else if (LsMatch())
		m_lsBuyPlan = LS_SAVE;
	else
	{
		// a pub: some always buy whatever they can (the stock buy does exactly that)
		float skill = LsSkill();
		bool spender = LsTrait(45) < 0.3f || (skill < 0.4f && LsTrait(45) < 0.55f);
		m_lsBuyPlan = spender ? LS_BUY : LS_SAVE;
	}
}

// A save round: nothing, or a better pistol for some when there's money for it
void CCSBot::LsBuyEco()
{
	CBasePlayerItem *pPistol = m_rgpPlayerItems[PISTOL_SLOT];
	bool stockPistol = !pPistol || pPistol->m_iId == WEAPON_GLOCK18 || pPistol->m_iId == WEAPON_USP;
	if (TheCSBots()->AllowPistols() && stockPistol && m_iAccount >= 1400 && LsTrait(46) < (LsMatch() ? 0.25f : 0.45f))
		ClientCommand("deagle");
	PrintIfWatched("Saving this round.\n");
}

// A force buy with its own gun out of reach: the dearest cheaper gun that still leaves
// money for a vest (or just the dearest it can have)
void CCSBot::LsBuyForce()
{
	if (LsWeaponMode())
		return;
	struct Gun { const char *alias; int cost; int team; int kind; };	// kind: 0 rifle, 1 SMG, 2 shotgun
	static const Gun guns[] =
	{
		{ "famas", 2250, CT, 0 }, { "galil", 2000, TERRORIST, 0 },
		{ "ump45", 1700, 0, 1 }, { "m3", 1700, 0, 2 }, { "mp5", 1500, 0, 1 },
		{ "mac10", 1400, TERRORIST, 1 }, { "tmp", 1250, CT, 1 },
	};

	for (int pass = 0; pass < 2 && !m_bHasPrimary; pass++)
	{
		int budget = m_iAccount - (pass == 0 ? 650 : 0);
		for (const Gun &g : guns)
		{
			if (g.team && g.team != m_iTeam)
				continue;
			if (g.cost > budget)
				continue;
			if ((g.kind == 0 && !TheCSBots()->AllowRifles()) || (g.kind == 1 && !TheCSBots()->AllowSubMachineGuns())
				|| (g.kind == 2 && !TheCSBots()->AllowShotguns()))
				continue;
			// (some skip the first pick: not every force buy is the same gun)
			if (RANDOM_FLOAT(0.0f, 1.0f) < 0.25f)
				continue;
			ClientCommand(g.alias);
			PrintIfWatched("Force buy: %s.\n", g.alias);
			if (m_bHasPrimary)
				break;
		}
	}
}

// Grenades and the defuse kit, instead of the stock random picks
void CCSBot::LsBuyExtras()
{
	if (m_lsBuyPlan == LS_SAVE || LsWeaponMode())
		return;

	// a kit for each site among the CTs: not everyone, and not no one
	if (m_iTeam == CT && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB && !CSGameRules()->IsCareer() && !m_bHasDefuser)
	{
		int kits = 0, cts = 0;
		for (int i = 1; i <= gpGlobals->maxClients; i++)
		{
			CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
			if (!pPlayer || pPlayer->m_iTeam != CT || !pPlayer->IsAlive())
				continue;
			cts++;
			if (pPlayer->m_bHasDefuser)
				kits++;
		}
		int want = Q_max(1, TheCSBots()->GetZoneCount());
		if (LsMatch() && cts >= 4)
			want++;
		want = Q_min(want, cts);

		bool buy;
		if (kits < want)
			buy = LsMatch() || RANDOM_FLOAT(0.0f, 1.0f) < 0.85f;		// (in a pub, someone forgets)
		else
			buy = !LsMatch() && m_iAccount > 5000 && LsTrait(47) < 0.25f;	// the one who always has one
		if (buy)
			ClientCommand("defuser");
	}

	if (!TheCSBots()->AllowGrenades())
		return;

	const char *list[5];
	int n = 0;
	float skill = LsSkill();

	if (LsNadeDumper())
	{
		// the lot, in whatever order
		const char *all[] = { "hegren", "flash", "flash", "sgren" };
		int start = RANDOM_LONG(0, 3);
		for (int k = 0; k < 4; k++)
		{
			if (RANDOM_FLOAT(0.0f, 1.0f) < 0.75f)
				list[n++] = all[(start + k) % 4];
		}
	}
	else
	{
		float user = LsMatch() ? 1.0f : Q_min(1.0f, 0.3f + 0.5f * skill + 0.4f * (LsTrait(42) - 0.5f));
		if (m_iTeam == TERRORIST)
		{
			if (user > 0.45f) list[n++] = "sgren";
			list[n++] = "flash";
			if (user > 0.6f) list[n++] = "flash";
			list[n++] = "hegren";
		}
		else
		{
			list[n++] = "flash";
			list[n++] = "hegren";
			if (user > 0.6f) list[n++] = "sgren";
			if (user > 0.75f) list[n++] = "flash";
		}
		// some never bother with them; the first is the one they'd have
		int keep = 0;
		for (int k = 0; k < n; k++)
		{
			if ((k == 0 && user >= 0.3f) || RANDOM_FLOAT(0.0f, 1.0f) < user)
				list[keep++] = list[k];
		}
		n = keep;
	}

	// a force buy: one at most
	if (m_lsBuyPlan == LS_FORCE)
		n = Q_min(n, 1);

	for (int k = 0; k < n; k++)
		ClientCommand(list[k]);
}

// ---- keeping the gun in a lost round --------------------------------------------------------

// Alone against three or more late in a round its side is losing (a T with no bomb down,
// a CT with it ticking, a CT on a hostage map): hides and keeps its rifle or AWP for next
// round instead of throwing it away. A match player nearly always does; in a pub it's the
// better players, and a few heroes never
void CCSBot::LsSaveCheck()
{
	if (LsWeaponMode())
		return;
	float now = gpGlobals->time;
	if (now < m_lsSaveNext || !TheCSBots() || !CSGameRules())
		return;
	m_lsSaveNext = now + RANDOM_FLOAT(0.7f, 1.2f);

	bool bombMap = TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_DEFUSE_BOMB;
	bool planted = TheCSBots()->IsBombPlanted();

	if (m_lsSaving)
	{
		// the odds evened up: back to playing
		if (GetFriendsRemaining() > 0 || GetEnemiesRemaining() < 2)
		{
			m_lsSaving = false;
			SetDisposition(ENGAGE_AND_INVESTIGATE);
			if (IsHiding())
				Idle();
			return;
		}
		if (IsAttacking() || IsDefusingBomb())
			return;

		// away from the bomb, or somewhere out of the way, only shooting if it must
		const Vector *bomb = planted ? GetGameState()->GetBombPosition() : nullptr;
		if (bomb && m_iTeam == CT && (*bomb - pev->origin).IsLengthLessThan(1500.0f))
		{
			if (!IsEscapingFromBomb())
				EscapeFromBomb();
		}
		else if (!IsHiding())
			Hide(nullptr, CSGameRules()->GetRoundRemainingTime() + 30.0f, 1500.0f, true);
		SetDisposition(SELF_DEFENSE);
		return;
	}

	if (m_lsSaveDecided || GetFriendsRemaining() > 0 || GetEnemiesRemaining() < 3 || IsDefusingBomb() || GetHostageEscortCount() > 0)
		return;

	// a gun worth keeping
	CBasePlayerItem *pPrimary = m_rgpPlayerItems[PRIMARY_WEAPON_SLOT];
	if (!pPrimary)
		return;
	WeaponClassType kind = WeaponIDToWeaponClass(pPrimary->m_iId);
	if (kind != WEAPONCLASS_RIFLE && kind != WEAPONCLASS_SNIPERRIFLE)
		return;

	// its side losing
	bool losing;
	if (bombMap)
		losing = (m_iTeam == TERRORIST) ? !planted : planted;
	else
		losing = (m_iTeam == CT && TheCSBots()->GetScenario() == CCSBotManager::SCENARIO_RESCUE_HOSTAGES);
	if (!losing)
		return;

	// and late
	if (!planted && CSGameRules()->GetRoundRemainingTime() > 60.0f)
		return;

	m_lsSaveDecided = true;
	float skill = LsSkill();
	float chance = LsMatch() ? 0.9f : 0.25f + 0.6f * skill;
	if (LsTrait(48) > 0.85f)
		chance *= 0.15f;
	if (RANDOM_FLOAT(0.0f, 1.0f) >= chance)
		return;

	m_lsSaving = true;
	m_lsSaveNext = now;
	LsWhy("saving its gun");
	PrintIfWatched("Alone against too many: saving my gun.\n");
}
