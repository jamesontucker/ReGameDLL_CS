/*
*
*   Loner-Strike additions to the CZ bots: Deathrun. The brain plays it as people do, instead of the
*   plugins steering bots by velocity (gm_botnav) and pressing buttons from anywhere (dr_bot_traps).
*
*   bot_ls_deathrun 1 on a deathrun map (deathrun_* / dr_*; 0, the default: the plugins drive):
*   - runners (CTs) run the course with their own movement: nav paths toward the course end (the
*     place farthest from the CT start), real jumps over gaps the nav links across, ladders, the
*     narrow-footing rules on beams. They leave a moment apart, each at its own pace; the careful
*     ones wait short of a trap while the terrorist stands by its button, and everyone remembers
*     where people died this map. A floor that isn't there yet (a platform away, a trap floor
*     open) is waited for at the edge. At the end, the plain brain takes over: it hunts the
*     terrorist.
*   - the activator (a bot terrorist) walks to the trap buttons and presses them with use while
*     looking at them, the way a player does: it notices runners late or early, has a reaction
*     time, times traps a bit early or late, holds one for a group when it's greedy, lets a lone
*     runner by to save it, hesitates now and then, never uses a spent trap again in the round,
*     keeps ahead of the runners along the buttons, and sometimes gives the traps up to go and
*     fight when the runners are nearly through.
*
*   bot_ls_nav_course (1 deathrun maps, 2 any map, 0 off): nav learning for obstacle courses - door
*   and platform floors are floors, gaps a running jump clears are linked across (and what's past
*   them learned), trigger_teleport destinations are learned too. Learning only: a nav that's
*   already there isn't changed.
*
*   ls_dr: what the brain knows about this map's course and traps (server console).
*
*/

#include "precompiled.h"
#include <vector>
#include <algorithm>
#include <functional>

cvar_t cv_bot_ls_deathrun = { "bot_ls_deathrun", "0", FCVAR_SERVER, 0.0f, nullptr };
cvar_t cv_bot_ls_nav_course = { "bot_ls_nav_course", "1", FCVAR_SERVER, 1.0f, nullptr };

// A deathrun map by its name (the plugins' own test: "deathrun" anywhere, or a dr_ prefix)
static bool LsDrMapName()
{
	const char *map = STRING(gpGlobals->mapname);
	if (!map)
		return false;
	if (!Q_strnicmp(map, "dr_", 3))
		return true;
	for (const char *p = map; *p; p++)
		if (!Q_strnicmp(p, "deathrun", 8))
			return true;
	return false;
}

bool LsNavCourse()
{
	float v = cv_bot_ls_nav_course.value;
	return v >= 2.0f || (v >= 1.0f && LsDrMapName());
}

bool LsDrOn()
{
	return cv_bot_ls_deathrun.value > 0.0f && LsDrMapName() && !TheNavAreaList.empty();
}

// ---- nav learning for obstacle courses --------------------------------------------------------

// A door that's a floor: a trap floor that slides or swings away, a bridge that drops (the
// ground search walks through doors - a door's top edge isn't somewhere to stand - so a course
// built on them was a pit to it). Wide both ways (a door standing up is a few units thick), and
// the trace met its top
bool LsMoverFloor(const TraceResult &result)
{
	if (!result.pHit || result.vecPlaneNormal.z < 0.7f || !LsNavCourse())
		return false;
	entvars_t *pevHit = VARS(result.pHit);
	if (!FClassnameIs(pevHit, "func_door") && !FClassnameIs(pevHit, "func_door_rotating"))
		return false;
	Vector size = pevHit->absmax - pevHit->absmin;
	if (size.x < 40.0f || size.y < 40.0f)
		return false;
	return result.vecEndPos.z >= pevHit->absmax.z - 4.0f;
}

namespace LsDr
{

struct Box { Vector lo, hi; };

// A trigger_hurt that's always on and kills (or near enough): a pit's floor, a laser grid that
// never stops. Switched ones (a name, or starting off) are traps, not the map
static void DeadlyHurts(std::vector<Box> &out)
{
	out.clear();
	CBaseEntity *pEnt = nullptr;
	while ((pEnt = UTIL_FindEntityByClassname(pEnt, "trigger_hurt")) != nullptr)
	{
		if (pEnt->pev->dmg < 50.0f || (pEnt->pev->spawnflags & 2) || !FStringNull(pEnt->pev->targetname))
			continue;
		Box b;
		b.lo = pEnt->pev->absmin;
		b.hi = pEnt->pev->absmax;
		out.push_back(b);
	}
}

// A player standing on this floor spot is in one (feet to head overlapping the trigger)
static bool DeadlyAt(const std::vector<Box> &boxes, const Vector &floor)
{
	for (const Box &b : boxes)
		if (floor.x >= b.lo.x && floor.x <= b.hi.x && floor.y >= b.lo.y && floor.y <= b.hi.y && floor.z <= b.hi.z + 1.0f && floor.z + HumanHeight >= b.lo.z)
			return true;
	return false;
}

static bool InBoxes(const std::vector<Box> &boxes, const Vector &p, float pad = 0.0f)
{
	for (const Box &b : boxes)
		if (p.x >= b.lo.x - pad && p.x <= b.hi.x + pad && p.y >= b.lo.y - pad && p.y <= b.hi.y + pad && p.z >= b.lo.z - pad && p.z <= b.hi.z + pad)
			return true;
	return false;
}

// ---- learning --------------------------------------------------------------------------------

struct Link { Vector from, to; int dir; };
static std::vector<Link> s_learnLinks;			// gaps found while learning (linked when the areas are made)
static std::vector<unsigned char> s_scanned;	// node id -> directions looked across for a gap
static std::vector<Box> s_learnHurts;
static std::vector<int> s_teleSeeded;			// trigger_teleports whose destination has been seeded
static int s_gapSeeds = 0, s_teleSeeds = 0;

// The ground under a point, by a trace down from it (doors that are floors count)
static bool GroundBelow(const Vector &from, float depth, float *z, Vector *normal)
{
	TraceResult tr;
	Vector to = from - Vector(0, 0, depth);
	edict_t *ignore = nullptr;
	Vector start = from;
	for (int tries = 0; tries < 4; tries++)
	{
		UTIL_TraceLine(start, to, ignore_monsters, dont_ignore_glass, ignore, &tr);
		if (tr.fStartSolid || tr.flFraction >= 1.0f)
			return false;
		if (tr.pHit && IsEntityWalkable(VARS(tr.pHit), WALK_THRU_DOORS | WALK_THRU_BREAKABLES) && !LsMoverFloor(tr))
		{
			ignore = tr.pHit;
			start = tr.vecEndPos - Vector(0, 0, 1.0f);
			continue;
		}
		*z = tr.vecEndPos.z;
		*normal = tr.vecPlaneNormal;
		return true;
	}
	return false;
}

// A running jump from a node's edge one way: the nearest grid point it lands on, if any.
// The jump: 240 units/s along (a knife or pistol run, a little short of 250), 268 up (45 units
// high), feet tucked 18 more for a landing higher up (a duck-jump); a hull that fits all the way;
// a floor to land on that's flat enough, not a deadly trigger, not a fall that hurts much
static bool GapJumpAt(const Vector &p, int dir, bool runup, Vector *land, Vector *landNormal, bool say)
{
	Vector2D d;
	DirectionToVector2D((NavDirType)dir, &d);

	// room to stand where it takes off, and a step behind it to run up from
	TraceResult tr;
	Vector stand(p.x, p.y, p.z + HalfHumanHeight + 1.0f);
	UTIL_TraceHull(stand, stand, ignore_monsters, human_hull, nullptr, &tr);
	if (tr.fStartSolid || tr.fAllSolid)
	{
		if (say)
			CONSOLE_ECHO("  no room to stand at the takeoff\n");
		return false;
	}
	if (!runup)
	{
		if (say)
			CONSOLE_ECHO("  no step behind to run up from\n");
		return false;
	}

	const float vh = 240.0f, vz = 268.0f, g = 800.0f;
	for (float L = 2.0f * GenerationStepSize; L <= 9.0f * GenerationStepSize + 0.1f; L += GenerationStepSize)
	{
		Vector col(p.x + d.x * L, p.y + d.y * L, p.z + 64.0f);
		float gz;
		Vector n;
		if (!GroundBelow(col, 64.0f + 200.0f, &gz, &n) || n.z < 0.7f)
		{
			if (say)
				CONSOLE_ECHO("  %.0f: no floor\n", L);
			continue;
		}
		float dz = gz - p.z;
		// (a step it could just walk onto isn't a gap: the learner would have walked it)
		if (L <= 2.0f * GenerationStepSize + 0.1f && Q_fabs(dz) < StepHeight)
		{
			float midZ;
			Vector midN;
			Vector mid(p.x + d.x * GenerationStepSize, p.y + d.y * GenerationStepSize, p.z + 32.0f);
			if (GroundBelow(mid, 32.0f + 60.0f, &midZ, &midN))
			{
				if (say)
					CONSOLE_ECHO("  %.0f: floor in between (walkable)\n", L);
				continue;
			}
		}
		// flight: takes off at the node's edge (half a step out), lands a little way in
		float flight = L - 0.5f * GenerationStepSize + 8.0f;
		float t = flight / vh;
		float zAt = vz * t - 0.5f * g * t * t;
		bool duck = false;
		if (zAt < dz + 2.0f)
		{
			if (zAt + 18.0f < dz + 2.0f)
			{
				if (say)
					CONSOLE_ECHO("  %.0f: too high (%.0f up)\n", L, dz);
				continue;
			}
			duck = true;
		}
		if (dz < -180.0f)
			continue;
		// the landing has room for a standing player, and isn't deadly
		Vector at(col.x, col.y, gz + HalfHumanHeight + 1.0f);
		UTIL_TraceHull(at, at, ignore_monsters, human_hull, nullptr, &tr);
		if (tr.fStartSolid || tr.fAllSolid)
		{
			if (say)
				CONSOLE_ECHO("  %.0f: no room to land (floor %.0f)\n", L, gz);
			continue;
		}
		if (DeadlyAt(s_learnHurts, Vector(col.x, col.y, gz)))
		{
			if (say)
				CONSOLE_ECHO("  %.0f: deadly landing\n", L);
			continue;
		}
		// the arc, in short straight pieces, with the hull it has in the air (tucked: the short one)
		bool clear = true;
		// (the hull's centre is 37 above the feet either way: standing, or tucked 18 up in the short hull)
		const float centre = HalfHumanHeight + 1.0f;
		Vector prev(p.x + d.x * 0.5f * GenerationStepSize, p.y + d.y * 0.5f * GenerationStepSize, p.z + centre);
		const int pieces = 6;
		for (int k = 1; k <= pieces && clear; k++)
		{
			float tk = t * float(k) / float(pieces);
			float zk = vz * tk - 0.5f * g * tk * tk;
			if (k == pieces)
				zk = Q_max(zk, dz);
			Vector cur(p.x + d.x * (0.5f * GenerationStepSize + vh * tk), p.y + d.y * (0.5f * GenerationStepSize + vh * tk), p.z + zk + centre);
			UTIL_TraceHull(prev, cur, ignore_monsters, duck ? head_hull : human_hull, nullptr, &tr);
			if (tr.fStartSolid || tr.flFraction < 1.0f)
			{
				clear = false;
				if (say)
					CONSOLE_ECHO("  %.0f: the arc hits %s at piece %d (%.0f %.0f %.0f)\n", L, tr.pHit ? STRING(tr.pHit->v.classname) : "?", k, tr.vecEndPos.x, tr.vecEndPos.y, tr.vecEndPos.z);
			}
			prev = cur;
		}
		if (!clear)
			continue;
		*land = Vector(col.x, col.y, gz);
		*landNormal = n;
		if (say)
			CONSOLE_ECHO("  %.0f: lands at %.0f %.0f %.0f%s\n", L, col.x, col.y, gz, duck ? " (duck-jump)" : "");
		return true;
	}
	return false;
}

static bool GapJump(const CNavNode *node, int dir, Vector *land, Vector *landNormal)
{
	return GapJumpAt(*node->GetPosition(), dir, node->GetConnectedNode(OppositeDirection((NavDirType)dir)) != nullptr, land, landNormal, false);
}

} // namespace LsDr

