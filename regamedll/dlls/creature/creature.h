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

// Loner-Strike creatures: one framework, providers behind it.
//
// A creature is anything a mode puts in the world to fight or flee from that isn't a player:
// zombies, aliens, wildlife, turrets. Every creature, whoever provides it, is reached through
// ILsCreature (registered in one list), so the bots, the commands and the plugins treat them
// alike. Providers:
//   native  - our own ls_creature entity (CLsCreature): moves on the bot nav mesh with
//             CCreatureImprov (the hostage AI's locomotion, as a sibling class, so hostages
//             don't change), sees and hears players, runs simple behaviours
//   adopted - entities some other code made (MonsterMod's monster_*), wrapped by classname so
//             they're in the list too: alive, health, hostility; they keep their own brain
// Types (health, speed, attack, behaviour...) come from creatures.ini (see CREATURES.md in the
// Loner-Strike tools repo). Nothing here runs until a creature exists: no creatures, stock game.

#pragma once

#include "hostage/hostage_improv.h"

class CLsCreature;

enum LsCreatureCategory
{
	LSC_INFECTED = 0,	// zombies, headcrab-likes, carriers
	LSC_ALIEN,			// hostile aliens: ranged, melee
	LSC_WILDLIFE,		// passive, territorial, predators
	LSC_MECHANICAL,		// turrets, drones, robots, security
	LSC_BOSS,			// big, multi-phase (phases later)
	LSC_SPECIAL,		// flying, teleporting, support, summoning (later)
	LSC_CATEGORY_COUNT
};

// What it does when it has nobody to fight (its "home" behaviour), plus the two it switches to
enum LsCreatureBehaviour
{
	LSB_IDLE = 0,		// stand where it is
	LSB_WANDER,			// stroll around its home spot
	LSB_PATROL,			// walk a path_corner chain (or the nav areas around home without one)
	LSB_GUARD,			// hold its home spot, fight only what comes close
	LSB_CHASE,			// hunt: go for the nearest player it knows of, even unseen
	LSB_FLEE,			// run from the threat (hurt, or a passive animal)
	LSB_COUNT
};

enum LsCreatureAttackType
{
	LSA_NONE = 0,
	LSA_MELEE,			// claw, bite: a hit at close range after a wind-up
	LSA_RANGED,			// spit, shot: a trace with spread at range (a turret is ranged and doesn't move)
};

enum LsCreatureTargeting
{
	LST_NEAREST = 0,
	LST_WEAKEST,		// lowest health it can see
	LST_ATTACKER,		// whoever hurt it last, else the nearest
	LST_RANDOM,
};

enum LsCreatureAnim
{
	LSAN_IDLE = 0,
	LSAN_WALK,
	LSAN_RUN,
	LSAN_ATTACK,
	LSAN_FLINCH,
	LSAN_DIE,
	LSAN_COUNT
};

enum LsCreatureSound
{
	LSS_IDLE = 0,
	LSS_ALERT,			// noticed someone
	LSS_ATTACK,			// the swing / the shot
	LSS_HIT,			// the swing landed
	LSS_MISS,
	LSS_PAIN,
	LSS_DIE,
	LSS_COUNT
};

const int LS_CREATURE_MAX_TYPES = 32;
const int LS_CREATURE_MAX_SOUNDS = 6;		// per kind
const int LS_CREATURE_MAX_LIST = 128;		// registered creatures (alive or dead), all providers

// One creature type: what creatures.ini says about it (or the built-in defaults)
struct LsCreatureType
{
	char name[32];
	char display[48];
	int category;

	float health;
	float speed;			// running
	float walkSpeed;
	float aggression;		// 0 never starts a fight (fights back or flees), 1 attacks anyone it notices
	float detectRadius;		// how far it sees
	float detectFov;		// degrees (360 = all round)
	float hearScale;		// times the bots' hearing ranges (0 = deaf)

