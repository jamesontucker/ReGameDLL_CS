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
static bool s_lsCourseLearning = false;	// (learning a nav right now, with course learning on)

static bool LsFloorTop(const TraceResult &result)
{
	if (!result.pHit || result.vecPlaneNormal.z < 0.7f)
		return false;
	entvars_t *pevHit = VARS(result.pHit);
	// (and a breakable floor of any stuff: deathrun's floors that crumble when the trap's pressed)
	if (!FClassnameIs(pevHit, "func_door") && !FClassnameIs(pevHit, "func_door_rotating") && !FClassnameIs(pevHit, "func_breakable"))
		return false;
	Vector size = pevHit->absmax - pevHit->absmin;
	if (size.x < 40.0f || size.y < 40.0f)
		return false;
	// (a turning door's box is the whole sphere it can turn through: any top-facing face of it is its floor)
	if (FClassnameIs(pevHit, "func_door_rotating"))
		return true;
	return result.vecEndPos.z >= pevHit->absmax.z - 4.0f;
}

// (for the learner's ground search only: in play the ground search is the stock one)
bool LsMoverFloor(const TraceResult &result)
{
	return s_lsCourseLearning && LsFloorTop(result);
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
		// (its own box: absmin/absmax are padded a unit all round, and on a floor a unit decides)
		Box b;
		b.lo = pEnt->pev->origin + pEnt->pev->mins;
		b.hi = pEnt->pev->origin + pEnt->pev->maxs;
		out.push_back(b);
	}
}

// A player standing on this floor spot is in one (feet to head overlapping the trigger, as the
// engine tests it: both boxes padded a unit, so a floor 2 units over a deadly trigger still touches it)
static bool DeadlyAt(const std::vector<Box> &boxes, const Vector &floor)
{
	for (const Box &b : boxes)
		if (floor.x >= b.lo.x && floor.x <= b.hi.x && floor.y >= b.lo.y && floor.y <= b.hi.y && floor.z <= b.hi.z + 2.0f && floor.z + HumanHeight >= b.lo.z - 2.0f)
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
static bool GroundBelow(const Vector &from, float depth, float *z, Vector *normal, edict_t **hit = nullptr)
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
		if (tr.pHit && IsEntityWalkable(VARS(tr.pHit), WALK_THRU_DOORS | WALK_THRU_BREAKABLES) && !LsFloorTop(tr))
		{
			ignore = tr.pHit;
			start = tr.vecEndPos - Vector(0, 0, 1.0f);
			continue;
		}
		*z = tr.vecEndPos.z;
		*normal = tr.vecPlaneNormal;
		if (hit)
			*hit = tr.pHit;
		return true;
	}
	return false;
}

// A running jump from a node's edge one way: the nearest grid point it lands on, if any.
// The jump: 240 units/s along (a knife or pistol run, a little short of 250), 268 up (45 units
// high), feet tucked 18 more for a landing higher up (a duck-jump); a hull that fits all the way;
// a floor to land on that's flat enough, not a deadly trigger, not a fall that hurts much
static bool GapJumpVec(const Vector &p, Vector2D d, bool runup, Vector *land, Vector *landNormal, bool say);
static bool s_jumpRelaxed = false;	// (a runner with no other way: the full running jump's landing isn't checked)
static float s_jumpSpeed = 0.0f;		// (a hop at this speed along rather than a full running jump: 0 = full)

static bool GapJumpAt(const Vector &p, int dir, bool runup, Vector *land, Vector *landNormal, bool say)
{
	Vector2D d;
	DirectionToVector2D((NavDirType)dir, &d);
	return GapJumpVec(p, d, runup, land, landNormal, say);
}

// The same check any way along the ground (d: a unit vector)
static bool GapJumpVec(const Vector &p, Vector2D d, bool runup, Vector *land, Vector *landNormal, bool say)
{

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

	const float vh = s_jumpSpeed > 0.0f ? s_jumpSpeed : 240.0f, vz = 268.0f, g = 800.0f;
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
			if (GroundBelow(mid, 32.0f + 60.0f, &midZ, &midN) && !DeadlyAt(s_learnHurts, Vector(mid.x, mid.y, midZ)))
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
		// where a full running jump really comes down at that height (it doesn't stop at the near
		// edge: a jump onto a small block overshoots it) - floor there too: about that height, or a
		// little lower (it lands on a lip or a step and drops onto the floor behind: dojo's walkways
		// have a raised edge), or a wall it's stopped by (it comes down at the wall's foot) - not a hole
		{
			float disc = vz * vz - 2.0f * g * (dz - (duck ? 18.0f : 0.0f));
			float tDown = disc > 0.0f ? (vz + sqrtf(disc)) / g : t;
			float reach = 0.5f * GenerationStepSize + vh * tDown;
			bool solid = true;
			for (float off = -16.0f; off <= 16.0f && solid; off += 16.0f)
			{
				float rz;
				Vector rn;
				Vector rc(p.x + d.x * (reach + off), p.y + d.y * (reach + off), gz + 40.0f);
				if (UTIL_PointContents(rc) == CONTENTS_SOLID)
					continue;	// (a wall there: it's stopped short of it)
				if (!GroundBelow(rc, 40.0f + 120.0f, &rz, &rn) || rz > gz + 20.0f || rz < gz - 120.0f || DeadlyAt(s_learnHurts, Vector(rc.x, rc.y, rz)))
					solid = false;
			}
			if (!solid && s_jumpRelaxed)
				solid = true;	// (no other way: the near-edge landing will do, a person would try it)
			if (!solid)
			{
				if (say)
					CONSOLE_ECHO("  %.0f: a full jump comes down %.0f out, and there's no floor there\n", L, reach);
				continue;
			}
		}
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
	// (a run-up: two steps behind it, straight)
	const CNavNode *back = node->GetConnectedNode(OppositeDirection((NavDirType)dir));
	bool runup = back && back->GetConnectedNode(OppositeDirection((NavDirType)dir)) != nullptr;
	// (a full running jump, or a hop at walking pace where a full one carries past the landing - a
	// ledge or a lip just across, lava beyond: the runner hops it the same way)
	static const float paces[] = { 0.0f, 150.0f, 130.0f, 110.0f };
	for (float pace : paces)
	{
		s_jumpSpeed = pace;
		bool ok = GapJumpAt(*node->GetPosition(), dir, runup, land, landNormal, false);
		s_jumpSpeed = 0.0f;
		if (ok)
			return true;
	}
	return false;
}

} // namespace LsDr