void LsCourseLearnStart()
{
	using namespace LsDr;
	s_learnLinks.clear();
	s_scanned.clear();
	s_teleSeeded.clear();
	s_gapSeeds = s_teleSeeds = 0;
	if (LsNavCourse())
		DeadlyHurts(s_learnHurts);
}

// The search ran out: more to learn past a gap a running jump clears, or where a teleport goes
CNavNode *LsCourseSeedSearch()
{
	using namespace LsDr;
	if (!LsNavCourse())
		return nullptr;

	// where each trigger_teleport sends you (once each)
	CBaseEntity *pTele = nullptr;
	while ((pTele = UTIL_FindEntityByClassname(pTele, "trigger_teleport")) != nullptr)
	{
		int idx = pTele->entindex();
		if (std::find(s_teleSeeded.begin(), s_teleSeeded.end(), idx) != s_teleSeeded.end())
			continue;
		s_teleSeeded.push_back(idx);
		if (FStringNull(pTele->pev->target))
			continue;
		CBaseEntity *pDest = UTIL_FindEntityByTargetname(nullptr, STRING(pTele->pev->target));
		if (!pDest)
			continue;
		Vector pos = pDest->pev->origin;
		pos.x = floorf(pos.x / GenerationStepSize + 0.5f) * GenerationStepSize;
		pos.y = floorf(pos.y / GenerationStepSize + 0.5f) * GenerationStepSize;
		float gz;
		Vector n;
		if (!GroundBelow(pos + Vector(0, 0, 8.0f), 400.0f, &gz, &n) || n.z < 0.7f)
			continue;
		pos.z = gz;
		if (CNavNode::GetNode(&pos) || TheNavAreaGrid.GetNavArea(&pos, HumanHeight))
			continue;
		s_teleSeeds++;
		return new CNavNode(&pos, &n, nullptr);
	}

	// across gaps: every node with no way on (or a big step down) one way gets a look across
	for (CNavNode *node = CNavNode::GetFirst(); node; node = node->GetNext())
	{
		unsigned int id = node->GetID();
		if (id >= s_scanned.size())
			s_scanned.resize(id + 1024, 0);
		if (s_scanned[id] == 0x0f)
			continue;
		for (int dir = 0; dir < NUM_DIRECTIONS; dir++)
		{
			if (s_scanned[id] & (1 << dir))
				continue;
			s_scanned[id] |= (1 << dir);
			const CNavNode *adj = node->GetConnectedNode((NavDirType)dir);
			if (adj && adj->GetPosition()->z > node->GetPosition()->z - 60.0f)
				continue;
			if (DeadlyAt(s_learnHurts, *node->GetPosition()))
				continue;
			Vector land, n;
			if (!GapJump(node, dir, &land, &n))
				continue;
			Link link;
			link.from = *node->GetPosition();
			link.to = land;
			link.dir = dir;
			const CNavNode *there = CNavNode::GetNode(&land);
			if (there)
			{
				// (already learned: just the link, unless it's the next node along anyway)
				if (there != adj)
					s_learnLinks.push_back(link);
				continue;
			}
			if (TheNavAreaGrid.GetNavArea(&land, HumanHeight))
				continue;
			s_learnLinks.push_back(link);
			s_gapSeeds++;
			return new CNavNode(&land, &n, nullptr);
		}
	}
	return nullptr;
}