	int attack;				// LsCreatureAttackType
	float damage;
	float attackRange;
	float attackRate;		// attacks a second
	float attackDelay;		// wind-up before the hit lands
	float spread;			// ranged: degrees

	float fleeHealth;		// flee below this part of its health (0 = never)
	int behaviour;			// LsCreatureBehaviour at home
	float guardRadius;		// guard/wander/patrol: how far from home it goes
	int targeting;			// LsCreatureTargeting
	int hates;				// 0 everyone, 1 terrorists only, 2 CTs only
	float respawn;			// seconds after death it comes back at home (0 = never)
	float corpseTime;		// seconds the body stays
	bool moves;				// false: a turret
	bool canClimb;			// may path up and down ladders (no built-in type does)
	int frags;				// score for the killer
	int money;				// money for the killer

	char models[256];		// candidates, first that exists wins (space separated)
	char model[96];			// the one chosen
	int skin;
	int body;
	float scale;
	Vector hullMin, hullMax;
	bool glow;
	Vector glowColor;
	bool fallbackGlow;		// only when a later model candidate was chosen (a tinted stand-in)
	Vector fallbackGlowColor;
	int bloodColor;

	char anim[LSAN_COUNT][32];			// sequence names ("" = by activity)
	char sound[LSS_COUNT][160];			// space separated lists, files that exist are kept

	// filled in at precache
	bool precached;
	bool usingFallback;		// the first model candidate wasn't there
	bool disabled;			// the file dropped it on a reload: no new spawns (the slot stays for live ones)
	string_t iszName;
	string_t iszModel;		// engine strings: the engine keeps the pointers it's given at precache
	string_t iszSound[LSS_COUNT][LS_CREATURE_MAX_SOUNDS];
	int soundCount[LSS_COUNT];
	char soundFile[LSS_COUNT][LS_CREATURE_MAX_SOUNDS][64];
};

// What every creature exposes, whoever provides it
class ILsCreature
{
public:
	virtual ~ILsCreature() {}

	virtual CBaseEntity *Entity() const = 0;					// for identity (and our own: everything); null once gone
	virtual entvars_t *Vars() const = 0;						// its entvars: the safe way to read position etc. for any provider
	virtual const char *Provider() const = 0;					// "ls", "adopted"
	virtual const LsCreatureType *Type() const = 0;				// may be null (an adopted one without a type)
	virtual int Category() const = 0;
	virtual bool IsAlive() const = 0;
	virtual float Health() const = 0;
	virtual void SetTarget(CBaseEntity *pTarget) = 0;			// null = let it choose
	virtual CBaseEntity *GetTarget() const = 0;
	virtual void SetBehaviour(int behaviour, const Vector *spot) = 0;	// spot = new home (guard, wander)
	virtual int GetBehaviour() const = 0;
	virtual float Threat() const = 0;							// how dangerous, for a bot choosing whom to shoot (0..1)
	virtual bool IsHostileTo(CBaseEntity *pOther) const = 0;	// would it attack this player
	virtual bool LastNoise(Vector *pos, float *when, float *range) const = 0;	// the last sound it made
	virtual void Kill() = 0;									// dies as if killed by the world
};

// A provider: who can make creatures of a type
class ILsCreatureProvider
{
public:
	virtual ~ILsCreatureProvider() {}

	virtual const char *Name() const = 0;
	virtual bool Available() const = 0;
	virtual CBaseEntity *Spawn(int typeIndex, const Vector &origin, float yaw, const char *keys) = 0;
};

// The hostage AI's locomotion for a creature: nav mesh paths (CNavPathFollower), ladders, jumps
// over ledges, local obstacle feelers, unsticking, doors and breakables in the way, keeping its
// distance from others. A sibling of CHostageImprov (the code is the same where it can be), so
// hostages keep their exact behaviour. Without a nav mesh it walks straight at its goal.
class CCreatureImprov: public CImprov
{
public:
	CCreatureImprov(CLsCreature *pCreature);
	~CCreatureImprov() {}