void LsCourseLearnStart()
{
	using namespace LsDr;
	s_learnLinks.clear();
	s_scanned.clear();
	s_teleSeeded.clear();
	s_gapSeeds = s_teleSeeds = 0;
	s_lsCourseLearning = LsNavCourse();
	if (s_lsCourseLearning)
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
			// (a step onto deadly floor - lava, a hurt pit level with the walkway - is no way on either:
			// on such maps the walkways' edges only ever led into it, and no jump was looked for)
			const CNavNode *adj = node->GetConnectedNode((NavDirType)dir);
			if (adj && adj->GetPosition()->z > node->GetPosition()->z - 60.0f && !DeadlyAt(s_learnHurts, *adj->GetPosition()))
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
	bool was = s_lsCourseLearning;
	s_lsCourseLearning = false;
	if (!was)
		return;
	int made = 0, noArea = 0, same = 0, already = 0;
	for (const Link &l : s_learnLinks)
	{
		// (looked up a little way in from each end: a node on an area's edge can fall in the grid
		// cell next to it)
		Vector2D d;
		DirectionToVector2D((NavDirType)l.dir, &d);
		Vector a = l.from + Vector(-d.x * 6.0f, -d.y * 6.0f, 8.0f), b = l.to + Vector(d.x * 6.0f, d.y * 6.0f, 8.0f);
		CNavArea *from = TheNavAreaGrid.GetNavArea(&a);
		CNavArea *to = TheNavAreaGrid.GetNavArea(&b);
		if (!from)
			from = TheNavAreaGrid.GetNearestNavArea(&a);
		if (!to)
			to = TheNavAreaGrid.GetNearestNavArea(&b);
		if (from && (*from->GetCenter() - a).Make2D().IsLengthGreaterThan(400.0f))
			from = nullptr;
		if (to && (*to->GetCenter() - b).Make2D().IsLengthGreaterThan(400.0f))
			to = nullptr;
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

// A door between two learned floors (nav generation doesn't connect through a closed door): linked,
// and the button that opens it (a trap whose wiring moves it), if a runner can get to that button
struct DoorLink { int from, to; EntityHandle<CBaseEntity> door; Vector at; int trap = -1; };

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
static std::vector<DoorLink> s_doorLinks;

// A ledge a jump climbs from A onto B (touching areas): 0 if it does, else which check fails
// (1 not side by side, 2 not 20-56 up, 3 already linked, 4 deadly on top, 5 no room on top, 6 the way up blocked);
// dir: the way across. Used at map start and by "ls_dr ledge".
static int LedgeClimb(CNavArea *A, CNavArea *B, Vector *dirOut, Vector *landOut)
{
	const Extent *ea = A->GetExtent(), *eb = B->GetExtent();
	float dx = Q_max(eb->lo.x - ea->hi.x, ea->lo.x - eb->hi.x), dy = Q_max(eb->lo.y - ea->hi.y, ea->lo.y - eb->hi.y);
	if (dx > 4.0f || dy > 4.0f || (dx > 0.5f && dy > 0.5f))
		return 1;
	Vector ca = *A->GetCenter(), cb = *B->GetCenter();
	Vector pa, pb;
	A->GetClosestPointOnArea(&cb, &pa);
	B->GetClosestPointOnArea(&pa, &pb);
	float rise = pb.z - pa.z;
	if (rise < 20.0f || rise > 56.0f)
		return 2;
	if (A->IsConnected(B, NUM_DIRECTIONS))
		return 3;
	if (DeadlyAt(s_hurts, cb))
		return 4;
	Vector dir2 = (pb - pa).Make2D().IsLengthLessThan(1.0f) ? (cb - ca) : (pb - pa);
	dir2.z = 0.0f;
	dir2 = dir2.Normalize();
	Vector land = pb + dir2 * 24.0f;
	land.z = B->GetZ(&land);
	// (a duck-jump goes up tucked: the crouched hull, so a low corridor over the ledge is fine)
	TraceResult tr;
	Vector up(pa.x, pa.y, pb.z + HalfHumanHeight * 0.5f + 2.0f), top(land.x, land.y, land.z + HalfHumanHeight * 0.5f + 2.0f);
	UTIL_TraceHull(top, top, ignore_monsters, head_hull, nullptr, &tr);
	if (tr.fStartSolid || tr.fAllSolid)
		return 5;
	UTIL_TraceHull(up, top, ignore_monsters, head_hull, nullptr, &tr);
	if (tr.fStartSolid || tr.flFraction < 1.0f)
		return 6;
	if (dirOut)
		*dirOut = dir2;
	if (landOut)
		*landOut = land;
	return 0;
}

// A walkable seam A -> B the nav left unlinked: side by side, level within a step where they meet,
// and a standing player gets across (no wall between) - starwars' start corridor, ramps whose
// centres are 48 apart but meet level
static bool SeamWalk(CNavArea *A, CNavArea *B)
{
	const Extent *ea = A->GetExtent(), *eb = B->GetExtent();
	float dx = Q_max(eb->lo.x - ea->hi.x, ea->lo.x - eb->hi.x), dy = Q_max(eb->lo.y - ea->hi.y, ea->lo.y - eb->hi.y);
	if (dx > 2.0f || dy > 2.0f || A->IsConnected(B, NUM_DIRECTIONS))
		return false;
	// (a shared stretch of edge, or only a corner - a hole in the nav beside them leaves the way on
	// diagonal: the hull trace below decides whether a player gets across)
	Vector cb = *B->GetCenter(), pa, pb;
	A->GetClosestPointOnArea(&cb, &pa);
	B->GetClosestPointOnArea(&pa, &pb);
	if (Q_fabs(pb.z - pa.z) > StepHeight || DeadlyAt(s_hurts, cb))
		return false;
	Vector d = (cb - *A->GetCenter());
	d.z = 0.0f;
	d = d.Normalize();
	// (just above the floor - a low corridor has little room over a standing player - and the step's
	// height higher at the low end)
	Vector from = pa - d * 16.0f, to = pb + d * 16.0f;
	float za = A->GetZ(&from), zb = B->GetZ(&to), top = Q_max(za, zb);
	from.z = top + HalfHumanHeight + 2.0f;
	to.z = top + HalfHumanHeight + 2.0f;
	TraceResult tr;
	UTIL_TraceHull(from, to, ignore_monsters, human_hull, nullptr, &tr);
	return !tr.fStartSolid && tr.flFraction >= 1.0f;
}

// Is the door of this link in the way right now (a hull between the two floors hits it)
static bool DoorShut(const DoorLink &dl)
{
	CBaseEntity *pDoor = dl.door;
	if (!pDoor || dl.from < 0 || dl.to < 0 || size_t(dl.from) >= s_areas.size() || size_t(dl.to) >= s_areas.size())
		return false;
	Vector a = *s_areas[dl.from]->GetCenter() + Vector(0, 0, HalfHumanHeight + 4.0f);
	Vector b = *s_areas[dl.to]->GetCenter() + Vector(0, 0, HalfHumanHeight + 4.0f);
	TraceResult tr;
	UTIL_TraceHull(a, b, ignore_monsters, human_hull, nullptr, &tr);
	return tr.flFraction < 1.0f && tr.pHit == pDoor->edict();
}

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

static bool s_btnDown[160];
static void ResetBots();

static void Build()
{
	s_ready = false;
	// (indexes into the last nav's tables are no good now)
	ResetBots();
	Q_memset(s_btnDown, 0, sizeof(s_btnDown));
	s_areas.clear();
	s_idx.clear();
	s_out.clear();
	s_traps.clear();
	s_teles.clear();
	s_doorLinks.clear();
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
		// (where you end up: a destination over a shaft drops you into the next teleport - followed)
		Vector dest = pDest->pev->origin;
		CNavArea *destArea = nullptr;
		for (int chain = 0; chain < 4 && !destArea; chain++)
		{
			destArea = TheNavAreaGrid.GetNavArea(&dest, 400.0f);
			if (destArea)
				break;
			TraceResult tr;
			UTIL_TraceLine(dest, dest - Vector(0, 0, 8192.0f), ignore_monsters, nullptr, &tr);
			Vector bottom = tr.vecEndPos + Vector(0, 0, 16.0f);
			CBaseEntity *pNext = nullptr, *pFound = nullptr;
			while ((pNext = UTIL_FindEntityByClassname(pNext, "trigger_teleport")) != nullptr)
				if (pNext != pTele && bottom.x >= pNext->pev->absmin.x && bottom.x <= pNext->pev->absmax.x && bottom.y >= pNext->pev->absmin.y
					&& bottom.y <= pNext->pev->absmax.y && bottom.z >= pNext->pev->absmin.z - 32.0f && bottom.z <= pNext->pev->absmax.z && !FStringNull(pNext->pev->target))
					pFound = pNext;
			CBaseEntity *pNextDest = pFound ? UTIL_FindEntityByTargetname(nullptr, STRING(pFound->pev->target)) : nullptr;
			if (!pNextDest)
				break;
			dest = pNextDest->pev->origin;
		}
		int to = Index(destArea);
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
			// (room to walk into it from this area: a sliver along its edge is no way in)
			float ox = Q_min(e->hi.x, hi.x) - Q_max(e->lo.x, lo.x), oy = Q_min(e->hi.y, hi.y) - Q_max(e->lo.y, lo.y);
			if (ox < 24.0f || oy < 24.0f)
				continue;
			// (walked to: the trigger's middle when it's over this area, else the middle of the overlap)
			Vector at((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, c->z);
			if (at.x < e->lo.x || at.x > e->hi.x || at.y < e->lo.y || at.y > e->hi.y)
				at = Vector((Q_max(e->lo.x, lo.x) + Q_min(e->hi.x, hi.x)) * 0.5f, (Q_max(e->lo.y, lo.y) + Q_min(e->hi.y, hi.y)) * 0.5f, c->z);
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

	// doors in the way: a wall-type func_door / func_door_rotating (taller than a step, thin one way)
	// with learned floor on both sides. Linked both ways; a runner opens it with its button, or waits
	{
		const char *doorClasses[] = { "func_door", "func_door_rotating" };
		for (const char *cls : doorClasses)
		{
			CBaseEntity *pDoor = nullptr;
			while ((pDoor = UTIL_FindEntityByClassname(pDoor, cls)) != nullptr)
			{
				Vector lo = pDoor->pev->absmin, hi = pDoor->pev->absmax, size = hi - lo;
				if (size.z < 48.0f)
					continue;	// (a floor or a step)
				bool thinX = size.x <= 64.0f && size.x < size.y, thinY = size.y <= 64.0f && size.y < size.x;
				if (!thinX && !thinY)
					continue;
				Vector mid = (lo + hi) * 0.5f;
				float half = (thinX ? size.x : size.y) * 0.5f + 24.0f, w = thinX ? size.y : size.x;
				for (float along = 0.0f; along <= 0.61f; along += 0.3f)
				{
					float off = (along == 0.0f ? 0.0f : (along < 0.4f ? -0.3f : 0.3f)) * w;
					Vector a = mid, b = mid;
					if (thinX) { a.x -= half; b.x += half; a.y += off; b.y += off; }
					else { a.y -= half; b.y += half; a.x += off; b.x += off; }
					a.z = b.z = lo.z + 24.0f;
					CNavArea *A = TheNavAreaGrid.GetNavArea(&a), *B = TheNavAreaGrid.GetNavArea(&b);
					if (!A || !B || A == B || Q_fabs(A->GetZ(&a) - B->GetZ(&b)) > 24.0f)
						continue;
					int ia = Index(A), ib = Index(B);
					if (ia < 0 || ib < 0)
						continue;
					NavDirType ab = thinX ? EAST : SOUTH, ba = thinX ? WEST : NORTH;
					if (!A->IsConnected(B, NUM_DIRECTIONS)) { A->ConnectTo(B, ab); s_out[ia].push_back(ib); }
					if (!B->IsConnected(A, NUM_DIRECTIONS)) { B->ConnectTo(A, ba); s_out[ib].push_back(ia); }
					DoorLink dl;
					dl.door = pDoor;
					dl.at = Vector(mid.x, mid.y, lo.z);
					dl.from = ia; dl.to = ib; s_doorLinks.push_back(dl);
					dl.from = ib; dl.to = ia; s_doorLinks.push_back(dl);
					break;
				}
			}
		}
		if (!s_doorLinks.empty())
			CONSOLE_ECHO("Loner-Strike deathrun: %d doors linked across\n", int(s_doorLinks.size() / 2));
	}

	// ledges a jump climbs: touching areas the next one 20-56 units up (over a step, within a duck-jump),
	// the arc's hull clear - nav generation leaves them unlinked and the course stopped below them
	// (starwars' start corridor: a channel, then a 36-48 unit ledge)
	{
		int made = 0, seams = 0;
		for (size_t i = 0; i < n; i++)
		{
			CNavArea *A = s_areas[i];
			const Extent *ea = A->GetExtent();
			if (DeadlyAt(s_hurts, *A->GetCenter()))
				continue;
			for (size_t j = 0; j < n; j++)
			{
				if (i == j)
					continue;
				CNavArea *B = s_areas[j];
				const Extent *eb = B->GetExtent();
				float dx = Q_max(eb->lo.x - ea->hi.x, ea->lo.x - eb->hi.x), dy = Q_max(eb->lo.y - ea->hi.y, ea->lo.y - eb->hi.y);
				if (dx > 4.0f || dy > 4.0f)
					continue;	// (touching: side by side, or corner to corner for a seam)
				Vector dir2;
				if (LedgeClimb(A, B, &dir2, nullptr) != 0)
				{
					if (SeamWalk(A, B))
					{
						Vector d = *B->GetCenter() - *A->GetCenter();
						A->ConnectTo(B, Q_fabs(d.x) > Q_fabs(d.y) ? (d.x > 0 ? EAST : WEST) : (d.y > 0 ? SOUTH : NORTH));
						s_out[i].push_back(int(j));
						seams++;
					}
					continue;
				}
				NavDirType dir = Q_fabs(dir2.x) > Q_fabs(dir2.y) ? (dir2.x > 0 ? EAST : WEST) : (dir2.y > 0 ? SOUTH : NORTH);
				A->ConnectTo(B, dir);
				s_out[i].push_back(int(j));
				made++;
			}
		}
		if (made || seams)
			CONSOLE_ECHO("Loner-Strike deathrun: %d ledges a jump climbs and %d walkable seams linked\n", made, seams);
	}

	// gaps a running jump clears, linked on this nav too (a nav learned without course learning -
	// or by hand - has none): from every edge of every area, every step along it, the jump check
	// the learner uses; a landing on another area not linked yet gets a one-way link
	{
		s_learnHurts = s_hurts;
		int made = 0;
		for (size_t i = 0; i < n && made < 2000; i++)
		{
			CNavArea *area = s_areas[i];
			if (DeadlyAt(s_hurts, *area->GetCenter()))
				continue;
			const Extent *e = area->GetExtent();
			for (int dir = 0; dir < NUM_DIRECTIONS; dir++)
			{
				bool ns = dir == NORTH || dir == SOUTH;
				// (a run-up: the area's depth that way)
				float depth = ns ? e->hi.y - e->lo.y : e->hi.x - e->lo.x;
				if (depth < 2.0f * GenerationStepSize - 1.0f)
					continue;
				float from = ns ? e->lo.x : e->lo.y, to = ns ? e->hi.x : e->hi.y;
				CNavArea *lastTo = nullptr;
				for (float t = from + 0.5f * GenerationStepSize; t < to; t += GenerationStepSize)
				{
					Vector p;
					p.x = ns ? t : (dir == EAST ? e->hi.x : e->lo.x) - (dir == EAST ? 1.0f : -1.0f) * 0.5f * GenerationStepSize;
					p.y = ns ? (dir == SOUTH ? e->hi.y : e->lo.y) - (dir == SOUTH ? 1.0f : -1.0f) * 0.5f * GenerationStepSize : t;
					p.z = area->GetZ(&p);
					Vector land, nrm;
					// (floor just past the edge: no gap here)
					Vector2D dv;
					DirectionToVector2D((NavDirType)dir, &dv);
					float gz;
					Vector past(p.x + dv.x * GenerationStepSize, p.y + dv.y * GenerationStepSize, p.z + 24.0f);
					if (GroundBelow(past, 84.0f, &gz, &nrm) && !DeadlyAt(s_hurts, Vector(past.x, past.y, gz)))
						continue;
					if (!GapJumpAt(p, dir, true, &land, &nrm, false))
						continue;
					Vector at = land + Vector(0, 0, 8.0f);
					CNavArea *dest = TheNavAreaGrid.GetNavArea(&at);
					if (!dest || dest == area || dest == lastTo || area->IsConnected(dest, NUM_DIRECTIONS) || DeadlyAt(s_hurts, *dest->GetCenter()))
						continue;
					// (only across a real gap: the areas don't touch)
					const Extent *f = dest->GetExtent();
					float sep = Q_max(Q_max(f->lo.x - e->hi.x, e->lo.x - f->hi.x), Q_max(f->lo.y - e->hi.y, e->lo.y - f->hi.y));
					if (sep <= 10.0f)
						continue;
					area->ConnectTo(dest, (NavDirType)dir);
					int j = Index(dest);
					if (j >= 0)
						s_out[i].push_back(j);
					lastTo = dest;
					made++;
				}
			}
		}
		if (made)
			CONSOLE_ECHO("Loner-Strike deathrun: %d gap links made on this nav\n", made);
	}

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
	// each door's opener: a button whose wiring moves it, that runners can get to
	for (DoorLink &dl : s_doorLinks)
	{
		CBaseEntity *pDoor = dl.door;
		if (!pDoor)
			continue;
		Vector dm = (pDoor->pev->absmin + pDoor->pev->absmax) * 0.5f;
		float bestD = 1e9f;
		for (size_t ti = 0; ti < s_traps.size(); ti++)
		{
			const Trap &t = s_traps[ti];
			if (t.spotIdx < 0 || s_prog[t.spotIdx] < 0.0f || !InTrap(t, dm, 4.0f))
				continue;
			float d = (t.btn - dl.at).Length();
			if (d < bestD)
			{
				bestD = d;
				dl.trap = int(ti);
			}
		}
	}

	s_ready = true;
	int reach = 0;
	for (const Trap &t : s_traps)
		reach += t.reachT ? 1 : 0;
	CONSOLE_ECHO("Loner-Strike deathrun: course end %.0f %.0f %.0f, %.0f units from the start; %d traps (%d the terrorist can walk to), %d teleport links, %d deadly areas\n",
		s_end.x, s_end.y, s_end.z, s_course, int(s_traps.size()), reach, int(s_teles.size()), int(std::count(s_deadly.begin(), s_deadly.end(), 1)));
}

static void DeathsLoad();

// Rebuilt when the map or its nav changes
static bool Ready()
{
	if (TheNavAreaList.empty())
		return false;
	const char *map = STRING(gpGlobals->mapname);
	if (Q_strcmp(s_map, map) || s_navLoads != g_lsNavLoads || s_navFront != TheNavAreaList.front() || s_navSize != TheNavAreaList.size())
	{
		bool newMap = Q_strcmp(s_map, map) != 0;
		Q_strlcpy(s_map, map);
		s_navLoads = g_lsNavLoads;
		s_navFront = TheNavAreaList.front();
		s_navSize = TheNavAreaList.size();
		Build();
		// (what was learned here last time, from the file)
		if (newMap)
			DeathsLoad();
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
	float teleWaitSince = 0.0f;		// standing in a teleport that doesn't send it anywhere
	int openDoor = -1;				// a shut door on its way (s_doorLinks): its button, or a wait
	int gaveUpArea = -1;			// the floor it last gave up on (another way) and when: back at it soon
	float gaveUpAt = -99.0f;		// after means there is no other way
	Vector gaveUpPos;
	float noWayLogAt = -99.0f;
	float noWayTryAt = -99.0f;
	float noWayJumpUntil = 0.0f;		// a no-other-way jump armed: kept armed till then
	// a jump across a gap, armed by the path
	bool gapArmed = false;
	Vector takeoff, landing;
	Vector2D gapDir;
	float edge = 0.0f;				// the takeoff edge along gapDir
	float jumpedAt = -9.0f;
	float armedAt = -9.0f;			// (the path armed it then: a jump not re-armed lately is let go)
	bool duckJump = false;
	float backUntil = 0.0f;			// backing up for a run-up
	bool tele = false;				// walking into a teleport rather than jumping
	float hopSpeed = 0.0f;			// a hop at this pace (small blocks), 0: a full running jump
	bool climb = false;				// a jump up onto a ledge: from right at its foot
	Vector paceFor;					// the learned gap whose pace was worked out, and that pace
	float paceCached = 0.0f;
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

static void ResetBots()
{
	for (int i = 0; i <= MAX_CLIENTS; i++)
	{
		s_run[i].waypoint = s_run[i].waitTrap = s_run[i].passedTrap = -1;
		s_run[i].gapArmed = false;
	}
	s_act.target = -1;
}

static float RoundStart()
{
	return CSGameRules() ? CSGameRules()->m_fRoundStartTime : 0.0f;
}

// Deaths are kept beside the map's knowledge, maps/<map>.lsdr: a text file, a version line then
// "x y z count trap" a line (the next visit's runners know where people fell or were caught)
static const int LSDR_VERSION = 1;

static void DeathsSave()
{
	char gd[64]{}, path[MAX_OSPATH], tmp[MAX_OSPATH];
	GET_GAME_DIR(gd);
	Q_snprintf(path, sizeof(path), "%s/maps/%s.lsdr", gd, s_map);
	Q_snprintf(tmp, sizeof(tmp), "%s.part", path);
	FILE *f = fopen(tmp, "wt");
	if (!f)
		return;
	fprintf(f, "LSDR %d\n", LSDR_VERSION);
	for (const Death &d : s_deaths)
		fprintf(f, "%.0f %.0f %.0f %d %d\n", d.at.x, d.at.y, d.at.z, d.count, d.trap ? 1 : 0);
	bool ok = !ferror(f);
	fclose(f);
	if (!ok)
	{
		remove(tmp);
		return;
	}
	remove(path);
	if (rename(tmp, path) != 0)
		remove(tmp);
}

static void DeathsLoad()
{
	s_deaths.clear();
	char rel[128];
	Q_snprintf(rel, sizeof(rel), "maps/%s.lsdr", s_map);
	int len = 0;
	byte *data = LOAD_FILE_FOR_ME(rel, &len);
	if (!data)
		return;
	std::vector<char> text((char *)data, (char *)data + len);
	FREE_FILE(data);
	text.push_back('\0');
	char *line = text.data();
	int version = 0;
	bool first = true;
	while (line && *line)
	{
		char *next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		if (first)
		{
			first = false;
			if (sscanf(line, "LSDR %d", &version) != 1 || version != LSDR_VERSION)
				return;	// (another version: start over)
		}
		else
		{
			float x, y, z;
			int count, trap;
			if (sscanf(line, "%f %f %f %d %d", &x, &y, &z, &count, &trap) == 5 && s_deaths.size() < 64
				&& Q_fabs(x) < 16384.0f && Q_fabs(y) < 16384.0f && Q_fabs(z) < 16384.0f)
			{
				Death d;
				d.at = Vector(x, y, z);
				d.count = Q_max(1, Q_min(count, 20));
				d.trap = trap != 0;
				d.when = 0.0f;
				s_deaths.push_back(d);
			}
		}
		line = next;
	}
}

// a death this map: where it was standing last; a trap's if a button went down just before
static void NoteDeath(const Vector &at, bool trap)
{
	for (Death &d : s_deaths)
		if ((d.at - at).IsLengthLessThan(120.0f) && d.trap == trap)
		{
			d.count = Q_min(d.count + 1, 20);
			d.when = gpGlobals->time;
			DeathsSave();
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
	DeathsSave();
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

bool LsDrTeleport(const CNavArea *area, const CNavArea *fromArea)
{
	using namespace LsDr;
	int ia = Index(fromArea), ib = Index(area);
	if (ia < 0 || ib < 0)
		return false;
	for (const Tele &tl : s_teles)
		if (tl.from == ia && tl.to == ib)
			return true;
	return false;
}

// A jump across a gap costs a little more than the walk: people take the floor when there is one
float LsDrLinkCost(const CNavArea *area, const CNavArea *fromArea)
{
	using namespace LsDr;
	if (!fromArea || !area)
		return 0.0f;
	const Extent *ea = fromArea->GetExtent(), *eb = area->GetExtent();
	float sep = Q_max(Q_max(eb->lo.x - ea->hi.x, ea->lo.x - eb->hi.x), Q_max(eb->lo.y - ea->hi.y, ea->lo.y - eb->hi.y));
	if (sep <= 10.0f)
		return 0.0f;
	int ia = Index(fromArea), ib = Index(area);
	for (const Tele &tl : s_teles)
		if (tl.from == ia && tl.to == ib)
			return 0.0f;
	return 250.0f + 2.0f * sep;
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
	return trap ? 1.0f : 1.0f + Q_min(19.0f, 1.5f * float(n));	// (a place people keep falling: well out of the way)
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

	// a fight (the terrorist in sight) is the brain's - when he's near enough to fight. One seen far
	// across the course (open maps: from the start, over the pit) doesn't take a runner off the course:
	// the stock hunt walks straight at him, off the edge
	bool fightNear = false;
	if (IsAttacking() || m_isEnemyVisible)
	{
		CBasePlayer *pEnemy = static_cast<CBasePlayer *>(GetEnemy());
		fightNear = !pEnemy || (pEnemy->pev->origin - pev->origin).IsLengthLessThan(600.0f);
	}
	if (fightNear || IsOnLadder() || !LsFree())
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
	// a shut door on the way: the button that opens it (one runner at a time: the nearest), else a wait
	if (r.openDoor >= int(s_doorLinks.size()))
		r.openDoor = -1;
	if (r.openDoor >= 0)
	{
		const DoorLink &dl = s_doorLinks[r.openDoor];
		if (!DoorShut(dl))
		{
			r.openDoor = -1;
			r.holdUntil = 0.0f;
			r.moveState = -1.0f;	// (back on the course)
			LsWhy("deathrun: door open");
		}
		else
		{
			bool presser = false;
			if (dl.trap >= 0 && ButtonReady(s_traps[dl.trap]))
			{
				const Trap &t = s_traps[dl.trap];
				float mine = (pev->origin - t.spot).Length();
				presser = true;
				for (int c = 1; c <= gpGlobals->maxClients && presser; c++)
				{
					CBasePlayer *p = UTIL_PlayerByIndex(c);
					if (p && p != this && p->IsAlive() && p->m_iTeam == CT && (p->pev->origin - t.spot).Length() < mine - 1.0f)
						presser = false;
				}
				if (presser)
				{
					CBaseEntity *pBtn = t.button;
					Vector o = pev->origin, lo = pBtn->pev->absmin, hi = pBtn->pev->absmax;
					Vector gap(Q_max(Q_max(lo.x - o.x, 0.0f), o.x - hi.x), Q_max(Q_max(lo.y - o.y, 0.0f), o.y - hi.y), Q_max(Q_max(lo.z - o.z, 0.0f), o.z - hi.z));
					if (gap.Length() > 52.0f)
					{
						if (r.moveState != m_stateTimestamp || !HasPath())
						{
							MoveTo(&t.spot, FASTEST_ROUTE);
							r.moveState = m_stateTimestamp;
						}
						LsWhy("deathrun: to the door's button");
						return false;
					}
					SetLookAt("Loner-Strike: door button", &t.btn, PRIORITY_HIGH, 0.5f, false, 3.0f);
					UTIL_MakeVectors(pev->v_angle);
					Vector to = (t.btn - (pev->origin + pev->view_ofs)).Normalize();
					if (DotProduct(to, gpGlobals->v_forward) > 0.9f)
						UseEnvironment();
					ClearMovement();
					LsWhy("deathrun: opens the door");
					return true;
				}
			}
			// (a door you open with use, no button: walk up to it and use it)
			CBaseEntity *pDoor = dl.door;
			if (!presser && pDoor && (pDoor->pev->spawnflags & SF_DOOR_USE_ONLY) && FStringNull(pDoor->pev->targetname))
			{
				Vector dm = (pDoor->pev->absmin + pDoor->pev->absmax) * 0.5f;
				Vector o = pev->origin, lo = pDoor->pev->absmin, hi = pDoor->pev->absmax;
				Vector gap(Q_max(Q_max(lo.x - o.x, 0.0f), o.x - hi.x), Q_max(Q_max(lo.y - o.y, 0.0f), o.y - hi.y), Q_max(Q_max(lo.z - o.z, 0.0f), o.z - hi.z));
				SetLookAt("Loner-Strike: door", &dm, PRIORITY_HIGH, 0.5f, false, 3.0f);
				if (gap.Length() > 40.0f)
				{
					Vector step(dm.x, dm.y, pev->origin.z);
					MoveTowardsPosition(&step);
				}
				else
				{
					UTIL_MakeVectors(pev->v_angle);
					Vector to = (dm - (pev->origin + pev->view_ofs)).Normalize();
					if (DotProduct(to, gpGlobals->v_forward) > 0.8f)
						UseEnvironment();
				}
				LsWhy("deathrun: opens the door (use)");
				return true;
			}
			r.holdUntil = Q_max(r.holdUntil, now + 0.3f);
			r.holdLook = dl.at;
			LsWhy("deathrun: waits for the door");
		}
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
	if (r.waypoint >= int(s_toEnd.size()))
		r.waypoint = -1;
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
	if (a.fighting || IsOnLadder())
		return false;
	// (a runner close by, in sight: a fight; one seen across the course doesn't take it off its
	// button - the traps are the job)
	if (IsAttacking() || m_isEnemyVisible)
	{
		CBasePlayer *pEnemy = GetEnemy();
		if (pEnemy && (pEnemy->pev->origin - pev->origin).IsLengthLessThan(350.0f))
			return false;
		StopAttacking();
		IgnoreEnemies(0.5f);
	}

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
		// (waiting on a floor that carries you - a conveyor, a push, a moving platform's edge - isn't
		// standing still: walk against the drift so the wait doesn't end in the pit it carries you to)
		Vector2D drift = pev->basevelocity.Make2D();
		if (drift.IsLengthGreaterThan(1.0f) && (pev->flags & FL_ONGROUND))
		{
			Vector2D up = -drift.Normalize() * 48.0f;
			Vector against(pev->origin.x + up.x, pev->origin.y + up.y, pev->origin.z);
			MoveTowardsPosition(&against);
		}
		ResetStuckMonitor();
		SetLookAt("Loner-Strike: deathrun wait", &r.holdLook, PRIORITY_MEDIUM, 0.5f);
		return;
	}
	if (m_iTeam != TERRORIST || s_act.fighting || s_act.target < 0 || s_act.target >= int(s_traps.size()))
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

// The floor there now (the nearest point pulled a little way in - an area's edge can be a block's
// edge - or its middle); stay: still under that point in that many seconds (a platform moving
// along, a train, something turning: where it'll be by then)
static bool LsDrFloorThere(const CNavArea *area, const Vector &nearPos, float stay = 0.0f)
{
	Vector p;
	area->GetClosestPointOnArea(&nearPos, &p);
	const Vector *c = area->GetCenter();
	Vector flat(c->x, c->y, p.z);
	float toMid = (flat - p).Length();
	Vector in = toMid > 0.5f ? p + (flat - p) * (Q_min(12.0f, toMid) / toMid) : p;
	Vector spots[2] = { in, *c };
	for (int k = 0; k < 2; k++)
	{
		float z;
		Vector n;
		edict_t *hit = nullptr;
		if (!GroundBelow(Vector(spots[k].x, spots[k].y, area->GetZ(&spots[k]) + 40.0f), 100.0f, &z, &n, &hit))
			continue;
		if (stay <= 0.0f || !hit || FNullEnt(hit) || hit == INDEXENT(0))
			return true;
		entvars_t *pv = VARS(hit);
		if (pv->avelocity.Length() > 1.0f)
		{
			// (spinning about its own upright axis - a turning pillar, a spinning disc - it's floor near the
			// axis all the time: the circle inside it doesn't move. Tipping or swinging: not to jump onto)
			bool yawOnly = Q_fabs(pv->avelocity.x) < 1.0f && Q_fabs(pv->avelocity.z) < 1.0f;
			float inner = 0.35f * Q_min(pv->size.x, pv->size.y) - 4.0f;	// (size may be its turning box: up to 1.41x)
			if (yawOnly && inner > 8.0f && (spots[k] - pv->origin).Make2D().IsLengthLessThan(inner))
				return true;
			continue;
		}
		if (pv->velocity.Length() < 1.0f)
			return true;
		Vector shift = pv->velocity * stay;
		Vector at = spots[k] - shift;
		if (at.x >= pv->absmin.x + 8.0f && at.x <= pv->absmax.x - 8.0f && at.y >= pv->absmin.y + 8.0f && at.y <= pv->absmax.y - 8.0f)
			return true;
	}
	return false;
}

// Floors that come and go (platforms, trap floors, turning stones), per area, by anyone this map:
// when it was last there, when it went, the longest it's been seen gone before it came back, and
// whether it was seen moving. A wait lasts as long as the evidence says it comes back: its own
// cycle (and a bit), 8 s for a floor never seen to move or return - then it's gone: another way
struct FloorLog { float there = -999.0f, goneSince = -1.0f, cycle = 0.0f; bool moves = false, breaks = false; };
static std::vector<FloorLog> s_floorLog;

static void LsDrFloorSeen(const CNavArea *area, bool there, bool moving = false, bool breaks = false)
{
	int i = Index(area);
	if (i < 0)
		return;
	if (s_floorLog.size() != s_areas.size())
		s_floorLog.assign(s_areas.size(), FloorLog());
	FloorLog &f = s_floorLog[i];
	float now = gpGlobals->time;
	if (f.there > now)
		f = FloorLog();	// (a new map: the clock went back)
	f.moves = f.moves || moving;
	f.breaks = f.breaks || breaks;
	if (there)
	{
		if (f.goneSince >= 0.0f && !f.breaks)	// (a breakable's back at the next round: no cycle)
		{
			f.cycle = Q_max(f.cycle, now - f.goneSince);
			f.moves = true;
		}
		f.goneSince = -1.0f;
		f.there = now;
	}
	else if (f.goneSince < 0.0f)
		f.goneSince = now;
}

static float LsDrFloorPatience(const CNavArea *area)
{
	int i = Index(area);
	if (i < 0 || size_t(i) >= s_floorLog.size())
		return 8.0f;
	const FloorLog &f = s_floorLog[i];
	if (f.breaks)
		return 2.0f;	// (a broken floor doesn't come back this round: no waiting for it)
	if (f.cycle > 0.0f)
		return Q_min(30.0f, 1.3f * f.cycle + 1.0f);
	return 8.0f;
}

static bool LsDrFloorBroke(const CNavArea *area)
{
	int i = Index(area);
	return i >= 0 && size_t(i) < s_floorLog.size() && s_floorLog[i].breaks;
}

// Is the floor under this area a breakable (a trap floor that, once broken, is gone for the round)
static bool LsDrFloorBreakable(const CNavArea *area)
{
	const Vector *c = area->GetCenter();
	float z;
	Vector n;
	edict_t *hit = nullptr;
	if (!GroundBelow(Vector(c->x, c->y, c->z + 40.0f), 100.0f, &z, &n, &hit) || !hit || FNullEnt(hit) || hit == INDEXENT(0))
		return false;
	return FClassnameIs(VARS(hit), "func_breakable");
}

// Is the floor under this spot a moving thing right now (a train, a turning door)
static bool LsDrFloorMoving(const CNavArea *area)
{
	const Vector *c = area->GetCenter();
	float z;
	Vector n;
	edict_t *hit = nullptr;
	if (!GroundBelow(Vector(c->x, c->y, c->z + 40.0f), 100.0f, &z, &n, &hit) || !hit || FNullEnt(hit) || hit == INDEXENT(0))
		return false;
	return VARS(hit)->velocity.Length() > 1.0f || VARS(hit)->avelocity.Length() > 1.0f;
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
		// (a ledge a jump climbs: touching, but higher than a step - jumped like a gap, tucked)
		bool climb = false;
		if (!LsDrSeparated(A, B))
		{
			Vector pa, pb;
			const Vector *cb = B->GetCenter();
			A->GetClosestPointOnArea(cb, &pa);
			B->GetClosestPointOnArea(&pa, &pb);
			climb = pb.z - pa.z > 20.0f && pb.z - pa.z <= 56.0f;
			if (!climb)
				continue;
		}
		// a teleport link: walk into the trigger
		int ia = Index(A), ib = Index(B);
		for (const Tele &tl : s_teles)
			if (tl.from == ia && tl.to == ib)
			{
				r.tele = true;
				m_goalPosition = tl.at;
				// (one at a time: Deathrun Manager's semiclip makes runners within 128 units of each other
				// not solid, and what isn't solid doesn't touch triggers - two together in a teleport stay
				// put. The one nearer it goes; the others keep their distance till it's gone)
				float mine = (pev->origin - tl.at).Make2D().Length();
				if (mine < 320.0f)
				{
					bool wait = false, crowded = false;
					for (int c = 1; c <= gpGlobals->maxClients; c++)
					{
						CBasePlayer *p = UTIL_PlayerByIndex(c);
						if (!p || p == this || !p->IsAlive() || p->m_iTeam != CT)
							continue;
						float apart = (p->pev->origin - pev->origin).Make2D().Length();
						if (apart > 170.0f)
							continue;
						crowded = true;
						if ((p->pev->origin - tl.at).Make2D().Length() < mine)
							wait = true;
					}
					if (wait)
					{
						// (back the way it came along its path - on the floor, not off the side - or hold)
						Vector back = m_path[Q_max(0, m_pathIndex - 1)].pos;
						if ((back - pev->origin).Make2D().IsLengthLessThan(40.0f) && m_pathIndex >= 2)
							back = m_path[m_pathIndex - 2].pos;
						m_goalPosition = back;
						r.holdLook = tl.at;
						r.teleWaitSince = 0.0f;
						LsWhy("deathrun: lets one through the teleport");
						return;
					}
					if (crowded)
						r.teleWaitSince = 0.0f;
				}
				// (close: walked straight into it, LsDrDrive - not round the edge of a hole beside it)
				if ((pev->origin - tl.at).Make2D().IsLengthLessThan(200.0f))
				{
					r.gapArmed = true;
					r.hopSpeed = 0.0f;
					r.climb = false;
					r.armedAt = now;
					r.landing = tl.at;
					r.jumpedAt = -9.0f;
				}
				// (in it a while and still here: it's switched off - another way for a while)
				if ((pev->origin - tl.at).Make2D().IsLengthLessThan(40.0f))
				{
					if (r.teleWaitSince <= 0.0f)
						r.teleWaitSince = now;
					else if (now - r.teleWaitSince > 2.5f)
					{
						LsForceBadArea(A, 30.0f);
						DestroyPath();
						r.teleWaitSince = 0.0f;
						LsWhy("deathrun: teleport off");
					}
				}
				else
					r.teleWaitSince = 0.0f;
				break;
			}
		if (r.tele)
			return;
		// a door between the two (a linked door, whichever pair of areas the path crosses it by): shut, its
		// button or a wait (LsDrTick)
		if (!s_doorLinks.empty() && (pev->origin - *B->GetCenter()).Make2D().IsLengthLessThan(400.0f))
		{
			Vector a = *A->GetCenter() + Vector(0, 0, HalfHumanHeight + 4.0f), b = *B->GetCenter() + Vector(0, 0, HalfHumanHeight + 4.0f);
			TraceResult tr;
			UTIL_TraceHull(a, b, ignore_monsters, human_hull, nullptr, &tr);
			if (tr.flFraction < 1.0f && tr.pHit)
			{
				for (size_t k = 0; k < s_doorLinks.size(); k++)
				{
					CBaseEntity *pDoor = s_doorLinks[k].door;
					if (pDoor && pDoor->edict() == tr.pHit)
					{
						r.openDoor = int(k);
						return;
					}
				}
			}
		}
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
		bool there = LsDrFloorThere(B, land);
		LsDrFloorSeen(B, there, there && LsDrFloorMoving(B), there && LsDrFloorBreakable(B));
		// (and still there by the time it lands: a moving platform's timed, not jumped at blindly)
		if (there && !LsDrFloorThere(B, land, 0.6f))
			there = false;
		if (!there)
		{
			if ((pev->origin - takeoff).Make2D().IsLengthLessThan(150.0f))
			{
				if (r.floorWaitSince <= 0.0f)
					r.floorWaitSince = now;
				if (now - r.floorWaitSince > LsDrFloorPatience(B))
				{
					// (gone: a jump over it if there's one, else another way - or, none, a jump / keep waiting)
					if (LsDrJumpGap(i) || LsDrFloorGone(i, B))
						return;
				}
				r.holdUntil = Q_max(r.holdUntil, now + 0.3f);
				r.holdLook = land;
				if (r.floorWaitSince == now && cv_bot_ls_trace.value > 0.0f)
				{
					// (what it's waiting for: the gap's far area and the point looked at)
					char note[160];
					Q_snprintf(note, sizeof(note), "deathrun: floor gap wait: area %d -> %d, land %.0f %.0f %.0f", A->GetID(), B->GetID(), land.x, land.y, land.z);
					LsTraceNote(note);
				}
				LsWhy("deathrun: waits for the floor");
			}
			return;
		}
		r.floorWaitSince = 0.0f;
		// (the pace the learned jump needs: full, or a hop where a full run carries past the landing;
		// worked out once per gap)
		if ((land - r.paceFor).IsLengthGreaterThan(1.0f))
		{
			r.paceFor = land;
			r.paceCached = 0.0f;
			static const float paces[] = { 0.0f, 150.0f, 130.0f, 110.0f };
			Vector l2, n2;
			s_learnHurts = s_hurts;
			for (float sp : paces)
			{
				s_jumpSpeed = sp;
				bool ok = GapJumpVec(Vector(takeoff.x - d.x * 12.0f, takeoff.y - d.y * 12.0f, A->GetZ(&takeoff)), d, true, &l2, &n2, false);
				s_jumpSpeed = 0.0f;
				if (ok)
				{
					r.paceCached = sp;
					break;
				}
			}
		}
		r.gapArmed = true;
		r.hopSpeed = climb ? 150.0f : r.paceCached;
		r.climb = climb;
		r.armedAt = now;
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
		bool there = LsDrFloorThere(B, m_path[i].pos);
		LsDrFloorSeen(B, there, there && LsDrFloorMoving(B), there && LsDrFloorBreakable(B));
		if (there && !LsDrFloorThere(B, m_path[i].pos, 0.4f))
			there = false;
		if (!there)
		{
			// a gap it can jump (a trap floor gone, a broken stretch): the next floor further along
			// within a running jump - jumped, after a moment's look (the careful look longer)
			if (r.floorWaitSince > 0.0f && now - r.floorWaitSince > 0.6f + 1.5f * (1.0f - LsAggro()) && LsDrJumpGap(i))
				return;
			if (r.floorWaitSince <= 0.0f)
				r.floorWaitSince = now;
			if (now - r.floorWaitSince > LsDrFloorPatience(B) && LsDrFloorGone(i, B))
				return;
			r.holdUntil = Q_max(r.holdUntil, now + 0.3f);
			r.holdLook = m_path[i].pos;
			if (r.floorWaitSince == now && cv_bot_ls_trace.value > 0.0f)
			{
				char note[160];
				Q_snprintf(note, sizeof(note), "deathrun: floor stretch wait: area %d, at %.0f %.0f %.0f", B->GetID(), m_path[i].pos.x, m_path[i].pos.y, m_path[i].pos.z);
				LsTraceNote(note);
			}
			LsWhy("deathrun: waits for the floor");
			return;
		}
	}
	r.floorWaitSince = 0.0f;
}

// The floor of path stretch i is gone: a jump from its own spot that really works (checked like the
// learner's), toward the path past it, landing nearer the end; armed for LsDrDrive
bool CCSBot::LsDrJumpGap(int i)
{
	int idx = entindex();
	Runner &r = s_run[idx];
	float now = gpGlobals->time;
	// (people have fallen jumping from here: no, as they'd learn)
	bool trapDeath;
	if (DeathsNear(pev->origin, 150.0f, &trapDeath) >= 2 && !trapDeath)
		return false;
	int here = Index(m_lastKnownArea);
	float hereToEnd = here >= 0 ? s_toEnd[here] : -1.0f;
	float feet = GetFeetZ();
	// each way toward the path's next points past the missing floor: where the floor under it ends
	// that way, and the jump from there checked as the learner checks one (the arc's hull clear, a
	// floor to land on that's not deadly, a fall it lives through), landing somewhere nearer the end
	for (int k = i + 1; k < m_pathLength && k <= i + 5; k++)
	{
		Vector2D d = (m_path[k].pos - pev->origin).Make2D();
		float len = d.Length();
		if (len < 24.0f)
			continue;
		d = d / len;
		float edge = -1.0f;
		for (float a = 0.0f; a <= 160.0f; a += 8.0f)
		{
			float z;
			Vector n;
			Vector q(pev->origin.x + d.x * a, pev->origin.y + d.y * a, feet + 24.0f);
			if (!GroundBelow(q, 60.0f, &z, &n) || DeadlyAt(s_hurts, Vector(q.x, q.y, z)))
			{
				edge = a;
				break;
			}
		}
		if (edge < 0.0f)
			continue;
		// (takes off half a step short of the edge, from the floor it's on)
		Vector take(pev->origin.x + d.x * Q_max(0.0f, edge - 12.0f), pev->origin.y + d.y * Q_max(0.0f, edge - 12.0f), feet);
		Vector land, n;
		s_learnHurts = s_hurts;
		// a full running jump, or else a hop at walking pace: small blocks with gaps between them (temple's
		// strip) are hopped one at a time - a full run carries past them into the next hole
		static const float speeds[] = { 0.0f, 150.0f, 130.0f, 110.0f };
		float hop = -1.0f;
		for (float sp : speeds)
		{
			s_jumpSpeed = sp;
			bool ok = GapJumpVec(take, d, true, &land, &n, false);
			s_jumpSpeed = 0.0f;
			if (!ok)
				continue;
			Vector at = land + Vector(0, 0, 8.0f);
			int li = Index(TheNavAreaGrid.GetNavArea(&at));
			if (li < 0 || s_deadly[li] || (hereToEnd >= 0.0f && s_toEnd[li] >= 0.0f && s_toEnd[li] >= hereToEnd))
				continue;
			hop = sp;
			break;
		}
		if (hop < 0.0f)
			continue;
		r.hopSpeed = hop;
		r.climb = false;
		r.gapArmed = true;
		r.tele = false;
		r.armedAt = now;
		r.landing = land;
		r.gapDir = d;
		r.edge = DotProduct(pev->origin.Make2D(), d) + edge - 4.0f;
		r.duckJump = land.z > feet + 12.0f;
		m_goalPosition = land;
		SetLookAt("Loner-Strike: the jump", &land, PRIORITY_HIGH, 0.3f);
		if (!s_jumpRelaxed)
			LsWhy(hop > 0.0f ? "deathrun: hops the gap" : "deathrun: jumps the gap");	// (no other way: the caller says so)
		return true;
	}
	return false;
}

// Waited the floor's patience and no jump that's sure: another way (the floor marked bad for a minute).
// But if it gave up on this same floor lately and the new path brought it straight back, there is no
// other way: then the best jump there is (the landing at the near edge, not where a full running jump
// comes down), and failing that it keeps waiting, for as long as the floor's been seen to take to come
// back if it has, else till it does - no more marking and coming back. Says why in the trace.
// True: handled (another way, a jump, or holding on).
bool CCSBot::LsDrFloorGone(int i, const CNavArea *B)
{
	Runner &r = s_run[entindex()];
	float now = gpGlobals->time;
	int b = Index(B);
	// (the same floor, or the same spot: a new path round the bad area can come back to the same gap
	// by the area next to it)
	bool cameBack = now - r.gaveUpAt < 90.0f && ((b >= 0 && b == r.gaveUpArea) || (pev->origin - r.gaveUpPos).Make2D().IsLengthLessThan(200.0f));
	if (!cameBack)
	{
		r.gaveUpArea = b;
		r.gaveUpAt = now;
		r.gaveUpPos = pev->origin;
		LsForceBadArea(B, 60.0f);
		DestroyPath();
		r.floorWaitSince = 0.0f;
		LsWhy("deathrun: floor gone, another way");
		return true;
	}
	r.gaveUpAt = now;	// (still no other way)
	// (a jump found is kept armed every think while it runs up and goes - an arm not renewed within
	// 0.35 s is let go; looked for again once a second while there's none)
	if (now < r.noWayJumpUntil || now - r.noWayTryAt > 1.0f)
	{
		r.noWayTryAt = now;
		s_jumpRelaxed = true;
		bool jumped = LsDrJumpGap(i);
		s_jumpRelaxed = false;
		if (jumped)
		{
			if (now >= r.noWayJumpUntil)
				r.noWayJumpUntil = now + 3.0f;
			LsWhy("deathrun: no other way, best jump there is");
			return true;
		}
		r.noWayJumpUntil = 0.0f;
	}
	// (keep waiting: the floor's own cycle if it's been seen, else till it comes back)
	if (now - r.noWayLogAt > 3.0f)
	{
		bool trapDeath;
		bool fell = DeathsNear(pev->origin, 150.0f, &trapDeath) >= 2 && !trapDeath;
		int fi = Index(B);
		bool seen = fi >= 0 && size_t(fi) < s_floorLog.size() && s_floorLog[fi].cycle > 0.0f;
		LsWhy(LsDrFloorBroke(B) ? "deathrun: no other way, the floor's broken for the round"
			: fell ? "deathrun: no other way, people fell jumping here: waits for the floor"
			: seen ? "deathrun: no other way, no jump in reach: waits out the floor's cycle"
			: "deathrun: no other way, no jump in reach: waits till the floor comes back");
		r.noWayLogAt = now;
	}
	r.holdUntil = Q_max(r.holdUntil, now + 0.3f);
	return true;
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
	if (!r.gapArmed || !LsDrOn())
		return;
	CCSBot *me = static_cast<CCSBot *>(bot);
	float now = gpGlobals->time;
	bool ground = (bot->pev->flags & FL_ONGROUND) != 0;
	// (only while the path keeps arming it, or in the air on the jump; not in a fight)
	bool fresh = now - r.armedAt < 0.35f || (!ground && now - r.jumpedAt < 1.5f);
	if (!fresh || bot->m_iTeam != CT || me->IsAttacking() || me->IsOnLadder())
	{
		r.gapArmed = false;
		return;
	}
	Vector2D o = bot->pev->origin.Make2D();
	Vector2D wish;
	if (r.tele)
	{
		// (into the teleport: straight at its middle, no jump)
		wish = r.landing.Make2D() - o;
		float len = wish.Length();
		if (len < 4.0f)
			return;
		wish = wish / len;
	}
	else if (ground)
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
			// (takes off as late as the jump needs: a long one - stepping stones near a running jump's
			// limit - from the very edge; a short one may go early, with room to spare)
			float pace = r.hopSpeed > 0.0f ? r.hopSpeed : 250.0f;
			float need = DotProduct(r.landing.Make2D(), r.gapDir) - r.edge;
			float slack = 0.66f * pace - need;
			float early = Q_min(14.0f + 0.06f * Q_max(0.0f, speed), Q_max(3.0f, 0.5f * slack));
			if (r.climb)
				early = 6.0f;	// (up onto a ledge: from its foot, or the jump meets the wall on the way up)
			if (toEdge < early && toEdge > -20.0f && now - r.jumpedAt > 0.8f)
			{
				// (a full jump wants a run at it; a hop its own pace, give or take)
				bool paced = r.hopSpeed > 0.0f ? (speed > 0.6f * r.hopSpeed && speed < 1.35f * r.hopSpeed) : speed > 200.0f;
				if (paced)
				{
					if (me->IsCrouching())
						me->StandUp();
					if (me->Jump(MUST_JUMP))
						r.jumpedAt = now;
				}
				else if (toEdge < 6.0f)
					r.backUntil = now + 0.45f;
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
	float speed = (!r.tele && r.hopSpeed > 0.0f) ? r.hopSpeed : 250.0f;
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
	// ls_dr ledge <x> <y> <z>: every touching area 20-56 units above the area at a spot, and whether a
	// jump climbs onto it (which check fails); ledges are linked at map start when it does
	if (CMD_ARGC() >= 5 && FStrEq(CMD_ARGV(1), "ledge"))
	{
		Vector p(Q_atof(CMD_ARGV(2)), Q_atof(CMD_ARGV(3)), Q_atof(CMD_ARGV(4)));
		CNavArea *A = TheNavAreaGrid.GetNavArea(&p);
		if (!A)
			A = TheNavAreaGrid.GetNearestNavArea(&p);
		if (!A)
		{
			CONSOLE_ECHO("ls_dr ledge: no area at %.0f %.0f %.0f\n", p.x, p.y, p.z);
			return;
		}
		static const char *why[] = { "climbs", "not side by side", "not 20-56 up", "already linked", "deadly on top", "no room on top (crouched)", "the way up is blocked (crouched)" };
		const Extent *ea = A->GetExtent();
		CONSOLE_ECHO("ls_dr ledge from area #%d (%.0f %.0f - %.0f %.0f, z %.0f):\n", A->GetID(), ea->lo.x, ea->lo.y, ea->hi.x, ea->hi.y, A->GetCenter()->z);
		int shown = 0;
		for (CNavArea *B : TheNavAreaList)
		{
			if (B == A)
				continue;
			const Extent *eb = B->GetExtent();
			float dx = Q_max(eb->lo.x - ea->hi.x, ea->lo.x - eb->hi.x), dy = Q_max(eb->lo.y - ea->hi.y, ea->lo.y - eb->hi.y);
			if (dx > 4.0f || dy > 4.0f || Q_fabs(B->GetCenter()->z - A->GetCenter()->z) > 80.0f)
				continue;
			int r = LedgeClimb(A, B, nullptr, nullptr);
			bool seam = r != 0 && SeamWalk(A, B);
			CONSOLE_ECHO("  -> #%d (%.0f %.0f - %.0f %.0f, z %.0f): %s%s%s\n", B->GetID(), eb->lo.x, eb->lo.y, eb->hi.x, eb->hi.y, B->GetCenter()->z, why[r],
				seam ? "; a walkable seam (linked)" : "", A->IsConnected(B, NUM_DIRECTIONS) ? "; linked" : "");
			if (++shown >= 16)
				break;
		}
		return;
	}
	// ls_dr ents <x> <y> <z> <r>: entities round a spot, their solid type and box (a trigger's state)
	if (CMD_ARGC() >= 6 && FStrEq(CMD_ARGV(1), "ents"))
	{
		Vector p(Q_atof(CMD_ARGV(2)), Q_atof(CMD_ARGV(3)), Q_atof(CMD_ARGV(4)));
		CBaseEntity *pEnt = nullptr;
		while ((pEnt = UTIL_FindEntityInSphere(pEnt, p, Q_atof(CMD_ARGV(5)))) != nullptr)
			CONSOLE_ECHO("  #%d %s '%s' solid %d movetype %d box %.0f %.0f %.0f .. %.0f %.0f %.0f effects %d\n", pEnt->entindex(), STRING(pEnt->pev->classname),
				STRING(pEnt->pev->targetname), pEnt->pev->solid, pEnt->pev->movetype, pEnt->pev->absmin.x, pEnt->pev->absmin.y, pEnt->pev->absmin.z,
				pEnt->pev->absmax.x, pEnt->pev->absmax.y, pEnt->pev->absmax.z, pEnt->pev->effects);
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