// The areas are made: the gaps found link the areas on both sides (one way: the jump back is
// found on its own, if there is one)
void LsCourseLinkAreas()
{
	using namespace LsDr;
	if (!LsNavCourse())
		return;
	int made = 0, noArea = 0, same = 0, already = 0;
	for (const Link &l : s_learnLinks)
	{
		Vector a = l.from + Vector(0, 0, 8.0f), b = l.to + Vector(0, 0, 8.0f);
		CNavArea *from = TheNavAreaGrid.GetNavArea(&a);
		CNavArea *to = TheNavAreaGrid.GetNavArea(&b);
		if (!from || !to)
		{
			noArea++;
			continue;
		}
		if (from == to)
		{
			same++;
			continue;
		}
		if (from->IsConnected(to, NUM_DIRECTIONS))
		{
			already++;
			continue;
		}
		from->ConnectTo(to, (NavDirType)l.dir);
		made++;
	}
	CONSOLE_ECHO("Loner-Strike: course learning: %d places past gaps, %d teleport destinations, %d gaps found, %d linked (%d no area, %d same area, %d already linked); %d nodes looked across\n",
		s_gapSeeds, s_teleSeeds, int(s_learnLinks.size()), made, noArea, same, already, int(std::count_if(s_scanned.begin(), s_scanned.end(), [](unsigned char c) { return c != 0; })));
	s_learnLinks.clear();
	s_scanned.clear();
}

// ---- the course, the traps and what's been learned this map ------------------------------------