	virtual void OnMoveToSuccess(const Vector &goal);
	virtual void OnMoveToFailure(const Vector &goal, MoveToFailureType reason);
	virtual void OnInjury(float amount) {}
	virtual bool IsAlive() const;
	virtual void MoveTo(const Vector &goal);
	virtual void LookAt(const Vector &target) { m_isLookingAt = true; m_viewGoal = target; }
	virtual void ClearLookAt() { m_isLookingAt = false; }
	virtual void FaceTo(const Vector &goal) { m_isFacingTo = true; m_faceGoal = goal; }
	virtual void ClearFaceTo() { m_isFacingTo = false; }
	virtual bool IsAtMoveGoal(float error = 20.0f) const;
	virtual bool HasLookAt() const { return m_isLookingAt; }
	virtual bool HasFaceTo() const { return m_isFacingTo; }
	virtual bool IsAtFaceGoal() const { return false; }
	virtual bool IsFriendInTheWay(const Vector &goalPos) const { return false; }
	virtual bool IsFriendInTheWay(CBaseEntity *myFriend, const Vector &goalPos) const { return false; }
	virtual void MoveForward() {}
	virtual void MoveBackward() {}
	virtual void StrafeLeft() {}
	virtual void StrafeRight() {}
	virtual bool Jump();
	virtual void Crouch();
	virtual void StandUp();
	virtual void TrackPath(const Vector &pathGoal, float deltaT);
	virtual void StartLadder(const CNavLadder *ladder, NavTraverseType how, const Vector *approachPos, const Vector *departPos) { m_traversingLadder = true; }
	virtual bool TraverseLadder(const CNavLadder *ladder, NavTraverseType how, const Vector *approachPos, const Vector *departPos, float deltaT);
	virtual bool GetSimpleGroundHeightWithFloor(const Vector *pos, float *height, Vector *normal = nullptr);
	virtual void Run() { m_moveType = Running; }
	virtual void Walk() { m_moveType = Walking; }
	virtual void Stop();
	virtual float GetMoveAngle() const { return m_moveAngle; }
	virtual float GetFaceAngle() const { return m_moveAngle; }
	virtual const Vector &GetFeet() const;
	virtual const Vector &GetCentroid() const;
	virtual const Vector &GetEyes() const;
	virtual bool IsRunning() const { return m_moveType == Running; }
	virtual bool IsWalking() const { return m_moveType == Walking; }
	virtual bool IsStopped() const { return m_moveType == Stopped; }
	virtual bool IsCrouching() const { return m_isCrouching; }
	virtual bool IsJumping() const { return !m_jumpTimer.IsElapsed(); }
	virtual bool IsUsingLadder() const { return false; }
	virtual bool IsOnGround() const;
	virtual bool IsMoving() const { return m_actualVel.IsLengthGreaterThan(10.0f); }
	virtual bool CanRun() const { return true; }
	virtual bool CanCrouch() const { return true; }
	virtual bool CanJump() const { return true; }
	virtual bool IsVisible(const Vector &pos, bool testFOV = false) const;
	virtual bool IsPlayerLookingAtMe(CBasePlayer *pOther, float cosTolerance = 0.95f) const { return false; }
	virtual CBasePlayer *IsAnyPlayerLookingAtMe(int team = 0, float cosTolerance = 0.95f) const { return nullptr; }
	virtual CBasePlayer *GetClosestPlayerByTravelDistance(int team = 0, float *range = nullptr) const { return nullptr; }
	virtual CNavArea *GetLastKnownArea() const { return m_lastKnownArea; }
	virtual void OnUpdate(float deltaT) {}
	virtual void OnUpkeep(float deltaT);
	virtual void OnReset();
	virtual void OnGameEvent(GameEventType event, CBaseEntity *pEntity = nullptr, CBaseEntity *pOther = nullptr) {}
	virtual void OnTouch(CBaseEntity *pOther);

public:
	enum MoveType { Stopped, Walking, Running };