namespace LsDr
{

const int MAX_PARTS = 12;

struct Trap
{
	EntityHandle<CBaseEntity> button;
	Vector btn;							// the button's middle
	Vector spot;						// where the activator stands to press it (on the nav)
	int spotIdx = -1;					// ...its area
	bool reachT = false;				// the terrorist's side of the map gets there
	Box parts[MAX_PARTS];				// what it moves, breaks or switches on (and where a door goes)
	int partCount = 0;
	Vector mid;							// the middle of all that
	float progress = -1.0f;				// how far along the course it is (walking from the CT start)
	bool usedThisRound = false;
	int kills = 0;						// runners it got this map
};

struct Tele { int from, to; Vector at; };

struct Death { Vector at; int count; bool trap; float when; };

static char s_map[64] = "";
static int s_navLoads = -1;
static const void *s_navFront = nullptr;
static size_t s_navSize = 0;
static bool s_ready = false;

static std::vector<CNavArea *> s_areas;
static std::vector<int> s_idx;				// area id -> index (-1 none)
static std::vector<std::vector<int>> s_out;	// index -> areas it leads to
static std::vector<float> s_prog;			// walking distance from the CT start (-1 unreached)
static std::vector<float> s_progT;			// ...from the T start
static std::vector<float> s_toEnd;			// to the course end
static std::vector<int> s_next;				// the next area toward the end
static std::vector<char> s_deadly;
static std::vector<Box> s_hurts;
static std::vector<Trap> s_traps;
static std::vector<Tele> s_teles;
static std::vector<Death> s_deaths;
static int s_endIdx = -1, s_startIdx = -1, s_tStartIdx = -1;
static Vector s_end;
static float s_course = 0.0f;
static float s_lastPressAt = -99.0f;		// a trap button went down (anyone)
static float s_roundStart = -1.0f;

static int Index(const CNavArea *area)
{
	if (!area)
		return -1;
	unsigned int id = area->GetID();
	return id < s_idx.size() ? s_idx[id] : -1;
}

static float Dist(int a, int b)
{
	return (*s_areas[a]->GetCenter() - *s_areas[b]->GetCenter()).Length();
}

static CNavArea *AreaAt(const Vector &pos, float below = 400.0f)
{
	CNavArea *area = TheNavAreaGrid.GetNavArea(&pos, below);
	if (!area)
		area = TheNavAreaGrid.GetNearestNavArea(&pos);
	return area;
}

// Dijkstra from one area (forward, or over the links backwards), deadly areas left out
static void Flood(int from, bool backwards, std::vector<float> &dist, std::vector<int> *parent)
{
	size_t n = s_areas.size();
	dist.assign(n, -1.0f);
	if (parent)
		parent->assign(n, -1);
	if (from < 0)
		return;
	std::vector<std::vector<int>> in;
	if (backwards)
	{
		in.resize(n);
		for (size_t i = 0; i < n; i++)
			for (int j : s_out[i])
				in[j].push_back(int(i));
	}
	const std::vector<std::vector<int>> &adj = backwards ? in : s_out;
	typedef std::pair<float, int> QI;
	std::vector<QI> heap;
	dist[from] = 0.0f;
	heap.push_back(QI(0.0f, from));
	while (!heap.empty())
	{
		std::pop_heap(heap.begin(), heap.end(), std::greater<QI>());
		QI top = heap.back();
		heap.pop_back();
		int i = top.second;
		if (top.first > dist[i])
			continue;
		for (int j : adj[i])
		{
			if (s_deadly[j])
				continue;
			float nd = top.first + Dist(i, j);
			if (dist[j] < 0.0f || nd < dist[j])
			{
				dist[j] = nd;
				if (parent)
					(*parent)[j] = i;
				heap.push_back(QI(nd, j));
				std::push_heap(heap.begin(), heap.end(), std::greater<QI>());
			}
		}
	}
}

static void AddBox(Trap &t, const Vector &lo, const Vector &hi)
{
	if (t.partCount >= MAX_PARTS)
		return;
	t.parts[t.partCount].lo = lo;
	t.parts[t.partCount].hi = hi;
	t.partCount++;
}

// What a button's target sets off, through multi_managers and relays (as dr_bot_traps does it)
static void Resolve(Trap &t, const char *name, int depth)
{
	if (!name || !name[0] || depth > 4 || t.partCount >= MAX_PARTS)
		return;
	CBaseEntity *pEnt = nullptr;
	int guard = 0;
	while ((pEnt = UTIL_FindEntityByTargetname(pEnt, name)) != nullptr && guard++ < 32)
	{
		if (FClassnameIs(pEnt->pev, "func_button") || FClassnameIs(pEnt->pev, "func_rot_button") || pEnt->IsPlayer())
			continue;
		if (FClassnameIs(pEnt->pev, "multi_manager"))
		{
			CMultiManager *pMM = static_cast<CMultiManager *>(pEnt);
			for (int i = 0; i < pMM->m_cTargets && i < MAX_MM_TARGETS; i++)
			{
				const char *one = STRING(pMM->m_iTargetName[i]);
				if (one && one[0] && Q_strcmp(one, name))
					Resolve(t, one, depth + 1);
			}
			continue;
		}
		const char *model = STRING(pEnt->pev->model);
		if (model && model[0] == '*')
		{
			Vector lo = pEnt->pev->absmin, hi = pEnt->pev->absmax;
			AddBox(t, lo, hi);
			// (a door: where it goes too)
			if (FClassnameIs(pEnt->pev, "func_door") || FClassnameIs(pEnt->pev, "func_water"))
			{
				CBaseToggle *pDoor = static_cast<CBaseToggle *>(pEnt);
				Vector move = pDoor->m_vecPosition2 - pDoor->m_vecPosition1;
				if (move.Length() > 8.0f && move.Length() < 2048.0f)
					AddBox(t, lo + move, hi + move);
			}
		}
		const char *target = STRING(pEnt->pev->target);
		if (target && target[0] && Q_strcmp(target, name)
			&& (FClassnameIs(pEnt->pev, "trigger_relay") || FClassnameIs(pEnt->pev, "multisource") || FClassnameIs(pEnt->pev, "env_render")))
			Resolve(t, target, depth + 1);
	}
}

static bool InTrap(const Trap &t, const Vector &p, float pad)
{
	for (int i = 0; i < t.partCount; i++)
	{
		const Box &b = t.parts[i];
		if (p.x >= b.lo.x - pad && p.x <= b.hi.x + pad && p.y >= b.lo.y - pad && p.y <= b.hi.y + pad
			&& p.z >= b.lo.z - pad - 72.0f && p.z <= b.hi.z + pad + 72.0f)
			return true;
	}
	return false;
}

static void Build()
{
	s_ready = false;
	s_areas.clear();
	s_idx.clear();
	s_out.clear();
	s_traps.clear();
	s_teles.clear();
	s_endIdx = s_startIdx = s_tStartIdx = -1;
	s_course = 0.0f;

	unsigned int maxId = 0;
	for (CNavArea *area : TheNavAreaList)
	{
		maxId = Q_max(maxId, area->GetID());
		s_areas.push_back(area);
	}
	size_t n = s_areas.size();
	if (!n)
		return;
	s_idx.assign(maxId + 1, -1);
	for (size_t i = 0; i < n; i++)
		s_idx[s_areas[i]->GetID()] = int(i);
	s_out.assign(n, std::vector<int>());
	for (size_t i = 0; i < n; i++)
	{
		CNavArea *area = s_areas[i];
		for (int dir = 0; dir < NUM_DIRECTIONS; dir++)
			for (int k = 0; k < area->GetAdjacentCount((NavDirType)dir); k++)
			{
				int j = Index(area->GetAdjacentArea((NavDirType)dir, k));
				if (j >= 0)
					s_out[i].push_back(j);
			}
		const NavLadderList *up = area->GetLadderList(LADDER_UP);
		for (NavLadderList::const_iterator it = up->begin(); it != up->end(); it++)
		{
			const CNavLadder *l = *it;
			CNavArea *tops[4] = { l->m_topForwardArea, l->m_topLeftArea, l->m_topRightArea, l->m_topBehindArea };
			for (int k = 0; k < 4; k++)
				if (Index(tops[k]) >= 0)
					s_out[i].push_back(Index(tops[k]));
		}
		const NavLadderList *down = area->GetLadderList(LADDER_DOWN);
		for (NavLadderList::const_iterator it = down->begin(); it != down->end(); it++)
			if (Index((*it)->m_bottomArea) >= 0)
				s_out[i].push_back(Index((*it)->m_bottomArea));
	}

	// teleports: from the areas under the trigger to where it sends you (a link in the nav too,
	// so paths go through them)
	CBaseEntity *pTele = nullptr;
	while ((pTele = UTIL_FindEntityByClassname(pTele, "trigger_teleport")) != nullptr)
	{
		if (FStringNull(pTele->pev->target))
			continue;
		CBaseEntity *pDest = UTIL_FindEntityByTargetname(nullptr, STRING(pTele->pev->target));
		if (!pDest)
			continue;
		Vector dest = pDest->pev->origin;
		int to = Index(AreaAt(dest));
		if (to < 0)
			continue;
		Vector lo = pTele->pev->absmin, hi = pTele->pev->absmax;
		int linked = 0;
		for (size_t i = 0; i < n && linked < 16; i++)
		{
			const Extent *e = s_areas[i]->GetExtent();
			const Vector *c = s_areas[i]->GetCenter();
			if (e->hi.x < lo.x || e->lo.x > hi.x || e->hi.y < lo.y || e->lo.y > hi.y || c->z < lo.z - 72.0f || c->z > hi.z)
				continue;
			if (int(i) == to)
				continue;
			Vector at((Q_max(e->lo.x, lo.x) + Q_min(e->hi.x, hi.x)) * 0.5f, (Q_max(e->lo.y, lo.y) + Q_min(e->hi.y, hi.y)) * 0.5f, c->z);
			Vector d = *s_areas[to]->GetCenter() - at;
			NavDirType dir = Q_fabs(d.x) > Q_fabs(d.y) ? (d.x > 0 ? EAST : WEST) : (d.y > 0 ? SOUTH : NORTH);
			s_areas[i]->ConnectTo(s_areas[to], dir);
			s_out[i].push_back(to);
			Tele tl;
			tl.from = int(i);
			tl.to = to;
			tl.at = at;
			s_teles.push_back(tl);
			linked++;
		}
	}

	DeadlyHurts(s_hurts);
	s_deadly.assign(n, 0);
	for (size_t i = 0; i < n; i++)
		s_deadly[i] = DeadlyAt(s_hurts, *s_areas[i]->GetCenter()) ? 1 : 0;

	CBaseEntity *pStart = UTIL_FindEntityByClassname(nullptr, "info_player_start");
	CBaseEntity *pTStart = UTIL_FindEntityByClassname(nullptr, "info_player_deathmatch");
	if (pStart)
		s_startIdx = Index(AreaAt(pStart->pev->origin, 450.0f));
	if (pTStart)
		s_tStartIdx = Index(AreaAt(pTStart->pev->origin, 450.0f));
	Flood(s_startIdx, false, s_prog, nullptr);
	Flood(s_tStartIdx, false, s_progT, nullptr);
	for (size_t i = 0; i < n; i++)
		if (s_prog[i] > s_course && !s_deadly[i])
		{
			s_course = s_prog[i];
			s_endIdx = int(i);
		}
	if (s_endIdx < 0 || s_course < 400.0f)
	{
		CONSOLE_ECHO("Loner-Strike deathrun: no course on the nav (%d areas reached from the CT start)\n", s_startIdx >= 0 ? 1 : 0);
		return;
	}
	s_end = *s_areas[s_endIdx]->GetCenter();
	Flood(s_endIdx, true, s_toEnd, &s_next);

	// the traps
	const char *classes[] = { "func_button", "func_rot_button" };
	for (const char *cls : classes)
	{
		CBaseEntity *pBtn = nullptr;
		while ((pBtn = UTIL_FindEntityByClassname(pBtn, cls)) != nullptr && s_traps.size() < 160)
		{
			if ((pBtn->pev->spawnflags & SF_BUTTON_TOUCH_ONLY) || FStringNull(pBtn->pev->target))
				continue;
			Trap t;
			Resolve(t, STRING(pBtn->pev->target), 0);
			if (!t.partCount)
				continue;
			t.button = pBtn;
			t.btn = pBtn->Center();
			Vector sum(0, 0, 0);
			for (int k = 0; k < t.partCount; k++)
				sum = sum + (t.parts[k].lo + t.parts[k].hi) * 0.5f;
			t.mid = sum / float(t.partCount);
			// where it's pressed from: the floor nearest it
			CNavArea *pressArea = TheNavAreaGrid.GetNearestNavArea(&t.btn, true);
			if (pressArea)
			{
				pressArea->GetClosestPointOnArea(&t.btn, &t.spot);
				t.spotIdx = Index(pressArea);
				t.reachT = t.spotIdx >= 0 && s_progT[t.spotIdx] >= 0.0f && (t.spot - t.btn).Make2D().IsLengthLessThan(140.0f);
			}
			// how far along the course: the nearest course area to what it moves
			float best = -1.0f, bestD = 1e9f;
			for (size_t i = 0; i < n; i++)
			{
				if (s_prog[i] < 0.0f)
					continue;
				const Vector *c = s_areas[i]->GetCenter();
				bool in = InTrap(t, *c + Vector(0, 0, 10.0f), 32.0f);
				float d = in ? 0.0f : (*c - t.mid).Length();
				if (d < bestD || (d == bestD && s_prog[i] < best))
				{
					bestD = d;
					best = s_prog[i];
				}
			}
			t.progress = bestD < 600.0f ? best : -1.0f;
			s_traps.push_back(t);
		}
	}
	s_ready = true;
	int reach = 0;
	for (const Trap &t : s_traps)
		reach += t.reachT ? 1 : 0;
	CONSOLE_ECHO("Loner-Strike deathrun: course end %.0f %.0f %.0f, %.0f units from the start; %d traps (%d the terrorist can walk to), %d teleport links, %d deadly areas\n",
		s_end.x, s_end.y, s_end.z, s_course, int(s_traps.size()), reach, int(s_teles.size()), int(std::count(s_deadly.begin(), s_deadly.end(), 1)));
}

// Rebuilt when the map or its nav changes
static bool Ready()
{
	if (TheNavAreaList.empty())
		return false;
	const char *map = STRING(gpGlobals->mapname);
	if (Q_strcmp(s_map, map) || s_navLoads != g_lsNavLoads || s_navFront != TheNavAreaList.front() || s_navSize != TheNavAreaList.size())
	{
		if (Q_strcmp(s_map, map))
			s_deaths.clear();
		Q_strlcpy(s_map, map);
		s_navLoads = g_lsNavLoads;
		s_navFront = TheNavAreaList.front();
		s_navSize = TheNavAreaList.size();
		Build();
	}
	return s_ready;
}

static float Progress(const Vector &pos)
{
	int i = Index(AreaAt(pos, 200.0f));
	return i >= 0 ? s_prog[i] : -1.0f;
}

static bool ButtonReady(const Trap &t)
{
	CBaseEntity *pBtn = t.button;
	if (!pBtn)
		return false;
	CBaseToggle *pToggle = static_cast<CBaseToggle *>(pBtn);
	return pToggle->m_toggle_state == TS_AT_BOTTOM && !pToggle->IsLockedByMaster();
}

// ---- each bot's run, and the activator's round ------------------------------------------------

struct Runner
{
	float roundStart = -1.0f;
	float startAt = 0.0f;			// leaves then
	bool done = false;				// got to the end: the plain brain from here
	int waypoint = -1;				// the area it's walking to now (a stretch of the course at a time)
	float moveState = -1.0f;		// the MoveTo it sent (its state time)
	float holdUntil = 0.0f;			// standing still till then
	Vector holdLook;
	int waitTrap = -1;				// the trap it's waiting at (or waited at: then it goes)
	int passedTrap = -1;
	float floorWaitSince = 0.0f;	// waiting for a floor that isn't there
	// a jump across a gap, armed by the path
	bool gapArmed = false;
	Vector takeoff, landing;
	Vector2D gapDir;
	float edge = 0.0f;				// the takeoff edge along gapDir
	float jumpedAt = -9.0f;
	bool duckJump = false;
	float backUntil = 0.0f;			// backing up for a run-up
	bool tele = false;				// walking into a teleport rather than jumping
	Vector lastGround;
	bool lastGroundSet = false;
	bool alive = false;
	float lastNavAt = 0.0f;
};

struct Activator
{
	float roundStart = -1.0f;
	float react = 0.7f, leadLo = 0.2f, leadHi = 0.5f;
	int greed = 30, patience = 50;
	int target = -1;				// the trap it's working
	float pickAt = 0.0f;
	float seenAt = 0.0f;			// noticed runners coming into it then (0 not yet)
	bool saving = false;
	float nextPressAt = 0.0f;
	float pressAt = -1.0f;			// pressing: look at it, use
	float moveState = -1.0f;
	Vector moveTo;
	bool fighting = false;			// gave up the traps for the round
	float decideFightAt = 0.0f;
};

static Runner s_run[MAX_CLIENTS + 1];
static Activator s_act;

static float RoundStart()
{
	return CSGameRules() ? CSGameRules()->m_fRoundStartTime : 0.0f;
}

// a death this map: where it was standing last; a trap's if a button went down just before
static void NoteDeath(const Vector &at, bool trap)
{
	for (Death &d : s_deaths)
		if ((d.at - at).IsLengthLessThan(120.0f) && d.trap == trap)
		{
			d.count++;
			d.when = gpGlobals->time;
			return;
		}
	if (s_deaths.size() >= 64)
		s_deaths.erase(s_deaths.begin());
	Death d;
	d.at = at;
	d.count = 1;
	d.trap = trap;
	d.when = gpGlobals->time;
	s_deaths.push_back(d);
	for (Trap &t : s_traps)
		if (trap && InTrap(t, at, 48.0f))
			t.kills++;
}

static int DeathsNear(const Vector &at, float r, bool *trap)
{
	int n = 0;
	*trap = false;
	for (const Death &d : s_deaths)
		if ((d.at - at).IsLengthLessThan(r))
		{
			n += d.count;
			*trap = *trap || d.trap;
		}
	return n;
}

static CBasePlayer *TheActivator()
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *p = UTIL_PlayerByIndex(i);
		if (p && p->IsAlive() && p->m_iTeam == TERRORIST)
			return p;
	}
	return nullptr;
}

} // namespace LsDr

// Each frame: who died where, and which buttons went down (to tell a trap's death from a fall)
void LsDrStep()
{
	using namespace LsDr;
	static bool s_btnDown[160];
	if (!LsDrOn() || !Ready())
		return;
	for (size_t i = 0; i < s_traps.size() && i < 160; i++)
	{
		bool down = !ButtonReady(s_traps[i]);
		if (down && !s_btnDown[i])
			s_lastPressAt = gpGlobals->time;
		s_btnDown[i] = down;
	}
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *p = UTIL_PlayerByIndex(i);
		Runner &r = s_run[i];
		bool alive = p && p->IsAlive() && p->m_iTeam == CT;
		if (alive && (p->pev->flags & FL_ONGROUND) && gpGlobals->time - r.lastNavAt > 0.2f)
		{
			r.lastNavAt = gpGlobals->time;
			r.lastGround = p->pev->origin;
			r.lastGroundSet = true;
		}
		if (r.alive && !alive && p && p->m_iTeam == CT && r.lastGroundSet)
		{
			// (killed by a player: a fight, nothing to learn about the course)
			edict_t *infl = p->pev->dmg_inflictor;
			bool byPlayer = infl && !FNullEnt(infl) && (infl->v.flags & FL_CLIENT) && infl != p->edict();
			if (!byPlayer)
				NoteDeath(r.lastGround, gpGlobals->time - s_lastPressAt < 5.0f);
		}
		r.alive = alive;
		if (!alive)
			r.lastGroundSet = false;
	}
}

// A walk through this area: deadly ones never; where people died this map, costlier
bool LsDrCosts()
{
	return LsDrOn() && LsDr::s_ready;
}

float LsDrAreaCost(const CNavArea *area)
{
	using namespace LsDr;
	int i = Index(area);
	if (i < 0)
		return 1.0f;
	if (s_deadly[i])
		return -1.0f;
	if (s_deaths.empty())
		return 1.0f;
	bool trap;
	int n = DeathsNear(*area->GetCenter(), 100.0f, &trap);
	return trap ? 1.0f : 1.0f + Q_min(6.0f, 1.5f * float(n));
}

// ---- the bots -----------------------------------------------------------------------------------

using namespace LsDr;

bool CCSBot::LsDrRunner() const
{
	return LsDrOn() && s_ready && m_iTeam == CT;
}