	void SetSpeeds(float walk, float run) { m_walkSpeed = walk; m_runSpeed = run; }
	bool FaceTowards(const Vector &target, float deltaT);	// turn the body; true when facing it
	void ApplyForce(const Vector &force) { m_vel.x += force.x; m_vel.y += force.y; }
	const Vector &GetActualVelocity() const { return m_actualVel; }
	const Vector &GetMoveGoal() const { return m_moveGoal; }
	bool HasPath() const { return m_path.IsValid(); }
	bool HasGoal() const { return m_hasGoal; }
	void ForceRepath() { m_path.Invalidate(); m_repathFails = 0; m_repathTimer.Invalidate(); m_directStuckTimer.Invalidate(); }
	bool IsStuck() const { return m_follower.IsStuck() || (m_directStuckTimer.HasStarted() && m_directStuckTimer.GetElapsedTime() > 1.5f); }
	void SetMoveAngle(float angle) { m_moveAngle = angle; }

private:
	void UpdatePosition(float deltaT);
	void MoveTowards(const Vector &pos, float deltaT);
	bool DiscontinuityJump(float ground, bool onlyJumpDown = false, bool mustJump = false);
	void ResetJump();
	void Wiggle();
	void ClearPath();
	float MaxSpeed() const;

	CLsCreature *m_creature;
	CNavArea *m_lastKnownArea;
	mutable Vector m_centroid;
	mutable Vector m_eye;

	Vector2D m_vel;
	Vector m_actualVel;
	Vector m_lastPosition;
	Vector m_moveGoal;
	bool m_hasGoal;
	MoveType m_moveType;
	float m_walkSpeed;
	float m_runSpeed;
	float m_moveAngle;
	bool m_isCrouching;
	CountdownTimer m_minCrouchTimer;

	Vector m_viewGoal;
	bool m_isLookingAt;
	Vector m_faceGoal;
	bool m_isFacingTo;

	CNavPath m_path;
	CNavPathFollower m_follower;
	CountdownTimer m_repathTimer;		// a failed path isn't tried again every tick
	int m_repathFails;					// failures in a row to about the same goal: the wait doubles (1, 2, 4, 8 s)
	Vector m_failedGoal;
	CountdownTimer m_directCheckTimer;	// the "straight at it" sight check, a few times a second
	bool m_directOk;
	IntervalTimer m_directStuckTimer;	// no path: how long it's made no headway

	NavRelativeDirType m_wiggleDirection;
	CountdownTimer m_wiggleTimer;
	CountdownTimer m_wiggleJumpTimer;
	CountdownTimer m_inhibitObstacleAvoidance;
	CountdownTimer m_jumpTimer;
	bool m_hasJumped;
	bool m_hasJumpedIntoAir;
	Vector m_jumpTarget;
	CountdownTimer m_clearPathTimer;
	bool m_traversingLadder;
};

// Our own creature entity, "ls_creature". A map can place it (keys below), the server command
// and plugins can make it (create_entity + keys + DispatchSpawn, or ls_creature spawn).
// Keys: type, behaviour, path (first path_corner of a patrol; "patrol" also works), radius,
//       respawn, health, speed, damage, aggression, detect, hates, persist (1 = survives round
//       restarts), plus the usual origin/angles
class CLsCreature: public CBaseMonster
{
public:
	virtual void Spawn();
	virtual void Precache();
	virtual void KeyValue(KeyValueData *pkvd);
	virtual int ObjectCaps();
	virtual void Restart();
	virtual int Classify() { return CLASS_ALIEN_MONSTER; }
	virtual void TraceAttack(entvars_t *pevAttacker, float flDamage, Vector vecDir, TraceResult *ptr, int bitsDamageType);
	virtual BOOL TakeDamage(entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType);
	virtual void Killed(entvars_t *pevAttacker, int iGib);
	virtual BOOL IsAlive() { return pev->deadflag == DEAD_NO && pev->health > 0.0f; }
	virtual int BloodColor() { return m_bloodColor; }
	virtual void Touch(CBaseEntity *pOther);
#ifdef REGAMEDLL_API
	virtual void OnDestroy();
#endif

public:
	void EXPORT CreatureThink();
	void CreatureThinkBody();