// Each think, before the state machine. True: it's standing still this think (waiting to leave,
// waiting at a trap, at a button), LsDrPostUpdate keeps it there
bool CCSBot::LsDrTick()
{
	if (!LsDrOn() || !Ready() || !IsAlive())
		return false;
	if (CSGameRules() && CSGameRules()->IsFreezePeriod())
		return false;
	if (m_iTeam == TERRORIST)
		return LsDrActivatorTick();
	if (m_iTeam != CT)
		return false;

	int idx = entindex();
	if (idx < 1 || idx > MAX_CLIENTS)
		return false;
	Runner &r = s_run[idx];
	float now = gpGlobals->time;
	float rs = RoundStart();

	// a new round or a new life: off in a moment, each at their own pace (the careful later, a
	// few dawdle; a respawn mid-round goes quickly)
	if (r.roundStart != rs || m_lsDrLife != m_iNumSpawns)
	{
		bool midRound = r.roundStart == rs;
		Vector lg = r.lastGround;
		bool lgs = r.lastGroundSet, al = r.alive;
		float lna = r.lastNavAt;
		r = Runner();
		r.lastGround = lg;
		r.lastGroundSet = lgs;
		r.alive = al;
		r.lastNavAt = lna;
		r.roundStart = rs;
		m_lsDrLife = m_iNumSpawns;
		float aggro = LsAggro();
		if (midRound)
			r.startAt = now + RANDOM_FLOAT(0.4f, 1.5f);
		else
		{
			r.startAt = now + RANDOM_FLOAT(0.0f, 1.5f) + 2.0f * (1.0f - aggro) * LsTrait(210);
			if (RANDOM_FLOAT(0.0f, 1.0f) < 0.12f)
				r.startAt += RANDOM_FLOAT(2.0f, 5.0f);	// (looking round, typing, slow off the mark)
		}
		r.holdUntil = r.startAt;
		r.holdLook = s_end;
		LsWhy("deathrun: start");
	}
	if (r.done)
		return false;
	if (now < r.startAt)
		return true;

	// at the end: the plain brain hunts the terrorist from here
	if ((pev->origin - s_end).Make2D().IsLengthLessThan(250.0f) && Q_fabs(pev->origin.z - s_end.z) < 140.0f)
	{
		r.done = true;
		LsWhy("deathrun: end");
		return false;
	}

	// a fight (the terrorist in sight) is the brain's
	if (IsAttacking() || m_isEnemyVisible || IsOnLadder() || !LsFree())
		return false;

	// waiting short of a trap (or for the floor ahead)
	CBasePlayer *pAct = TheActivator();
	if (r.waitTrap >= int(s_traps.size()))
		r.waitTrap = -1;
	if (r.waitTrap >= 0)
	{
		Trap &t = s_traps[r.waitTrap];
		bool armed = ButtonReady(t) && pAct && (pAct->pev->origin - t.btn).IsLengthLessThan(320.0f);
		// (it went off: a moment for it to finish, then through)
		if (!ButtonReady(t) && r.holdUntil > now + 1.6f)
			r.holdUntil = now + RANDOM_FLOAT(0.8f, 1.6f);
		if (now >= r.holdUntil || (!armed && ButtonReady(t)))
		{
			r.passedTrap = r.waitTrap;
			r.waitTrap = -1;
			r.holdUntil = 0.0f;
			LsWhy("deathrun: go");
		}
		else
			return true;
	}
	if (now < r.holdUntil)
		return true;

	// a trap on the path ahead whose button the terrorist is standing by: the careful wait for
	// it to go off (or for him to give up), the bold run it
	if (pAct && HasPath())
	{
		float along = 0.0f;
		Vector prev = pev->origin;
		bool onTrap = false;
		for (size_t ti = 0; ti < s_traps.size() && !onTrap; ti++)
			onTrap = ButtonReady(s_traps[ti]) && InTrap(s_traps[ti], pev->origin, 8.0f);
		for (int k = m_pathIndex; k < m_pathLength && along < 420.0f && !onTrap; k++)
		{
			along += (m_path[k].pos - prev).Length();
			prev = m_path[k].pos;
			for (size_t ti = 0; ti < s_traps.size(); ti++)
			{
				Trap &t = s_traps[ti];
				if (int(ti) == r.passedTrap || !ButtonReady(t) || !InTrap(t, m_path[k].pos, 24.0f))
					continue;
				if (!(pAct->pev->origin - t.btn).IsLengthLessThan(260.0f))
					continue;
				// (decided once per trap)
				r.passedTrap = int(ti);
				bool trapDeath;
				int deaths = DeathsNear(t.mid, 400.0f, &trapDeath);
				float chance = 0.2f + 0.55f * (1.0f - LsAggro()) + (trapDeath ? Q_min(0.3f, 0.1f * float(deaths)) : 0.0f);
				if (along < 60.0f || RANDOM_FLOAT(0.0f, 1.0f) >= chance)
				{
					LsWhy("deathrun: runs the trap");
					break;
				}
				r.waitTrap = int(ti);
				r.holdUntil = now + RANDOM_FLOAT(0.8f, 1.8f + 3.0f * (1.0f - LsAggro()));
				r.holdLook = t.mid;
				LsWhy("deathrun: waits at a trap");
				return true;
			}
			if (r.waitTrap >= 0)
				break;
		}
	}

	// on its way: a stretch of the course at a time (a path over the whole course is longer than
	// a path can be), along the shortest way to the end
	int here = Index(m_lastKnownArea ? m_lastKnownArea : AreaAt(pev->origin));
	int way = here;
	if (here >= 0 && s_toEnd[here] >= 0.0f)
	{
		float walked = 0.0f;
		for (int hops = 0; hops < 80 && s_next[way] >= 0 && walked < 1600.0f; hops++)
		{
			walked += Dist(way, s_next[way]);
			way = s_next[way];
		}
	}
	else
		way = s_endIdx;
	bool headed = IsMovingTo() && Q_fabs(m_stateTimestamp - r.moveState) < 0.05f;
	bool farFromWay = r.waypoint < 0 || way < 0 || (s_toEnd[r.waypoint] >= 0.0f && s_toEnd[way] >= 0.0f && s_toEnd[r.waypoint] - s_toEnd[way] > 500.0f);
	if (way >= 0 && (!headed || farFromWay || (way == s_endIdx && r.waypoint != s_endIdx)))
	{
		r.waypoint = way;
		Vector to = (way == s_endIdx) ? s_end : *s_areas[way]->GetCenter();
		MoveTo(&to, LsAggro() > 0.45f ? FASTEST_ROUTE : SAFEST_ROUTE);
		r.moveState = m_stateTimestamp;
		// (deathrun's a run: nobody walks it)
		Run();
		LsWhy("deathrun: course");
	}
	return false;
}

// The terrorist working the traps
bool CCSBot::LsDrActivatorTick()
{
	Activator &a = s_act;
	float now = gpGlobals->time;
	float rs = RoundStart();
	if (a.roundStart != rs)
	{
		a = Activator();
		a.roundStart = rs;
		float skill = LsSkill();
		a.react = RANDOM_FLOAT(0.55f, 0.9f) - 0.3f * skill;
		float centre = RANDOM_FLOAT(0.25f, 0.55f);
		float spread = 0.35f - 0.2f * skill;
		a.leadLo = Q_max(0.05f, centre - spread * 0.5f);
		a.leadHi = centre + spread * 0.5f;
		a.greed = RANDOM_LONG(0, 70);
		a.patience = RANDOM_LONG(25, 80);
		a.nextPressAt = now + 3.0f;
		for (Trap &t : s_traps)
			t.usedThisRound = false;
	}
	if (a.fighting || IsAttacking() || m_isEnemyVisible || IsOnLadder())
		return false;

	// the runners
	CBasePlayer *runners[MAX_CLIENTS];
	float prog[MAX_CLIENTS];
	int count = 0;
	float lead = -1.0f, rear = 1e9f;
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *p = UTIL_PlayerByIndex(i);
		if (!p || !p->IsAlive() || p->m_iTeam != CT)
			continue;
		runners[count] = p;
		prog[count] = Progress(p->pev->origin);
		lead = Q_max(lead, prog[count]);
		if (prog[count] >= 0.0f)
			rear = Q_min(rear, prog[count]);
		count++;
	}
	if (!count)
		return false;

	// nearly through, or nothing left ahead of them: now and then it leaves the buttons to fight
	bool anyAhead = false;
	for (const Trap &t : s_traps)
		if (!t.usedThisRound && t.reachT && t.progress >= 0.0f && t.progress > rear - 100.0f)
			anyAhead = true;
	if ((lead > 0.85f * s_course || !anyAhead) && now >= a.decideFightAt)
	{
		a.decideFightAt = now + 3.0f;
		if (!anyAhead || RANDOM_FLOAT(0.0f, 1.0f) < 0.25f + 0.5f * LsAggro())
		{
			a.fighting = true;
			a.target = -1;
			LsWhy("deathrun: leaves the traps to fight");
			return false;
		}
	}

	// which trap: the first one coming up that it can get to before the runners do
	if (a.target >= int(s_traps.size()))
		a.target = -1;
	if (a.target < 0 || now >= a.pickAt || s_traps[a.target].usedThisRound)
	{
		a.pickAt = now + 1.0f;
		int best = -1, late = -1;
		float bestProg = 1e9f, bestSpare = -1e9f;
		for (size_t ti = 0; ti < s_traps.size(); ti++)
		{
			const Trap &t = s_traps[ti];
			if (t.usedThisRound || !t.reachT || t.progress < 0.0f || !t.button)
				continue;
			// (runners still to come to it)
			if (t.progress < rear - 100.0f)
				continue;
			float ahead = Q_max(0.0f, t.progress - lead);
			float travel = (pev->origin - t.spot).Length() * 1.3f / 250.0f;
			float spare = ahead / 230.0f - travel;
			bool there = (pev->origin - t.spot).Make2D().IsLengthLessThan(150.0f);
			if ((spare > 0.5f || there) && t.progress < bestProg)
			{
				bestProg = t.progress;
				best = int(ti);
			}
			if (spare > bestSpare)
			{
				bestSpare = spare;
				late = int(ti);
			}
		}
		if (best < 0)
			best = late;
		if (best != a.target)
		{
			a.target = best;
			a.seenAt = 0.0f;
			a.saving = false;
			a.pressAt = -1.0f;
		}
	}
	if (a.target < 0)
		return false;
	Trap &t = s_traps[a.target];

	// to its button
	if ((pev->origin - t.spot).Make2D().IsLengthGreaterThan(48.0f) || Q_fabs(pev->origin.z - HalfHumanHeight - t.spot.z) > 60.0f)
	{
		bool headed = IsMovingTo() && Q_fabs(m_stateTimestamp - a.moveState) < 0.05f && (a.moveTo - t.spot).IsLengthLessThan(8.0f);
		if (!headed)
		{
			a.moveTo = t.spot;
			MoveTo(&t.spot, FASTEST_ROUTE);
			a.moveState = m_stateTimestamp;
			Run();
			LsWhy("deathrun: to a trap button");
		}
		return false;
	}

	// there: watch for runners and press it (dr_bot_traps' activator, as a player)
	if (a.pressAt > 0.0f)
	{
		// (it went: spent for the round; still up after a moment: closer, and again)
		if (!ButtonReady(t))
		{
			t.usedThisRound = true;
			a.pressAt = -1.0f;
			a.target = -1;
			a.seenAt = 0.0f;
			a.nextPressAt = now + 1.5f + RANDOM_FLOAT(0.0f, 1.0f);
			LsWhy("deathrun: pressed");
			return true;
		}
		if (now - a.pressAt > 1.5f)
		{
			a.pressAt = -1.0f;
			t.usedThisRound = true;	// (can't work it from here: given up)
			a.target = -1;
		}
		return true;
	}
	if (now < a.nextPressAt || !ButtonReady(t))
		return true;

	const float radius = 48.0f;
	int coming = 0, in = 0;
	bool ctAtButton = false;
	float leadT = RANDOM_FLOAT(a.leadLo, a.leadHi);
	for (int c = 0; c < count; c++)
	{
		CBasePlayer *p = runners[c];
		if ((p->pev->origin - t.btn).IsLengthLessThan(120.0f))
		{
			ctAtButton = true;
			break;
		}
		Vector ahead = p->pev->origin + p->pev->velocity * leadT;
		bool isIn = InTrap(t, ahead, radius);
		bool isNear = isIn || InTrap(t, p->pev->origin, 400.0f);
		coming += isNear ? 1 : 0;
		in += isIn ? 1 : 0;
	}
	if (ctAtButton || !coming)
	{
		a.seenAt = 0.0f;
		a.saving = false;
		return true;
	}
	if (a.seenAt == 0.0f)
	{
		// (not watching every moment: noticed now, or a little late)
		a.seenAt = RANDOM_LONG(1, 100) <= 75 ? now : now + RANDOM_FLOAT(0.8f, 2.0f);
		// (one out in front, plenty behind: a patient one lets him by and saves it for them)
		a.saving = coming == 1 && count - coming >= 2 && RANDOM_LONG(1, 100) <= a.patience;
		return true;
	}
	if (a.saving || now - a.seenAt < a.react || !in)
		return true;
	// (greedy: more on the way, hold it a little longer)
	if (in < coming && now - a.seenAt < 2.5f && RANDOM_LONG(1, 100) <= a.greed)
		return true;
	// (a moment of doubt)
	if (RANDOM_LONG(1, 100) <= 6)
	{
		a.seenAt = now;
		return true;
	}
	a.pressAt = now;
	LsWhy("deathrun: presses");
	return true;
}

// After the state machine: a runner waiting, the activator by its button (stepping up to it,
// looking at it, pressing use)
void CCSBot::LsDrPostUpdate()
{
	if (!LsDrOn() || !s_ready || !IsAlive() || IsAttacking())
		return;
	float now = gpGlobals->time;
	int idx = entindex();
	if (idx < 1 || idx > MAX_CLIENTS)
		return;
	if (m_iTeam == CT)
	{
		Runner &r = s_run[idx];
		if (r.done || now >= r.holdUntil || m_isEnemyVisible)
			return;
		ClearMovement();
		ResetStuckMonitor();
		SetLookAt("Loner-Strike: deathrun wait", &r.holdLook, PRIORITY_MEDIUM, 0.5f);
		return;
	}
	if (m_iTeam != TERRORIST || s_act.fighting || s_act.target < 0 || s_act.target >= int(s_traps.size()) || m_isEnemyVisible)
		return;
	Activator &a = s_act;
	Trap &t = s_traps[a.target];
	if ((pev->origin - t.spot).Make2D().IsLengthGreaterThan(48.0f) || Q_fabs(pev->origin.z - HalfHumanHeight - t.spot.z) > 60.0f)
		return;
	CBaseEntity *pBtn = t.button;
	if (!pBtn)
		return;
	ClearMovement();
	ResetStuckMonitor();
	// within use reach: the button's box within 56 units of where it stands
	Vector o = pev->origin, lo = pBtn->pev->absmin, hi = pBtn->pev->absmax;
	Vector gap(Q_max(Q_max(lo.x - o.x, 0.0f), o.x - hi.x), Q_max(Q_max(lo.y - o.y, 0.0f), o.y - hi.y), Q_max(Q_max(lo.z - o.z, 0.0f), o.z - hi.z));
	if (gap.Length() > 52.0f)
	{
		Vector step(t.btn.x, t.btn.y, pev->origin.z);
		MoveTowardsPosition(&step);
	}
	SetLookAt("Loner-Strike: trap button", &t.btn, PRIORITY_HIGH, 0.5f, false, 3.0f);
	if (a.pressAt > 0.0f)
	{
		// (looking at it - within the use cone - then use)
		UTIL_MakeVectors(pev->v_angle);
		Vector to = (t.btn - (pev->origin + pev->view_ofs)).Normalize();
		if (DotProduct(to, gpGlobals->v_forward) > 0.9f && gap.Length() <= 60.0f)
			UseEnvironment();
	}
}

// In path movement: the next stretch crosses a gap (a jump, armed for LsDrDrive), goes through a
// teleport (walk into it), or onto a floor that isn't there now (wait at the edge)
static bool LsDrSeparated(const CNavArea *a, const CNavArea *b)
{
	const Extent *ea = a->GetExtent(), *eb = b->GetExtent();
	float dx = Q_max(eb->lo.x - ea->hi.x, ea->lo.x - eb->hi.x);
	float dy = Q_max(eb->lo.y - ea->hi.y, ea->lo.y - eb->hi.y);
	return Q_max(dx, dy) > 10.0f;
}

static bool LsDrFloorThere(const CNavArea *area, const Vector &nearPos)
{
	Vector p;
	area->GetClosestPointOnArea(&nearPos, &p);
	float z;
	Vector n;
	return GroundBelow(Vector(p.x, p.y, area->GetZ(&p) + 40.0f), 100.0f, &z, &n);
}