	// what the framework asks of it (LsNativeCreature forwards here)
	const LsCreatureType *GetType() const;
	int GetTypeIndex() const { return m_type; }
	void SetTarget(CBaseEntity *pTarget);
	CBaseEntity *GetTarget() const { return m_target; }
	void SetBehaviour(int behaviour, const Vector *spot);
	int GetBehaviour() const { return m_behaviour; }
	bool IsHostileTo(CBaseEntity *pOther) const;
	bool LastNoise(Vector *pos, float *when, float *range) const;
	void KillQuietly();

	// for the locomotion
	void OnMoveToSuccess(const Vector &goal);
	void OnMoveToFailure(const Vector &goal, int reason);

	// heard something (from the bots' game events)
	void OnGameEvent(GameEventType event, CBaseEntity *pEntity, CBaseEntity *pOther);

private:
	enum Mode { MODE_HOME, MODE_CHASE, MODE_ATTACK, MODE_SEARCH, MODE_FLEE, MODE_DEAD };

	void Setup();					// from the type and the keys, at spawn and at each respawn
	void ComeBack();
	void UpdateSenses();
	void UpdateHome();
	void UpdateChase();
	void UpdateAttack();
	void UpdateSearch();
	void UpdateFlee();
	void UpdateAnimation();
	bool CanSee(CBaseEntity *pOther, bool alert, bool thorough) const;
	bool WantsToFight(CBaseEntity *pOther, float dist) const;
	bool SkipsTarget(CBaseEntity *pOther) const { return pOther == m_skipTarget && gpGlobals->time < m_skipUntil; }
	void Notice(CBaseEntity *pOther, bool alert);
	void StartAttack();
	void LandAttack();
	void StartFlee(const Vector &from);
	void GoHome();
	bool PickWanderSpot(Vector *spot, float radius) const;
	bool PickFleeSpot(Vector *spot, const Vector &from) const;
	CBaseEntity *NextPatrolCorner(CBaseEntity *pFrom) const;
	void SetAnim(int anim, bool restart = false);
	void ResolveAnims();			// idle/walk/run sequences and their ground speeds, once per model
	int PickSequence(int anim);
	void UpdateStuck();				// no headway: jump, repath, then back to an earlier spot unseen
	bool SeenByAnyone(const Vector &spot) const;
	bool FreeSpot(const Vector &spot) const;
	bool FindFreeSpot(Vector *spot) const;
	void PlaySound(int kind, float volume = VOL_NORM);
	void MakeNoise(float range);
	float Dist2D(CBaseEntity *pOther) const { return (pOther->pev->origin - pev->origin).Make2D().Length(); }

public:
	int m_type;
	int m_behaviour;			// home behaviour now
	int m_homeBehaviour;		// as spawned (round restarts go back to it)
	Vector m_home;
	Vector m_homeAngles;
	Vector m_spawnOrigin;
	Vector m_spawnAngles;
	float m_guardRadius;
	float m_respawn;
	bool m_persist;				// survives round restarts (map-placed ones always do)
	bool m_fromMap;
	string_t m_patrolStart;

	// per-creature overrides from keys (0 = the type's)
	float m_keyHealth, m_keySpeed, m_keyDamage, m_keyAggression, m_keyDetect;
	int m_keyHates;