void CCSBot::LsDrPathTick()
{
	int idx = entindex();
	if (idx < 1 || idx > MAX_CLIENTS)
		return;
	Runner &r = s_run[idx];
	float now = gpGlobals->time;
	if (!LsDrOn() || !s_ready || m_pathLength < 2)
	{
		r.gapArmed = false;
		return;
	}
	// (in the air on a jump: keep steering for the landing)
	if (r.gapArmed && !(pev->flags & FL_ONGROUND) && now - r.jumpedAt < 1.5f)
	{
		m_goalPosition = r.landing;
		return;
	}
	r.gapArmed = false;
	r.tele = false;
	for (int i = Q_max(1, m_pathIndex - 1); i < m_pathLength && i <= m_pathIndex + 3; i++)
	{
		const ConnectInfo &to = m_path[i];
		const CNavArea *A = m_path[i - 1].area, *B = to.area;
		if (to.ladder || to.how > GO_WEST || A == B)
			continue;
		if (m_lastKnownArea == B)
			break;
		if (!LsDrSeparated(A, B))
			continue;
		// a teleport link: walk into the trigger
		int ia = Index(A), ib = Index(B);
		for (const Tele &tl : s_teles)
			if (tl.from == ia && tl.to == ib)
			{
				r.tele = true;
				m_goalPosition = tl.at;
				break;
			}
		if (r.tele)
			return;
		// a gap: run at the edge, jump, land a little way in
		Vector2D d;
		DirectionToVector2D((NavDirType)to.how, &d);
		const Extent *e = A->GetExtent();
		float edgeAlong = (to.how == NORTH) ? -e->lo.y : (to.how == SOUTH) ? e->hi.y : (to.how == EAST) ? e->hi.x : -e->lo.x;
		Vector takeoff = to.pos;
		if (to.how == NORTH || to.how == SOUTH)
			takeoff.y = (to.how == NORTH) ? e->lo.y : e->hi.y;
		else
			takeoff.x = (to.how == EAST) ? e->hi.x : e->lo.x;
		Vector land;
		B->GetClosestPointOnArea(&takeoff, &land);
		land.x += d.x * 24.0f;
		land.y += d.y * 24.0f;
		land.z = B->GetZ(&land);
		// (the floor over there isn't there right now - a platform away, a trap floor open: wait)
		if (!LsDrFloorThere(B, land))
		{
			if ((pev->origin - takeoff).Make2D().IsLengthLessThan(150.0f))
			{
				if (r.floorWaitSince <= 0.0f)
					r.floorWaitSince = now;
				if (now - r.floorWaitSince > 12.0f)
				{
					LsForceBadArea(B, 20.0f);
					DestroyPath();
					r.floorWaitSince = 0.0f;
					return;
				}
				r.holdUntil = Q_max(r.holdUntil, now + 0.3f);
				r.holdLook = land;
				LsWhy("deathrun: waits for the floor");
			}
			return;
		}
		r.floorWaitSince = 0.0f;
		r.gapArmed = true;
		r.takeoff = takeoff;
		r.landing = land;
		r.gapDir = d;
		r.edge = edgeAlong;
		r.duckJump = land.z > A->GetZ(&takeoff) + 12.0f;
		m_goalPosition = land;
		SetLookAt("Loner-Strike: the jump", &land, PRIORITY_HIGH, 0.3f);
		return;
	}
	// (the floor along the next stretch isn't there now: wait at the edge)
	for (int i = m_pathIndex; i < m_pathLength && i <= m_pathIndex + 1; i++)
	{
		const CNavArea *B = m_path[i].area;
		if (B == m_lastKnownArea || m_path[i].ladder)
			continue;
		if ((pev->origin - m_path[i].pos).Make2D().IsLengthGreaterThan(120.0f))
			break;
		if (!LsDrFloorThere(B, m_path[i].pos))
		{
			if (r.floorWaitSince <= 0.0f)
				r.floorWaitSince = now;
			if (now - r.floorWaitSince > 12.0f)
			{
				LsForceBadArea(B, 20.0f);
				DestroyPath();
				r.floorWaitSince = 0.0f;
				return;
			}
			r.holdUntil = Q_max(r.holdUntil, now + 0.3f);
			r.holdLook = m_path[i].pos;
			LsWhy("deathrun: waits for the floor");
			return;
		}
	}
	r.floorWaitSince = 0.0f;
}

// Each command: the jump across a gap, timed at the edge (a think is too coarse for that), with a
// run-up if it got there slowly, steered at the landing, tucked for one that's higher up
void LsDrDrive(CBasePlayer *bot, float *forward, float *strafe, unsigned short *buttons)
{
	if (!bot->IsBot() || !bot->IsAlive())
		return;
	int idx = bot->entindex();
	if (idx < 1 || idx > MAX_CLIENTS)
		return;
	Runner &r = s_run[idx];
	if (!r.gapArmed || r.tele || !LsDrOn())
		return;
	CCSBot *me = static_cast<CCSBot *>(bot);
	float now = gpGlobals->time;
	bool ground = (bot->pev->flags & FL_ONGROUND) != 0;
	Vector2D o = bot->pev->origin.Make2D();
	Vector2D wish;
	if (ground)
	{
		float toEdge = r.edge - DotProduct(o, r.gapDir);
		float speed = DotProduct(bot->pev->velocity.Make2D(), r.gapDir);
		if (now < r.backUntil)
			wish = -r.gapDir;
		else
		{
			wish = (r.landing.Make2D() - o);
			float len = wish.Length();
			wish = len > 1.0f ? wish / len : r.gapDir;
			if (toEdge < 14.0f + 0.06f * Q_max(0.0f, speed) && toEdge > -20.0f && now - r.jumpedAt > 0.8f)
			{
				if (speed > 170.0f)
				{
					if (me->IsCrouching())
						me->StandUp();
					if (me->Jump(MUST_JUMP))
						r.jumpedAt = now;
				}
				else if (toEdge < 6.0f)
					r.backUntil = now + 0.3f;
			}
		}
	}
	else
	{
		if (now - r.jumpedAt > 1.5f)
			return;
		wish = (r.landing.Make2D() - o);
		float len = wish.Length();
		wish = len > 1.0f ? wish / len : r.gapDir;
		if (r.duckJump)
			*buttons |= IN_DUCK;
	}
	// the move in the world, into forward and strafe for where it's looking
	float yaw = bot->pev->v_angle.y * float(M_PI / 180.0);
	float c = cosf(yaw), s = sinf(yaw);
	float speed = 250.0f;
	float wx = wish.x * speed, wy = wish.y * speed;
	*forward = wx * c + wy * s;
	*strafe = wx * s - wy * c;
	*buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);
	if (*forward > 1.0f)
		*buttons |= IN_FORWARD;
	else if (*forward < -1.0f)
		*buttons |= IN_BACK;
	if (*strafe > 1.0f)
		*buttons |= IN_MOVERIGHT;
	else if (*strafe < -1.0f)
		*buttons |= IN_MOVELEFT;
}

// ---- ls_dr ------------------------------------------------------------------------------------

void LsDrCommand()
{
	// ls_dr gap <x> <y> <z> <dir 0 N 1 E 2 S 3 W>: the gap check from a floor spot, with reasons
	if (CMD_ARGC() >= 6 && FStrEq(CMD_ARGV(1), "gap"))
	{
		Vector p(Q_atof(CMD_ARGV(2)), Q_atof(CMD_ARGV(3)), Q_atof(CMD_ARGV(4)));
		float z;
		Vector n, land;
		if (GroundBelow(p + Vector(0, 0, 40.0f), 200.0f, &z, &n))
			p.z = z;
		DeadlyHurts(s_learnHurts);
		CONSOLE_ECHO("ls_dr gap from %.0f %.0f %.0f dir %d:\n", p.x, p.y, p.z, Q_atoi(CMD_ARGV(5)));
		GapJumpAt(p, Q_atoi(CMD_ARGV(5)) & 3, true, &land, &n, true);
		return;
	}
	if (!Ready())
	{
		CONSOLE_ECHO("ls_dr: no course (bot_ls_deathrun %s, deathrun map %d, nav areas %d)\n", cv_bot_ls_deathrun.string, LsDrMapName() ? 1 : 0, int(TheNavAreaList.size()));
		return;
	}
	CONSOLE_ECHO("ls_dr: course end %.0f %.0f %.0f at %.0f units, %d areas reached of %d, %d teleport links, %d deaths remembered\n",
		s_end.x, s_end.y, s_end.z, s_course, int(std::count_if(s_prog.begin(), s_prog.end(), [](float v) { return v >= 0.0f; })),
		int(s_areas.size()), int(s_teles.size()), int(s_deaths.size()));
	for (size_t i = 0; i < s_traps.size(); i++)
	{
		const Trap &t = s_traps[i];
		CONSOLE_ECHO("  trap %d: button %.0f %.0f %.0f, %d parts, progress %.0f, terrorist %s, kills %d%s\n", int(i), t.btn.x, t.btn.y, t.btn.z,
			t.partCount, t.progress, t.reachT ? "gets there" : "can't get there", t.kills, t.usedThisRound ? ", used" : "");
	}
	for (const Death &d : s_deaths)
		CONSOLE_ECHO("  death x%d at %.0f %.0f %.0f (%s)\n", d.count, d.at.x, d.at.y, d.at.z, d.trap ? "trap" : "fall/hazard");
}