	CCreatureImprov *m_improv;

private:
	Mode m_mode;
	EHANDLE m_target;
	EHANDLE m_forcedTarget;		// told whom to attack (ls_creature target)
	EHANDLE m_skipTarget;		// one it couldn't get to (up a ladder): left alone a while
	float m_skipUntil;
	float m_cantReachSince;		// close below the target and not getting higher, since (0 = not)
	float m_cantReachZ;
	EHANDLE m_lastAttacker;
	EHANDLE m_noticing;			// seen, not yet sure
	float m_noticeStart;
	Vector m_targetLastSeen;
	float m_targetSeenAt;
	Vector m_searchSpot;
	float m_searchUntil;
	Vector m_fleeFrom;
	float m_fleeUntil;
	float m_fleeCooldown;
	EHANDLE m_patrolCorner;
	float m_waitUntil;			// standing still at a wander/patrol stop
	float m_nextSense;
	float m_nextFullThink;
	float m_nextRepath;
	Vector m_lastMoveTo;
	float m_attackStart;		// wind-up started (0 = not attacking)
	bool m_attackLanded;
	float m_nextAttack;
	float m_nextIdleSound;
	float m_painSoundAt;
	float m_flinchUntil;
	float m_deadAt;
	int m_anim;
	int m_seq[3];				// LSAN_IDLE, LSAN_WALK, LSAN_RUN
	float m_seqGround[3];		// their linear speeds (0 = in place)
	bool m_hasRun;
	bool m_animMoving;
	float m_attackFramerate;	// the swing played to fit the attack rate
	float m_attackAnimEnd;
	Vector m_progressPos;		// stuck watch
	float m_nextProgress;
	int m_stuckSecs;
	Vector m_crumbs[4];			// spots it walked through lately (where it goes back to when stuck)
	int m_crumbCount;
	int m_crumbNext;
	Vector m_noisePos;
	float m_noiseAt;
	float m_noiseRange;
	float m_runSpeed;
	float m_walkSpeed;
	float m_damage;
	float m_aggression;
	float m_detect;
	int m_hates;
};

// The registry and what the rest of the game DLL calls
void LsCreatures_RegisterCVars();
void LsCreaturesPrecache();										// CWorld::Precache: types in ls_creature_precache
void LsCreaturesServerActivate();								// the map runs: spawning by command allowed
void LsCreaturesServerDeactivate();
void LsCreaturesOnEvent(GameEventType event, CBaseEntity *pEntity, CBaseEntity *pOther);	// from CBotManager::OnEvent
void LsCreature_f();											// "ls_creature" server command

int LsCreatureTypeIndex(const char *name);						// -1 when unknown
LsCreatureType *LsCreatureTypeAt(int index);
bool LsCreaturePrecacheType(int index);							// only during map load; true when ready
CBaseEntity *LsCreatureSpawn(const char *type, const Vector &origin, float yaw, const char *keys);

void LsCreatureRegister(ILsCreature *pCreature);				// the list owns it
void LsCreatureUnregister(CBaseEntity *pEntity);
int LsCreatureCount();											// registered, alive or dead (iterate with LsCreatureAt)
ILsCreature *LsCreatureAt(int i);								// null for a gone one
ILsCreature *LsCreatureOf(CBaseEntity *pEntity);				// null when it isn't a creature
bool LsCreaturesExist();										// cheap: anything registered at all
int LsCreatureShouldCollide(edict_t *pentTouched, edict_t *pentOther);	// NEW_DLL_FUNCTIONS: creatures pass through each other

extern cvar_t ls_creature_debug;

// Loner-Strike watchdog: where the game DLL was when a server frame stops coming (a hang). The main
// thread leaves cheap marks (a string and an entity index) in a ring; a second thread notices
// when frames stop for ls_watchdog seconds and writes the last marks to ls_watchdog.log in the
// game folder. A last mark that's an "end" means the time went outside the game DLL (the engine
// or a plugin).
void LsMark(const char *where, int ent = 0);
void LsWatchdogFrame();						// StartFrame: a frame came
void LsWatchdogStart();						// GameDLLInit
