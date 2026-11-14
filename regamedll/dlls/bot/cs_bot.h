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

#pragma once


#include "bot/cs_gamestate.h"
#include "bot/cs_bot_manager.h"
#include "bot/cs_bot_chatter.h"

const int MAX_BUY_WEAPON_PRIMARY   = 13;
const int MAX_BUY_WEAPON_SECONDARY = 3;

enum
{
	BOT_PROGGRESS_DRAW = 0, // draw status bar progress
	BOT_PROGGRESS_START,    // init status bar progress
	BOT_PROGGRESS_HIDE,     // hide status bar progress
};

class CCSBot;
struct LsRoute;
class BotChatterInterface;

class BotState
{
public:
	virtual void OnEnter(CCSBot *me) {}
	virtual void OnUpdate(CCSBot *me) {}
	virtual void OnExit(CCSBot *me) {}
	virtual const char *GetName() const = 0;
};

class IdleState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual const char *GetName() const { return "Idle"; }
};

class HuntState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "Hunt"; }

	void ClearHuntArea() { m_huntArea = nullptr; }
	CNavArea *GetHuntArea() { return m_huntArea; }

private:
	CNavArea *m_huntArea;
};

class AttackState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "Attack"; }

	void SetCrouchAndHold(bool crouch) { m_crouchAndHold = crouch; }
	void StopAttacking(CCSBot *me);

protected:
	enum DodgeStateType
	{
		STEADY_ON,
		SLIDE_LEFT,
		SLIDE_RIGHT,
		JUMP,
		NUM_ATTACK_STATES
	} m_dodgeState;

	float m_nextDodgeStateTimestamp;
	CountdownTimer m_repathTimer;
	float m_scopeTimestamp;
	bool m_haveSeenEnemy;
	float m_lsVisibleFor = 0.0f;			// Loner-Strike: seconds the enemy has been in view this fight
	float m_lsVisibleAt = 0.0f;
	bool m_isEnemyHidden;
	float m_reacquireTimestamp;
	float m_shieldToggleTimestamp;
	bool m_shieldForceOpen;
	float m_pinnedDownTimestamp;
	bool m_crouchAndHold;
	bool m_didAmbushCheck;
	bool m_dodge;
	bool m_firstDodge;
	bool m_isCoward;
	CountdownTimer m_retreatTimer;
};

class InvestigateNoiseState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "InvestigateNoise"; }

private:
	void AttendCurrentNoise(CCSBot *me);
	Vector m_checkNoisePosition;
};

class BuyState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "Buy"; }

private:
	bool m_isInitialDelay;
	int m_prefRetries;
	int m_prefIndex;
	int m_retries;
	bool m_doneBuying;
	bool m_lsEquippedDone = false;				// Loner-Strike: the gun out once in freeze time
	float m_lsPastFreezeAt = -1.0f;				// Loner-Strike: first think in the buy state past the freeze time
	bool m_buyDefuseKit;
	bool m_buyGrenade;
	bool m_buyShield;
	bool m_buyPistol;
};

class MoveToState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "MoveTo"; }

	void SetGoalPosition(const Vector &pos) { m_goalPosition = pos; }
	void SetRouteType(RouteType route) { m_routeType = route; }

private:
	Vector m_goalPosition;
	RouteType m_routeType;
	bool m_radioedPlan;
	bool m_askedForCover;
};

class FetchBombState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual const char *GetName() const { return "FetchBomb"; }
};

class PlantBombState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "PlantBomb"; }
};

class DefuseBombState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "DefuseBomb"; }
};

class HideState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "Hide"; }

public:
	void SetHidingSpot(const Vector &pos) { m_hidingSpot = pos; }
	const Vector &GetHidingSpot() const { return m_hidingSpot; }

	void SetSearchArea(CNavArea *area) { m_searchFromArea = area; }
	CNavArea *GetSearchArea() { return m_searchFromArea; }
	void SetSearchRange(float range) { m_range = range; }

	void SetDuration(float time) { m_duration = time; }
	void SetHoldPosition(bool hold) { m_isHoldingPosition = hold; }

	bool IsAtSpot() const { return m_isAtSpot; }

private:
	CNavArea *m_searchFromArea;
	float m_range;

	Vector m_hidingSpot;
	bool m_isAtSpot;
	float m_duration;
	bool m_isHoldingPosition;
	float m_holdPositionTime;
	bool m_heardEnemy;

	float m_firstHeardEnemyTime;
	int m_retry;
	Vector m_leaderAnchorPos;
};

class EscapeFromBombState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "EscapeFromBomb"; }
};

class FollowState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "Follow"; }

	void SetLeader(CBasePlayer *pLeader) { m_leader = pLeader; }

private:
	void ComputeLeaderMotionState(float leaderSpeed);

	EntityHandle<CBasePlayer> m_leader;
	Vector m_lastLeaderPos;
	bool m_isStopped;
	float m_stoppedTimestamp;

	enum LeaderMotionStateType
	{
		INVALID,
		STOPPED,
		WALKING,
		RUNNING

	} m_leaderMotionState;

	IntervalTimer m_leaderMotionStateTime;

	bool m_isSneaking;
	float m_lastSawLeaderTime;
	CountdownTimer m_repathInterval;

	IntervalTimer m_walkTime;
	bool m_isAtWalkSpeed;

	float m_waitTime;
	CountdownTimer m_idleTimer;
};

class UseEntityState: public BotState
{
public:
	virtual void OnEnter(CCSBot *me);
	virtual void OnUpdate(CCSBot *me);
	virtual void OnExit(CCSBot *me);
	virtual const char *GetName() const { return "UseEntity"; }

	void SetEntity(CBaseEntity *pEntity) { m_entity = pEntity; }

private:
	EntityHandle<CBaseEntity> m_entity;
};

// The Counter-strike Bot
class CCSBot: public CBot
{
public:
	CCSBot();																											// constructor initializes all values to zero
	virtual BOOL TakeDamage(entvars_t *pevInflictor, entvars_t *pevAttacker, float flDamage, int bitsDamageType);		// invoked when injured by something (EXTEND) - returns the amount of damage inflicted
	virtual void Killed(entvars_t *pevAttacker, int iGib);																// invoked when killed (EXTEND)
	virtual void RoundRespawn();
	virtual void Blind(float duration, float holdTime, float fadeTime, int alpha = 255);								// player blinded by a flashbang
	virtual void OnTouchingWeapon(CWeaponBox *box);																		// invoked when in contact with a CWeaponBox

	virtual bool Initialize(const BotProfile *profile);						// (EXTEND) prepare bot for action
	virtual void SpawnBot();												// (EXTEND) spawn the bot into the game

	virtual void Upkeep();													// lightweight maintenance, invoked frequently
	virtual void Update();													// heavyweight algorithms, invoked less often

	virtual void Walk();
	virtual bool Jump(bool mustJump = false);								// returns true if jump was started

	virtual void OnEvent(GameEventType event, CBaseEntity *pEntity = nullptr, CBaseEntity *pOther = nullptr);		// invoked when event occurs in the game (some events have NULL entity)

	#define CHECK_FOV true
	virtual bool IsVisible(const Vector *pos, bool testFOV = false) const;											// return true if we can see the point
	virtual bool IsVisible(CBasePlayer *pPlayer, bool testFOV = false, unsigned char *visParts = nullptr) const;	// return true if we can see any part of the player

	virtual bool IsEnemyPartVisible(VisiblePartType part) const;													// if enemy is visible, return the part we see for our current enemy

public:
	void Disconnect();

	// behavior properties
	float GetCombatRange() const;
	bool IsRogue() const;					// return true if we dont listen to teammates or pursue scenario goals
	void SetRogue(bool rogue);
	bool IsHurrying() const;				// return true if we are in a hurry
	void Hurry(float duration);				// force bot to hurry
	bool IsSafe() const;					// return true if we are in a safe region
	bool IsWellPastSafe() const;			// return true if it is well past the early, "safe", part of the round
	bool IsEndOfSafeTime() const;			// return true if we were in the safe time last update, but not now
	float GetSafeTimeRemaining() const;		// return the amount of "safe time" we have left
	float GetSafeTime() const;				// return what we think the total "safe time" for this map is
	//bool IsUnhealthy() const;				// returns true if bot is low on health

	// behaviors
	void Idle();
	void Hide(CNavArea *searchFromArea = nullptr, float duration = -1.0f, float hideRange = 750.0f, bool holdPosition = false);						// DEPRECATED: Use TryToHide() instead
	#define USE_NEAREST true
	bool TryToHide(CNavArea *searchFromArea = nullptr, float duration = -1.0f, float hideRange = 750.0f, bool holdPosition = false, bool useNearest = false);			// try to hide nearby, return false if cannot

	void Hide(const Vector *hidingSpot, float duration = -1.0f, bool holdPosition = false);			// move to the given hiding place
	bool IsHiding() const;																			// returns true if bot is currently hiding
	bool IsAtHidingSpot() const;																	// return true if we are hiding and at our hiding spot
	const Vector *LsHideGoal() const { return IsHiding() ? &m_hideState.GetHidingSpot() : nullptr; }	// Loner-Strike: where it is hiding, or going to
	bool TryToRetreat();																			// retreat to a nearby hiding spot, away from enemies

	void Hunt();
	bool IsHunting() const;								// returns true if bot is currently hunting

	void Attack(CBasePlayer *victim);
	void FireWeaponAtEnemy();							// fire our active weapon towards our current enemy
	void StopAttacking();
	bool IsAttacking() const;							// returns true if bot is currently engaging a target

	void MoveTo(const Vector *pos, RouteType route = SAFEST_ROUTE); // move to potentially distant position
	bool IsMovingTo() const;							// return true if we are in the MoveTo state

	void PlantBomb();

	void FetchBomb();									// bomb has been dropped - go get it
	bool NoticeLooseBomb() const;						// return true if we noticed the bomb on the ground or on radar
	bool CanSeeLooseBomb() const;						// return true if we directly see the loose bomb
	bool IsCarryingBomb() const;

	void DefuseBomb();
	bool IsDefusingBomb() const;						// returns true if bot is currently defusing the bomb
	bool CanSeePlantedBomb() const;						// return true if we directly see the planted bomb

	void EscapeFromBomb();
	bool IsEscapingFromBomb() const;					// return true if we are escaping from the bomb

	void RescueHostages();
	void UseEntity(CBaseEntity *pEntity);				// use the entity

	bool IsBuying() const;

#ifdef REGAMEDLL_FIXES
	void Kill();
#endif

	void Panic(CBasePlayer *pEnemy);					// look around in panic
	void Follow(CBasePlayer *pPlayer);					// begin following given Player
	void ContinueFollowing();							// continue following our leader after finishing what we were doing
	void StopFollowing();								// stop following
	bool IsFollowing() const;							// return true if we are following someone (not necessarily in the follow state)
	CBasePlayer *GetFollowLeader();						// return the leader we are following
	float GetFollowDuration() const;					// return how long we've been following our leader
	bool CanAutoFollow() const;							// return true if we can auto-follow

	bool IsNotMoving() const;							// return true if we are currently standing still

	void AimAtEnemy();									// point our weapon towards our enemy
	void StopAiming();									// stop aiming at enemy
	bool IsAimingAtEnemy() const;						// returns true if we are trying to aim at an enemy

	bool IsSurprised() const;							// return true if we are "surprised"
	float GetSurpriseDelay() const;
	void ClearSurpriseDelay();

	float GetStateTimestamp() const;					// get time current state was entered

	bool IsDoingScenario() const;						// return true if we will do scenario-related tasks

	// scenario / gamestate
	CSGameState *GetGameState();						// return an interface to this bot's gamestate
	const CSGameState *GetGameState() const;			// return an interface to this bot's gamestate

	bool IsAtBombsite();								// return true if we are in a bomb planting zone
	bool GuardRandomZone(float range = 500.0f);			// pick a random zone and hide near it

	bool IsBusy() const;								// return true if we are busy doing something important

	// high-level tasks
	enum TaskType
	{
		SEEK_AND_DESTROY,
		PLANT_BOMB,
		FIND_TICKING_BOMB,
		DEFUSE_BOMB,
		GUARD_TICKING_BOMB,
		GUARD_BOMB_DEFUSER,
		GUARD_LOOSE_BOMB,
		GUARD_BOMB_ZONE,
		ESCAPE_FROM_BOMB,
		HOLD_POSITION,
		FOLLOW,
		VIP_ESCAPE,
		GUARD_VIP_ESCAPE_ZONE,
		COLLECT_HOSTAGES,
		RESCUE_HOSTAGES,
		GUARD_HOSTAGES,
		GUARD_HOSTAGE_RESCUE_ZONE,
		MOVE_TO_LAST_KNOWN_ENEMY_POSITION,
		MOVE_TO_SNIPER_SPOT,
		SNIPING,

		NUM_TASKS
	};

	void SetTask(TaskType task, CBaseEntity *pEntity = nullptr);				// set our current "task"
	TaskType GetTask() const;
	CBaseEntity *GetTaskEntity();

	// behavior modifiers
	enum DispositionType
	{
		ENGAGE_AND_INVESTIGATE,	// engage enemies on sight and investigate enemy noises
		OPPORTUNITY_FIRE,		// engage enemies on sight, but only look towards enemy noises, dont investigate
		SELF_DEFENSE,			// only engage if fired on, or very close to enemy
		IGNORE_ENEMIES,			// ignore all enemies - useful for ducking around corners, running away, etc

		NUM_DISPOSITIONS
	};

	void SetDisposition(DispositionType disposition);			// define how we react to enemies
	DispositionType GetDisposition() const;						// return enum describing current disposition

	void IgnoreEnemies(float duration);							// ignore enemies for a short duration

	enum MoraleType
	{
		TERRIBLE = -3,
		BAD = -2,
		NEGATIVE = -1,
		NEUTRAL = 0,
		POSITIVE = 1,
		GOOD = 2,
		EXCELLENT = 3,
	};

	MoraleType GetMorale() const;								// return enum describing current morale
	void IncreaseMorale();
	void DecreaseMorale();

	// listening for noises
	bool IsNoiseHeard() const;									// return true if we have heard a noise
	bool ShouldInvestigateNoise(float *retNoiseDist = nullptr);
	void InvestigateNoise();									// investigate recent enemy noise
	const Vector *GetNoisePosition() const;						// return position of last heard noise, or NULL if none heard
	CNavArea *GetNoiseArea() const;								// return area where noise was heard
	void ForgetNoise();											// clear the last heard noise
	bool CanSeeNoisePosition() const;							// return true if we directly see where we think the noise came from
	float GetNoiseRange() const;								// return approximate distance to last noise heard

	bool CanHearNearbyEnemyGunfire(float range = -1.0f) const;	// return true if we hear nearby threatening enemy gunfire within given range (-1 == infinite)
	PriorityType GetNoisePriority() const;						// return priority of last heard noise

	// radio and chatter
	void SendRadioMessage(GameEventType event);					// send voice chatter
	BotChatterInterface *GetChatter();							// return an interface to this bot's chatter system
	bool RespondToHelpRequest(CBasePlayer *them, Place place, float maxRange = -1.0f);	// decide if we should move to help the player, return true if we will
	void StartVoiceFeedback(float duration);
	bool IsUsingVoice() const;									// new-style "voice" chatter gets voice feedback

	// enemies
	// BOTPORT: GetEnemy() collides with GetEnemy() in CBaseEntity - need to use different nomenclature

	void SetEnemy(CBasePlayer *pEnemy);							// set given player as our current enemy
	CBasePlayer *GetEnemy();
	int GetNearbyEnemyCount() const;							// return max number of nearby enemies we've seen recently
	unsigned int GetEnemyPlace() const;							// return location where we see the majority of our enemies
	bool CanSeeBomber() const;									// return true if we can see the bomb carrier
	CBasePlayer *GetBomber() const;

	int GetNearbyFriendCount() const;							// return number of nearby teammates
	CBasePlayer *GetClosestVisibleFriend() const;				// return the closest friend that we can see
	CBasePlayer *GetClosestVisibleHumanFriend() const;			// return the closest human friend that we can see

	bool IsOutnumbered() const;									// return true if we are outnumbered by enemies
	int OutnumberedCount() const;								// return number of enemies we are outnumbered by

	#define ONLY_VISIBLE_ENEMIES true
	CBasePlayer *GetImportantEnemy(bool checkVisibility = false) const;			// return the closest "important" enemy for the given scenario (bomb carrier, VIP, hostage escorter)

	void UpdateReactionQueue();									// update our reaction time queue
	CBasePlayer *GetRecognizedEnemy();							// return the most dangerous threat we are "conscious" of
	bool IsRecognizedEnemyReloading();							// return true if the enemy we are "conscious" of is reloading
	bool IsRecognizedEnemyProtectedByShield();					// return true if the enemy we are "conscious" of is hiding behind a shield
	float GetRangeToNearestRecognizedEnemy();					// return distance to closest enemy we are "conscious" of

	CBasePlayer *GetAttacker() const;							// return last enemy that hurt us
	float GetTimeSinceAttacked() const;							// return duration since we were last injured by an attacker
	float GetFirstSawEnemyTimestamp() const;					// time since we saw any enemies
	float GetLastSawEnemyTimestamp() const;
	float GetTimeSinceLastSawEnemy() const;
	float GetTimeSinceAcquiredCurrentEnemy() const;
	bool HasNotSeenEnemyForLongTime() const;					// return true if we haven't seen an enemy for "a long time"
	const Vector &GetLastKnownEnemyPosition() const;
	bool IsEnemyVisible() const;								// is our current enemy visible
	float GetEnemyDeathTimestamp() const;
	bool IsFriendInLineOfFire();								// return true if a friend is in our weapon's way
	bool IsAwareOfEnemyDeath() const;							// return true if we *noticed* that our enemy died
	int GetLastVictimID() const;								// return the ID (entindex) of the last victim we killed, or zero

#ifdef REGAMEDLL_ADD
	bool CanSeeSniper(void) const;							///< return true if we can see an enemy sniper
	bool HasSeenSniperRecently(void) const;					///< return true if we have seen a sniper recently
#endif

	// navigation
	bool HasPath() const;
	void DestroyPath();

	float GetFeetZ() const;										// return Z of bottom of feet

	void OnDestroyNavDataNotify(NavNotifyDestroyType navNotifyType, void *dead);
	void RemovePath(CNavArea *area);
	void RemoveHidingSpot(HidingSpot *spot);

	enum PathResult
	{
		PROGRESSING,	// we are moving along the path
		END_OF_PATH,	// we reached the end of the path
		PATH_FAILURE,	// we failed to reach the end of the path
	};
	#define NO_SPEED_CHANGE false
	PathResult UpdatePathMovement(bool allowSpeedChange = true);				// move along our computed path - if allowSpeedChange is true, bot will walk when near goal to ensure accuracy

	bool AStarSearch(CNavArea *startArea, CNavArea *goalArea);					// find shortest path from startArea to goalArea - don't actually buid the path
	bool ComputePath(CNavArea *goalArea, const Vector *goal, RouteType route);	// compute path to goal position
	bool StayOnNavMesh();
	CNavArea *GetLastKnownArea() const;									// return the last area we know we were inside of
	const Vector &GetPathEndpoint() const;								// return final position of our current path
	float GetPathDistanceRemaining() const;								// eturn estimated distance left to travel along path
	void ResetStuckMonitor();
	bool IsAreaVisible(CNavArea *area) const;							// is any portion of the area visible to this bot
	const Vector &GetPathPosition(int numpath) const;
	bool GetSimpleGroundHeightWithFloor(const Vector *pos, float *height, Vector *normal = nullptr);	// find "simple" ground height, treating current nav area as part of the floor

	Place GetPlace() const;												// get our current radio chatter place

	bool IsUsingLadder() const;											// returns true if we are in the process of negotiating a ladder
	void GetOffLadder();

	void SetGoalEntity(CBaseEntity *pEntity);

	template <typename T = CBaseEntity>
	T *GetGoalEntity();

	bool IsNearJump() const;											// return true if nearing a jump in the path
	float GetApproximateFallDamage(float height) const;					// return how much damage will will take from the given fall height

	void ForceRun(float duration);										// force the bot to run if it moves for the given duration
	void Wiggle();														// random movement, for getting un-stuck

	bool IsFriendInTheWay(const Vector *goalPos) const;					// return true if a friend is between us and the given position
	void FeelerReflexAdjustment(Vector *goalPosition);					// do reflex avoidance movements if our "feelers" are touched

	// looking around
	void SetLookAngles(float yaw, float pitch);			// set our desired look angles
	void UpdateLookAngles();							// move actual view angles towards desired ones
	void UpdateLookAround(bool updateNow = false);		// update "looking around" mechanism
	void InhibitLookAround(float duration);				// block all "look at" and "looking around" behavior for given duration - just look ahead

	// TODO: Clean up notion of "forward angle" and "look ahead angle"
	void SetForwardAngle(float angle);					// define our forward facing
	void SetLookAheadAngle(float angle);				// define default look ahead angle

	// look at the given point in space for the given duration (-1 means forever)
	void SetLookAt(const char *desc, const Vector *pos, PriorityType pri, float duration = -1.0f, bool clearIfClose = false, float angleTolerance = 5.0f);
	void ClearLookAt();												// stop looking at a point in space and just look ahead
	bool IsLookingAtSpot(PriorityType pri = PRIORITY_LOW) const;	// return true if we are looking at spot with equal or higher priority
	bool IsViewMoving(float angleVelThreshold = 1.0f) const;		// returns true if bot's view angles are rotating (not still)

	const Vector &GetEyePosition() const
	{
		m_eyePos = pev->origin + pev->view_ofs;
		return m_eyePos;
	}
	float ComputeWeaponSightRange();					// return line-of-sight distance to obstacle along weapon fire ray
	bool IsSignificantlyCloser(const CBasePlayer *testPlayer, const CBasePlayer *referencePlayer) const;	// return true if testPlayer is significantly closer than referencePlayer

	// approach points
	void ComputeApproachPoints();						// determine the set of "approach points" representing where the enemy can enter this region
	void UpdateApproachPoints();						// recompute the approach point set if we have moved far enough to invalidate the current ones
	void ClearApproachPoints();
	void DrawApproachPoints();										// for debugging
	float GetHidingSpotCheckTimestamp(HidingSpot *spot) const;		// return time when given spot was last checked
	void SetHidingSpotCheckTimestamp(HidingSpot *spot);				// set the timestamp of the given spot to now

	// weapon query and equip
	#define MUST_EQUIP true
	void EquipBestWeapon(bool mustEquip = false);	// equip the best weapon we are carrying that has ammo
	void EquipPistol();								// equip our pistol
	void EquipKnife();								// equip our knife

	#define DONT_USE_SMOKE_GRENADE true
	bool EquipGrenade(bool noSmoke = false);		// equip a grenade, return false if we cant

	bool IsUsingKnife() const;						// returns true if we have knife equipped
	bool IsUsingPistol() const;						// returns true if we have pistol equipped
	bool IsUsingGrenade() const;					// returns true if we have grenade equipped
	bool IsUsingSniperRifle() const;				// returns true if using a "sniper" rifle
	bool IsUsingAWP() const;						// returns true if we have AWP equipped

	bool IsSniper() const;							// return true if we have a sniper rifle in our inventory
	bool IsSniping() const;							// return true if we are actively sniping (moving to sniper spot or settled in)
	bool IsUsingShotgun() const;					// returns true if using a shotgun
	bool IsUsingMachinegun() const;					// returns true if using the big 'ol machinegun
	void ThrowGrenade(const Vector *target);		// begin the process of throwing the grenade
	bool IsThrowingGrenade() const;					// return true if we are in the process of throwing a grenade
	bool HasGrenade() const;						// return true if we have a grenade in our inventory

	bool DoesActiveWeaponHaveSilencer() const;
	bool IsUsingHEGrenade() const;
	void StartRapidFire();
	void StopRapidFire();
	bool IsRapidFiring() const;

	enum ZoomType { NO_ZOOM, LOW_ZOOM, HIGH_ZOOM };
	ZoomType GetZoomLevel() const;					// return the current zoom level of our weapon

	bool AdjustZoom(float range);					// change our zoom level to be appropriate for the given range

	bool IsPrimaryWeaponEmpty() const;				// return true if primary weapon doesn't exist or is totally out of ammo
	bool IsPistolEmpty() const;						// return true if secondary weapon doesn't exist or is totally out of ammo

	int GetHostageEscortCount() const;
	void IncreaseHostageEscortCount();
	float GetRangeToFarthestEscortedHostage() const;
	void ResetWaitForHostagePatience();
	void ResetValues();								// reset internal data to initial state
	void BotDeathThink();
	CBasePlayer *FindNearbyPlayer();
	void AdjustSafeTime();							// called when enemy seen to adjust safe time for this round
	void EXPORT BotTouch(CBaseEntity *pOther);
	bool HasAnyAmmo(CBasePlayerWeapon *weapon) const;

private:
	friend class CCSBotManager;

	// TODO: Get rid of these
	friend class AttackState;
	friend class BuyState;

	char m_name[64];						// copied from STRING(pev->netname) for debugging

	// behavior properties
	float m_combatRange;					// desired distance between us and them during gunplay
	mutable bool m_isRogue;					// if true, the bot is a "rogue" and listens to no-one
	mutable CountdownTimer m_rogueTimer;
	MoraleType m_morale;					// our current morale, based on our win/loss history
	bool m_diedLastRound;					// true if we died last round
	float m_safeTime;						// duration at the beginning of the round where we feel "safe"
	bool m_wasSafe;							// true if we were in the safe time last update
	NavRelativeDirType m_blindMoveDir;		// which way to move when we're blind
	bool m_blindFire;						// if true, fire weapon while blinded

	// TODO: implement through CountdownTimer
	float m_surpriseDelay;					// when we were surprised
	float m_surpriseTimestamp;

	bool m_isFollowing;						// true if we are following someone
	EntityHandle<CBasePlayer> m_leader;		// the ID of who we are following
	float m_followTimestamp;				// when we started following
	float m_allowAutoFollowTime;			// time when we can auto follow

	CountdownTimer m_hurryTimer;			// if valid, bot is in a hurry

	// instances of each possible behavior state, to avoid dynamic memory allocation during runtime
	IdleState m_idleState;
	HuntState m_huntState;
	AttackState m_attackState;
	InvestigateNoiseState m_investigateNoiseState;
	BuyState m_buyState;
	MoveToState m_moveToState;
	FetchBombState m_fetchBombState;
	PlantBombState m_plantBombState;
	DefuseBombState m_defuseBombState;
	HideState m_hideState;
	EscapeFromBombState m_escapeFromBombState;
	FollowState m_followState;
	UseEntityState m_useEntityState;

	// TODO: Allow multiple simultaneous state machines (look around, etc)
	void SetState(BotState *state);				// set the current behavior state
	BotState *m_state;							// current behavior state
	float m_stateTimestamp;						// time state was entered
	bool m_isAttacking;							// if true, special Attack state is overriding the state machine

	TaskType m_task;							// our current task
	EntityHandle<CBaseEntity> m_taskEntity;		// an entity used for our task

	// navigation
	Vector m_goalPosition;
	EHandle m_goalEntity;
	void MoveTowardsPosition(const Vector *pos);				// move towards position, independant of view angle
	void MoveAwayFromPosition(const Vector *pos);				// move away from position, independant of view angle
	void StrafeAwayFromPosition(const Vector *pos);				// strafe (sidestep) away from position, independant of view angle
	void StuckCheck();											// check if we have become stuck

	CNavArea *m_currentArea;					// the nav area we are standing on
	CNavArea *m_lastKnownArea;					// the last area we were in
	EntityHandle<CBasePlayer> m_avoid;			// higher priority player we need to make way for
	float m_avoidTimestamp;
	bool m_isJumpCrouching;
	bool m_isJumpCrouched;
	float m_jumpCrouchTimestamp;

	// path navigation data
	enum { MAX_PATH_LENGTH = 256 };
	struct ConnectInfo
	{
		CNavArea *area;				// the area along the path
		NavTraverseType how;		// how to enter this area from the previous one
		Vector pos;					// our movement goal position at this point in the path
		const CNavLadder *ladder;	// if "how" refers to a ladder, this is it
	}
	m_path[MAX_PATH_LENGTH];
	int m_pathLength;
	int m_pathIndex;
	float m_areaEnteredTimestamp;
	void BuildTrivialPath(const Vector *goal);									// build trivial path to goal, assuming we are already in the same area
	bool FindGrenadeTossPathTarget(Vector *pos);

	CountdownTimer m_repathTimer;												// must have elapsed before bot can pathfind again

	bool ComputePathPositions();												// determine actual path positions bot will move between along the path
	void SetupLadderMovement();
	void SetPathIndex(int newIndex);											// set the current index along the path
	void DrawPath();
	int FindOurPositionOnPath(Vector *close, bool local = false) const;												// compute the closest point to our current position on our path
	int FindPathPoint(float aheadRange, Vector *point, int *prevIndex = nullptr);									// compute a point a fixed distance ahead along our path.
	bool FindClosestPointOnPath(const Vector *worldPos, int startIndex, int endIndex, Vector *close) const;			// compute closest point on path to given point
	bool IsStraightLinePathWalkable(const Vector *goal) const;														// test for un-jumpable height change, or unrecoverable fall

	mutable CountdownTimer m_avoidFriendTimer;	// used to throttle how often we check for friends in our path
	mutable bool m_isFriendInTheWay;			// true if a friend is blocking our path
	CountdownTimer m_politeTimer;				// we'll wait for friend to move until this runs out
	bool m_isWaitingBehindFriend;				// true if we are waiting for a friend to move

	#define ONLY_JUMP_DOWN true
	bool DiscontinuityJump(float ground, bool onlyJumpDown = false, bool mustJump = false);					// check if we need to jump due to height change

	enum LadderNavState
	{
		APPROACH_ASCENDING_LADDER,		// prepare to scale a ladder
		APPROACH_DESCENDING_LADDER,		// prepare to go down ladder
		FACE_ASCENDING_LADDER,
		FACE_DESCENDING_LADDER,
		MOUNT_ASCENDING_LADDER,			// move toward ladder until "on" it
		MOUNT_DESCENDING_LADDER,		// move toward ladder until "on" it
		ASCEND_LADDER,					// go up the ladder
		DESCEND_LADDER,					// go down the ladder
		DISMOUNT_ASCENDING_LADDER,		// get off of the ladder
		DISMOUNT_DESCENDING_LADDER,		// get off of the ladder
		MOVE_TO_DESTINATION,			// dismount ladder and move to destination area
	}
	m_pathLadderState;
	bool m_pathLadderFaceIn;						// if true, face towards ladder, otherwise face away
	const CNavLadder *m_pathLadder;					// the ladder we need to use to reach the next area
	bool UpdateLadderMovement();					// called by UpdatePathMovement()
	NavRelativeDirType m_pathLadderDismountDir;		// which way to dismount
	float m_pathLadderDismountTimestamp;			// time when dismount started
	float m_pathLadderEnd;							// if ascending, z of top, if descending z of bottom
	void ComputeLadderEndpoint(bool isAscending);
	float m_pathLadderTimestamp;					// time when we started using ladder - for timeout check

	CountdownTimer m_mustRunTimer;					// if nonzero, bot cannot walk

	// game scenario mechanisms
	CSGameState m_gameState;

	// hostages mechanism
	byte m_hostageEscortCount;
	void UpdateHostageEscortCount();
	float m_hostageEscortCountTimestamp;
	bool m_isWaitingForHostage;
	CountdownTimer m_inhibitWaitingForHostageTimer;
	CountdownTimer m_waitForHostageTimer;

	// listening mechanism
	Vector m_noisePosition;				// position we last heard non-friendly noise
	float m_noiseTimestamp;				// when we heard it (can get zeroed)
	CNavArea *m_noiseArea;				// the nav area containing the noise
	float m_noiseCheckTimestamp;
	PriorityType m_noisePriority;		// priority of currently heard noise
	bool UpdateLookAtNoise();			// return true if we decided to look towards the most recent noise source
	bool m_isNoiseTravelRangeChecked;

	// "looking around" mechanism
	float m_lookAroundStateTimestamp;	// time of next state change
	float m_lookAheadAngle;				// our desired forward look angle
	float m_forwardAngle;				// our current forward facing direction
	float m_inhibitLookAroundTimestamp;	// time when we can look around again

	enum LookAtSpotState
	{
		NOT_LOOKING_AT_SPOT,	// not currently looking at a point in space
		LOOK_TOWARDS_SPOT,		// in the process of aiming at m_lookAtSpot
		LOOK_AT_SPOT,			// looking at m_lookAtSpot
		NUM_LOOK_AT_SPOT_STATES
	}
	m_lookAtSpotState;

	Vector m_lookAtSpot;						// the spot we're currently looking at
	PriorityType m_lookAtSpotPriority;
	float m_lookAtSpotDuration;					// how long we need to look at the spot
	float m_lookAtSpotTimestamp;				// when we actually began looking at the spot
	float m_lookAtSpotAngleTolerance;			// how exactly we must look at the spot
	bool m_lookAtSpotClearIfClose;				// if true, the look at spot is cleared if it gets close to us
	const char *m_lookAtDesc;					// for debugging
	void UpdateLookAt();
	void UpdatePeripheralVision();				// update enounter spot timestamps, etc
	float m_peripheralTimestamp;

	enum { MAX_APPROACH_POINTS = 16 };
	Vector m_approachPoint[MAX_APPROACH_POINTS];
	unsigned char m_approachPointCount;
	Vector m_approachPointViewPosition;			// the position used when computing current approachPoint set

	bool BendLineOfSight(const Vector *eye, const Vector *point, Vector *bend) const;	// "bend" our line of sight until we can see the target point. Return bend point, false if cant bend.
	bool LsCornerAhead(Vector *out);		// Loner-Strike: an edge ahead someone could be just past
	void LsWallSanity();				// Loner-Strike: catching itself staring at a wall, and looking away
	// Loner-Strike: what it knows about the game beyond the basics every player picks up (walls
	// stop bullets, footsteps give you away, AWPs are for long range): more the better it is,
	// some by personality; the same every time it plays (LsKnows)
	enum { LSK_PENETRATION, LSK_WALLBANG_SPOTS, LSK_STOP_TO_SHOOT, LSK_QUIET, LSK_TIMINGS, LSK_LINEUPS, LSK_ECONOMY,
		LSK_DEFUSE_MATH, LSK_CROSSHAIR, LSK_EXPOSURE, LSK_COUNT };
	bool LsKnows(int fact);
	void LsLookableSpot(Vector *spot);	// Loner-Strike: a spot behind a wall becomes the edge it'd be seen past
	bool LsScanAngles(bool holding);	// Loner-Strike: look at the next angle an enemy could come from (map knowledge)
	float LsSafeSide(const CNavArea *area, NavDirType dir);	// Loner-Strike: -1..1, the less exposed side walking in
	void LsCrouchWalk();				// Loner-Strike: crouch past a low wall that hides it from an angle
	void LsDoorButton();				// Loner-Strike: a door that only opens from its button: press the button
	EntityHandle<CBaseEntity> m_lsButton;	// the button (or use-only door) it's going to press, and until when it tries
	float m_lsButtonUntil = 0.0f;
	float m_lsButtonCheckAt = 0.0f;
	const CNavArea *m_lsButtonBehind = nullptr;	// the area past the door it's opening (avoided if it can't)
	EntityHandle<CBaseEntity> m_lsBreak;	// something breakable in its way it's breaking, and until when
	float m_lsBreakUntil = 0.0f;
	float m_lsBreakStartAt = 0.0f;			// starts breaking it then (a newer player takes a moment)
	int m_lsSafeMap = 0;				// the map its safe time was set for
	float m_lsDoorBackUntil = 0.0f;		// stepping back out of a door's swing
	float m_lsDoorSideUntil = 0.0f;		// stepping round the edge of an open door leaf
	int m_lsDoorSide = 0;				// +1 right, -1 left
	float m_lsDoorSideAt = -10.0f;
	int m_lsDoorSideTries = 0;
	EntityHandle<CBaseEntity> m_lsDoor;	// the door it pressed for, and when to see whether it opened
	float m_lsDoorCheckAt = 0.0f;
	void LsForceBadArea(const CNavArea *area, float secs);	// Loner-Strike: avoid an area for a while (a door it can't open)
	bool m_lsCrouchWalking = false;
	float m_lsCrouchWalkSince = 0.0f;
	float m_lsCrouchWalkNext = 0.0f;		// not again before (a cooldown after a few steps)
	void LsBumpCheck();					// Loner-Strike: an enemy bumping into it is noticed, wherever it's looking
	unsigned short m_lsScanArea[8] = {};	// the last angles it checked (knowledge area index + 1), and when
	float m_lsScanAt[8] = {};
	int m_lsScanNextSlot = 0;
	float m_lsScanNext = 0.0f;
	const CNavArea *m_lsScanInArea = nullptr;	// the area it last walked into, and when
	float m_lsScanInAt = 0.0f;
	float m_lsBumpAt = 0.0f;			// noticed a bump; reacts at this time
	int m_lsBumpBy = 0;
	float m_lsBumpCheckAt = 0.0f;
	float m_lsWallSince = 0.0f;			// facing a wall up close since
	float m_lsWallNext = 0.0f;			// not checked again before
	float m_lsNarrowNext = 0.0f;		// narrow footing not checked again before
	bool m_lsNarrow = false;			// on narrow footing, as last checked
	float m_lsNarrowAlong = 0.0f;		// the way along it (yaw)
	bool m_lsBeamLock = false;			// the view held on its path this frame (walking a beam): the hand corrects small errors too
	Vector m_lsGlanceCorner;				// the last corner it glanced at walking, and when
	float m_lsGlanceCornerAt = 0.0f;
	float m_lsCornerCheckAt = 0.0f;
	bool FindApproachPointNearestPath(Vector *pos);										// find the approach point that is nearest to our current path, ahead of us

	bool m_isWaitingToTossGrenade;				// lining up throw
	CountdownTimer m_tossGrenadeTimer;			// timeout timer for grenade tossing

	SpotEncounter *m_spotEncounter;				// the spots we will encounter as we move thru our current area
	float m_spotCheckTimestamp;					// when to check next encounter spot

	// TODO: Add timestamp for each possible client to hiding spots
	enum { MAX_CHECKED_SPOTS = 64 };
	struct HidingSpotCheckInfo
	{
		HidingSpot *spot;
		float timestamp;
	}
	m_checkedHidingSpot[MAX_CHECKED_SPOTS];
	int m_checkedHidingSpotCount;

	// view angle mechanism
	float m_lookPitch;						// our desired look pitch angle
	float m_lookPitchVel;
	float m_lookYaw;						// our desired look yaw angle
	float m_lookYawVel;

	// aim angle mechanism
	mutable Vector m_eyePos;
	Vector m_aimOffset;						// current error added to victim's position to get actual aim spot
	Vector m_aimOffsetGoal;					// desired aim offset
	float m_aimOffsetTimestamp;				// time of next offset adjustment
	float m_aimSpreadTimestamp;				// time used to determine max spread as it begins to tighten up
	void SetAimOffset(float accuracy);		// set the current aim offset
	void UpdateAimOffset();					// wiggle aim error based on m_accuracy
	Vector m_aimSpot;						// the spot we are currently aiming to fire at

	// attack state data
	DispositionType m_disposition;				// how we will react to enemies
	CountdownTimer m_ignoreEnemiesTimer;		// how long will we ignore enemies
	mutable EntityHandle<CBasePlayer> m_enemy;	// our current enemy
	bool m_isEnemyVisible;						// result of last visibility test on enemy
	unsigned char m_visibleEnemyParts;			// which parts of the visible enemy do we see
	Vector m_lastEnemyPosition;					// last place we saw the enemy
	float m_lastSawEnemyTimestamp;
	float m_firstSawEnemyTimestamp;
	float m_currentEnemyAcquireTimestamp;
	float m_enemyDeathTimestamp;			// if m_enemy is dead, this is when he died
	bool m_isLastEnemyDead;					// true if we killed or saw our last enemy die
	int m_nearbyEnemyCount;					// max number of enemies we've seen recently
	unsigned int m_enemyPlace;				// the location where we saw most of our enemies

	struct WatchInfo
	{
		float timestamp;
		bool isEnemy;
	}
	m_watchInfo[MAX_CLIENTS];
	mutable EntityHandle<CBasePlayer> m_bomber;						// points to bomber if we can see him

	int m_nearbyFriendCount;										// number of nearby teammates
	mutable EntityHandle<CBasePlayer> m_closestVisibleFriend;		// the closest friend we can see
	mutable EntityHandle<CBasePlayer> m_closestVisibleHumanFriend;	// the closest human friend we can see

#ifdef REGAMEDLL_ADD
	IntervalTimer m_attentionInterval;								// time between attention checks
#endif

	CBasePlayer *m_attacker;				// last enemy that hurt us (may not be same as m_enemy)
	float m_attackedTimestamp;				// when we were hurt by the m_attacker

	int m_lastVictimID;						// the entindex of the last victim we killed, or zero
	bool m_isAimingAtEnemy;					// if true, we are trying to aim at our enemy
	bool m_isRapidFiring;					// if true, RunUpkeep() will toggle our primary attack as fast as it can
	IntervalTimer m_equipTimer;				// how long have we had our current weapon equipped
	bool DoEquip(CBasePlayerWeapon *pWeapon);	// equip the given item

	void ReloadCheck();						// reload our weapon if we must
	void SilencerCheck();					// use silencer

	float m_fireWeaponTimestamp;

#ifdef REGAMEDLL_ADD
	bool m_isEnemySniperVisible;									///< do we see an enemy sniper right now
	CountdownTimer m_sawEnemySniperTimer;							///< tracking time since saw enemy sniper
#endif

	// reaction time system
	enum { MAX_ENEMY_QUEUE = 20 };
	struct ReactionState
	{
		// NOTE: player position & orientation is not currently stored separately
		EntityHandle<CBasePlayer> player;
		bool isReloading;
		bool isProtectedByShield;
	}
	m_enemyQueue[MAX_ENEMY_QUEUE];				// round-robin queue for simulating reaction times

	byte m_enemyQueueIndex;
	byte m_enemyQueueCount;
	byte m_enemyQueueAttendIndex;				// index of the timeframe we are "conscious" of

	CBasePlayer *FindMostDangerousThreat();		// return most dangerous threat in my field of view (feeds into reaction time queue)

	// stuck detection
	bool m_isStuck;
	float m_stuckTimestamp;					// time when we got stuck
	Vector m_stuckSpot;						// the location where we became stuck
	NavRelativeDirType m_wiggleDirection;
	float m_wiggleTimestamp;
	float m_stuckJumpTimestamp;				// time for next jump when stuck

	enum { MAX_VEL_SAMPLES = 5 };
	float m_avgVel[MAX_VEL_SAMPLES];
	int m_avgVelIndex;
	int m_avgVelCount;
	Vector m_lastOrigin;

	// chatter mechanism
	GameEventType m_lastRadioCommand;			// last radio command we recieved
	void RespondToRadioCommands();
	bool IsRadioCommand(GameEventType event) const;	// returns true if the radio message is an order to do something

	#define NO_FORCE false
	void EndVoiceFeedback(bool force = true);
	float m_lastRadioRecievedTimestamp;			// time we recieved a radio message
	float m_lastRadioSentTimestamp;				// time when we send a radio message
	EntityHandle<CBasePlayer> m_radioSubject;	// who issued the radio message
	Vector m_radioPosition;						// position referred to in radio message
	float m_voiceFeedbackStartTimestamp;
	float m_voiceFeedbackEndTimestamp;			// new-style "voice" chatter gets voice feedback
	BotChatterInterface m_chatter;

	// learn map mechanism
	const CNavNode *m_navNodeList;
	CNavNode *m_currentNode;
	NavDirType m_generationDir;

	enum ProcessType
	{
		PROCESS_NORMAL,
		PROCESS_LEARN,
		PROCESS_ANALYZE_ALPHA,
		PROCESS_ANALYZE_BETA,
		PROCESS_SAVE,
	}
	m_processMode;
	CountdownTimer m_mumbleTimer;
	CountdownTimer m_booTimer;
	CountdownTimer m_relocateTimer;

	CNavNode *AddNode(const Vector *destPos, const Vector *normal, NavDirType dir, CNavNode *source);
	void StartLearnProcess();
	void UpdateLearnProcess();
	bool LearnStep();
	void StartAnalyzeAlphaProcess();
	void UpdateAnalyzeAlphaProcess();
	bool AnalyzeAlphaStep();
	void StartAnalyzeBetaProcess();
	void UpdateAnalyzeBetaProcess();
	bool AnalyzeBetaStep();
	void StartSaveProcess();
	void UpdateSaveProcess();
	void StartNormalProcess();

#ifdef REGAMEDLL_ADD
	bool IsNoticable(const CBasePlayer *pPlayer, unsigned char visibleParts) const;	// return true if we "notice" given player
#endif

public:
	// Loner-Strike: who this bot is (ls_bot persona, from gm_people) blended with the bot
	// style's profile (its range: a Newbie lobby or Gods, Matchmaking's teamwork), else the
	// profile alone
	float LsAggro() const;
	float LsTeamwork() const;
	int LsStyle() const;							// LS_STYLE_*, -1 when not told (or the personality slider is at 0)
	int LsTemper() const { return m_lsTemper; }
	int LsHumor() const { return m_lsHumor; }
	void LsSetPersona(float aggro, float team, int style, int temper, int humor, int skill) { m_lsAggro = aggro; m_lsTeam = team; m_lsStyle = style; m_lsTemper = temper; m_lsHumor = humor; m_lsSkillTrait = skill; }
	float LsSkill() const;
	float LsExecPatience();
	CCSBot *LsBombFetcher();						// the one T bot that goes for a dropped bomb now							// seconds it waits at an execute's stage before calling the go							// the profile's skill, leaned toward who the bot is
	// Loner-Strike team (cs_bot_ls_team.cpp)
	void LsTeamTick();								// team play each frame: retakes, rotations, pickups
	bool LsRadioAnswer();							// a teammate's radio call, answered like a person (true: handled)
	bool LsTeamHeard(GameEventType event, CBasePlayer *pPlayer);	// a teammate's gunfire close by: go and trade
	void LsAvenge(CBasePlayer *pVictim, CBasePlayer *pKiller);	// a teammate killed by someone it can't see
	bool LsPlayForTime();							// a CT alone against several before the plant: fall back
	bool LsHoldsPlant();							// guarding the planted bomb: hold, don't chase
	void LsEarlyPick(CBasePlayer *pVictim, CBasePlayer *pKiller);	// a T teammate got an early kill: push
	int LsDefuseHeard(CBasePlayer *pDefuser);		// a T guarding hears the defuse: 0 the stock way, 1 dealt with, 2 not yet (carry on hiding)
	bool LsOrderBusy() const;						// too busy for a person's order (a fight aside): the bomb, a hostage, a throw
	bool LsYieldsDefuse();							// at the bomb but leaves it: a kit CT is right there, or it can't finish
	float LsHealth() const { return 100.0f * float(pev->health) / Q_max(100.0f, float(pev->max_health)); }	// health out of 100, whatever its max (War3, SuperHero; a mod that raises health without the max counts as healthier)
	void LsFloorGunTick();							// fy_ maps: no gun at the start of a round, it runs for one on the floor
	void LsStalemateTick();						// rounds with no objective: a long quiet ends with someone going looking
	const CCSBotManager::Zone *LsQuietZone();		// the bomb site with the fewest teammates at it (ties at random)
	bool LsPlantHere();
	const CCSBotManager::Zone *LsPlantOrder() const;	// where a person told it to plant, this round
	bool LsRetakeRush();							// a CT after the plant who can still make it to the bomb: no dawdling
	bool LsExecuting();								// a T in the round's site push: no dawdling
	bool LsRushing() { return LsRetakeRush() || LsExecuting(); }
	bool LsLurksOtherSite();						// a T who goes for the other site this round (a lurker, mostly)
	void LsExecuteTick();							// Ts staging near the site, then going in together
	void LsHideNearBomb(const Vector &bomb);		// guarding the planted bomb: a spot near it no teammate has (stock: the same best spot for all)								// at the site with the bomb: plant now, or walk in to a better spot first
	bool LsFetchesBomb();							// one of the nearest two Ts to a dropped bomb
	// (end team)
	// Loner-Strike nades (cs_bot_ls_nades.cpp)
	void LsNadeReset();								// a new round
	bool LsNadeBusy() const { return m_lsNadePhase != 0; }	// getting a grenade out, throwing it, or looking away from its flash
	bool LsNadeDumper();							// a newer pub player who throws every grenade early in the round
	float LsNadeUse();								// how well it uses grenades (0-1ish)
	bool LsHasNade(int id);
	void LsNadeUpdate();							// grenades with a purpose (instead of the stock end-of-safe-time toss)
	bool LsNadeGunUp();								// an enemy turned up with a flash or smoke in hand: the gun instead
	void LsNadeStart(int id, const Vector &aim, float pause);
	void LsNadeStop();
	void LsStartOver();
	bool LsOverwhelmed();							// several facing it at once: the less bold back off (once a fight, by its roll)
	void LsFlankCheck();							// hit by someone it isn't fighting: turns to them, or a look, by its skill							// respawned or teleported: what it was in the middle of ends
	bool LsBlindTick();
	void LsHandUpdate(float goalYaw, float goalPitch, float dt);	// the view as a hand on a mouse moves it
	Vector LsLaggedEnemyPos();						// the enemy as its hand follows them: a moment behind (by skill)
	void LsTremorTick();							// a hand's tremor on the aim
	void LsSeedAs(const char *name) { if (!FStrEq(name, m_lsSeedAs)) { Q_strlcpy(m_lsSeedAs, name); m_lsSeedName = 0; } }	// (gm_people's persona comes as the bot joins, before its habits are used)
	void LsSniperStep();							// after a sniper shot: a step aside while the zoom comes back							// flashed: its own way of coping (true: dealt with this frame)
	bool LsKnifeOnly() const { return !m_rgpPlayerItems[PRIMARY_WEAPON_SLOT] && !m_rgpPlayerItems[PISTOL_SLOT]; }	// a knife and nothing else to shoot with
	bool LsTossing() const { return m_isWaitingToTossGrenade; }
	void LsHeAway(float secs) { m_lsHeBackAt = gpGlobals->time + secs; m_lsHeBlockedSince = -1.0f; }	// Gun Game's grenade put away for a while
	void LsTeleportCheck();						// moved further in a moment than anyone can run: LsStartOver
	bool LsPlanThrow(int id, const Vector &target, float within, Vector *aim, Vector *burst);
	bool LsPlanFlash(const Vector &corner, const Vector &beyond, Vector *aim, Vector *burst, bool *inMySight);
	bool LsFlashSafe(const Vector &burst, bool careless);	// no teammate facing the burst
	bool LsNadeCorner(Vector *corner, Vector *beyond);	// the corner ahead on its path
	int LsNadeSite();								// the bomb site a T is heading for, or -1
	bool LsNadeHot(const Vector &spot);				// enemies likely round there
	void LsSawClear(CBasePlayer *pPlayer);			// its enemy seen with no smoke in the way
	float LsSmokeSprayTime();
	bool LsClearLine(const Vector &spot, bool testFOV);	// in sight but for smoke
	bool LsSeeIntoSmoke(CBasePlayer *pPlayer, bool testFOV, unsigned char *visParts);	// an enemy behind smoke it still shoots at
	void LsPlanBuy();								// BuyState::OnEnter: save, force or buy this round
	bool LsSavingMoney() const { return m_lsBuyPlan == 1; }
	bool LsForcing() const { return m_lsBuyPlan == 2; }
	void LsBuyEco();								// a save round: a pistol at most
	void LsBuyForce();								// a force buy: a cheaper gun
	void LsBuyExtras();								// grenades and the defuse kit (instead of the stock picks)
	void LsSaveCheck();								// alone against many late in a lost round: hide, keep the gun
	void LsPostPlantAware();						// a CT hiding with the bomb down and time to retake: goes
	bool LsSavingGun() const { return m_lsSaving; }
	void LsCallBuy(int plan);						// a person's "eco" / "force" / "full buy": this buy, or the next
	void LsAskThrow(const char *item, const Vector &at);	// a person's "flash for me", "smoke mid": that grenade, there
	bool LsNadeOnly() const;						// deathmatch with nothing but a knife and grenades (Gun Game's HE level)
	static bool LsWeaponModeOn();					// a mode that hands out or limits the guns
	bool LsBuyPistolRound();						// its own pistol-round buy (true: done)
	// (end nades)
	// Loner-Strike moves (cs_bot_ls_moves.cpp)
	bool LsCornerMoves(Vector *goal);				// a blind corner ahead: jiggle, wide swing or outside line (true: a jiggle moves it)
	int LsPickCornerPlan();							// ...which, for this corner
	float LsCrouchTapTime();						// how long a crouch in a fight lasts (mostly taps)
	const Vector *LsOffAngle(const Vector *stockPick, const Vector *pos, CNavArea *source, float range);	// a less obvious spot to hold
	void LsPrefire(const Vector &spot);				// a hiding spot coming into view up close: a short burst (some)
	void LsFallBack();								// falling back from a fight: eyes on the threat
	bool LsFallingBack() const;
	bool LsLookingBack() const;						// ...looking back at it right now
	void LsMovesUpdate();							// each think, after the state machine: prefires, looking back
	bool LsScopesClose();							// newer snipers scope in (and keep the rifle) up close
	void LsAttackBegins(bool crouchAndHold);		// a fight starts: newer players may panic
	void LsPanicUpdate();
	int LsLadderSide();								// diagonal climb on this ladder: 1, -1, or 0 for straight
	bool LsLadderYaw(float faceYaw, float *yaw);	// ...the view for it
	void LsLadderClimb();							// ...the strafe for it
	bool LsQuietDrop(float dz);						// sneaking: walk off a small drop instead of jumping
	bool LsBadLadder(const CNavLadder *ladder) const { return ladder && ladder == m_lsBadLadder && gpGlobals->time < m_lsBadLadderUntil; }
	void LsLadderStuck(const CNavLadder *ladder) { m_lsBadLadder = ladder; m_lsBadLadderUntil = gpGlobals->time + 25.0f; }
	bool LsBadArea(const CNavArea *area) const;		// an area it keeps failing to get into (costly for a while)
	void LsAreaStuck(const CNavArea *area, bool climb = false);			// gave up getting into it: twice in a row and it's a bad area
	bool LsDucksOff(const CNavArea *area, const Vector &spot) const;	// getting off a ladder into a low space: crouch
	bool LsCalm(bool pluginLooks = true) const;		// nothing has startled it lately (calm looks turn slower)
	bool LsPuppetLooking() const { return gpGlobals->time < m_lsPuppet.lookUntil; }	// a plugin has it looking somewhere
	bool LsStuck() const { return m_isStuck; }
	// (end moves)
	// Loner-Strike (cs_bot_ls.cpp): each bot keeps to itself and moves like a person
	unsigned int LsSeed();
	float LsTrait(unsigned int which);
	float LsGetDanger(const CNavArea *area);
	bool LsRemembersDanger() const { return gpGlobals->time < m_lsDangerUntil; }	// any of its own danger memory not yet faded
	float LsSinceDied() const { return gpGlobals->time - m_lsDiedAt; }
	void LsAddDangerNearby(float amount, CNavArea *startArea, const Vector *pos, float maxRadius);
	void LsSawTeammateDie(CBasePlayer *pVictim);
	bool LsRouteTaste(RouteType route, struct LsRoute *out);	// false: no taste in routes now
	int LsCrowdAreas(CNavArea **out, int max, float *cost);	// Loner-Strike: where teammates are (for paths)
	void LsShiftPortalPoint(const CNavArea *from, const CNavArea *to, NavDirType dir, Vector *pos);
	bool LsUpdatePause();
	void LsPuppetCommand(const char *what, float now);	// ls_bot (a plugin's short say over this bot)
	void LsPuppetUpdate();
	void LsPuppetRelease();
	bool LsPuppetActive() const;
	bool LsBusy() const;							// doing something that mustn't be interrupted
	bool LsLoner() const;							// goes its own way (see bot_ls_solo)
	bool LsBangTick();								// spraying through a wall, while it's on
	bool LsBangGun(CBasePlayerWeapon *pWeapon) const;	// a gun that goes through doors and thin walls
	bool LsBangThrough(const Vector &target);		// starts spraying a spot behind one thin thing
	void LsHeard(GameEventType event, CBasePlayer *pPlayer, float dist);	// an enemy heard, not seen
	bool LsWallbang(float notSeenEnemyTime);		// spraying a thin wall the enemy just went behind (true while it does)
	bool LsRadioOk(GameEventType event);			// whether this radio message is sent (bot_ls_fight)
	void LsShotAt(CBasePlayer *pShooter, const Vector &impact);	// enemy bullets landing or passing close
	void LsHeardBreak(CBaseEntity *pBreak);		// glass, a vent cover, a board breaking: someone's there
	void LsHoleCheck();							// a floor broke: a route across it changed, if it noticed
	void LsHurtBy(CBasePlayer *pAttacker);		// hit: through a wall? that spot's not safe
	bool LsPreAim(const Vector &heard, float secs, CBasePlayer *who = nullptr);	// look where a sound will come out (the area's angles)
	int m_lsCtPushRound = -1;						// a CT: the round it decided whether to push out (once a round)
	bool m_lsCtPush = false, m_lsCtPushed = false;
	int m_lsRotateZone = -1;						// a site it rotated to (being hit): held for a while once there
	float m_lsRotateZoneUntil = 0.0f;
	float m_lsHoldSiteSkipUntil = 0.0f;
	float m_lsHoldSiteAt = -10.0f;					// a CT's last site hold, and how many came straight back in a row
	int m_lsHoldSiteFails = 0;
	float m_lsInvestAimAt = 0.0f;					// next pre-aim while closing on a noise (InvestigateNoiseState)
	float m_lsHuntHoldUntil = 0.0f;					// hunting, stopped a moment to hold an angle on a sound (HuntState)
	float m_lsScanBlindUntil = 0.0f;				// its map angles are all walled off from where it stands, until (UpdateLookAround)
	bool LsTakeCoverFrom(const Vector &threat, float minMove, bool fromFight = false, float holdSecs = -1.0f);	// to the nearest spot out of its sight, a short hold there
	bool LsMatchup(CBasePlayer *pEnemy, bool crouchAndHold);		// mid-fight: the wrong gun for this range (an AWP down a lane): out of it
	Vector LsDefuseSpot(const Vector &bomb);		// beside the bomb, the side away from the threat
	bool LsOrderActive() const;
	bool LsLowCoverToward(const Vector &at);	// low cover right by it that way (crouch behind it)
	float LsFallMargin();						// how close to dying from a drop it'll go (pros never close)
	EntityHandle<CBasePlayer> m_lsMatchupEnemy;	// the enemy whose matchup it weighed (once a fight)
	float m_lsMatchupAt = 0.0f;
	float m_lsDuckUntil = 0.0f;				// down behind low cover until, and when it may again
	float m_lsDuckAt = -999.0f;
	float m_lsReloadCrouchAt = 0.0f;			// whether to crouch for this reload was decided (until then)
	float m_lsKnifeOkUntil = 0.0f;
	// round knowledge (batch 3): the kill feed and radar, its own last death, the numbers
	bool LsCutOffRescue(const Vector &hostage);		// a hostage taken: to the rescue zone it'll go to, or partway
	void LsTold(const Vector &at);					// a teammate's callout: an enemy there
	void LsFeed(CBasePlayer *pVictim, CBasePlayer *pKiller);	// a teammate died (radar spot, the gun in the feed)
	void LsNoteDeath(CBasePlayer *pKiller);			// it died: where (avoided next round), and a person's spot
	void LsImpactNearMate(CBasePlayer *pShooter, const Vector &impact);	// enemy bullets landing on a teammate: a fight there
	float LsTempo();								// its side's alive count minus theirs (what the scoreboard says)
	float LsTimingWeight(const CNavArea *area);		// <1 for somewhere the enemy can't have reached yet this round
	Vector m_lsFeedSpot;
	float m_lsFeedAt = -999.0f;
	bool m_lsFeedSniper = false;
	const CNavArea *m_lsDiedArea = nullptr;
	int m_lsDiedMap = -1, m_lsDiedRound = -1;
	int m_lsBadMap = -1;							// the map its avoided areas are from
	float m_lsTempo = 0.0f, m_lsTempoAt = -999.0f;
	float m_lsImpactAt = -999.0f;
	float m_lsHurtPushAt = -999.0f;					// last went after an enemy called hurt
	bool m_lsCoverStay = false;						// the last cover pick was where it already is
	bool m_lsPushArrived = false;					// got to its spot in the push (the push is over for it)
	Vector m_lsPushDest;
	float m_lsPushGoAt = 0.0f;
	float m_lsForTimeAt = -999.0f;					// last fell back to play for time (once a contact)
	float m_lsSniperShotAt = -999.0f;				// the heard AWPer last fired (the bolt's coming back)
	float m_lsPlantHeardAt = -999.0f;
	float m_lsTempoPushRound = -1;
	void LsPublishState();							// what it's doing, for the plugins (pev->vuser1.y)
	void LsCoverPeekTick();							// holding behind low cover: down, up for a look, down
	float m_lsCoverPeekUntil = 0.0f;
	bool m_lsCoverPeekUp = false;
	void LsAimMapTick();							// aim_/fy_ maps: from cover to cover, pushing only on an edge
	float m_lsAimMapAt = 0.0f;
	float m_lsSmokeLaneAt = 0.0f;
	float m_lsHePlanAt = 0.0f;						// Gun Game's grenade level: next time it works out an arc
	float m_lsExecCheckAt = 0.0f;
	float m_lsJiggleJumpUntil = 0.0f;
	int m_lsRetakeWayRound = -1;
	CNavArea *LsObjectiveHuntArea();				// somewhere worth going on an objective map: forward, not back to its spawn
	bool LsOutOfRange(CBasePlayer *pEnemy, float range);	// its gun can't do anything from here: close in, don't spray
	bool LsBangChest();								// only a head showing over something a bullet goes through: the chest, through it
	float m_lsBangChestAt = 0.0f;
	bool m_lsBangChest = false;
	void LsTraceAttack();
	bool LsKnifeDanger();
	// out of bullets: it has a gun, but every one is empty (not a knife-only mode, where the
	// knife is the point)
	bool LsOutOfAmmo() const { return (m_rgpPlayerItems[PRIMARY_WEAPON_SLOT] || m_rgpPlayerItems[PISTOL_SLOT]) && IsPrimaryWeaponEmpty() && IsPistolEmpty(); }							// a reason to have the gun out now, knife run or not
	bool LsAboutToEnter();							// right at a doorway or corner into somewhere it can't see yet
	Vector m_lsRecent[8];							// where it's been lately (every few seconds), so hunting doesn't circle back
	int m_lsRecentNext = 0;
	float m_lsRecentAt = 0.0f;
	void LsNoteWhereItIs();
	bool LsBeenNear(const Vector &at, float range) const;
	bool LsBehindMe(const Vector &at) const;
	float m_lsCoverRunUntil = 0.0f;					// on a deliberate cover move (a matchup, a rush): aiming at it doesn't pull it back out
	float m_lsMatchupUntil = 0.0f;					// the matchup with m_lsMatchupEnemy decided until (not re-weighed each new fight)
	float m_lsHuntRetryAt = 0.0f;
	void LsCheckBeforePlant();						// a quick double check before planting
	bool LsBombHot(const Vector &bomb);				// a teammate died by the dropped bomb a moment ago (the radar shows it)
	void LsTraceSeen(CBasePlayer *pEnemy);			// bot_ls_trace: an enemy first in view					// every hunting goal failed: a moment before trying again		// behind the way it's going (a new goal there is a U-turn)							// bot_ls_trace: a fight starting, and how long it took to react					// the round it picked its way into a retake
	void LsRetakeApproach();				// mid jump-peek (in the air: still the jiggle)					// a smoke for the AWP lane it just got out of, thrown from cover then
	Vector m_lsSmokeLaneSpot;
	void LsSmokeLaneTick();
	void LsFlashWatch();							// a flashbang about to pop in front of it: turn away (the better ones)
	float m_lsFlashCheckAt = 0.0f, m_lsFlashAwayUntil = 0.0f;
	float m_lsFlashPopAt = -999.0f;					// a flash it saw go off (or a teammate's, called out): when and where
	Vector m_lsFlashPopPos;
	float m_lsFlashGuess = 2.0f;					// how long it reckons that flash blinds (closer the better it is)
	float m_lsFlashHeardAt = -999.0f;				// a bang heard (not seen): where, roughly
	Vector m_lsFlashHeardPos;
	float m_lsFlashBackAt = 0.0f;					// after turning its back to an enemy flash: when to look back where it came from
	Vector m_lsFlashBackPos;
	int m_lsAwayWho = 0;							// the enemy in view that has had its back (or side) to it since
	float m_lsAwaySince = 0.0f;
	bool LsLadderExposed() const;					// enemies about: a ladder is a bad place to be
	bool LsAwpKnownToward(const Vector &dir2d) const;	// an AWPer known (heard, the feed) roughly that way
	bool m_lsJumpPeek = false;						// this corner's jiggle is a jump-peek (baiting an AWP)
	void LsHurtCallTick();
	bool m_lsTap = false;						// a semi-automatic's trigger: pressed on every other think, let go between
	bool LsPuppetOwnsLook() const;
	bool LsTriggerTap() { m_lsTap = !m_lsTap; return m_lsTap; }
	float m_lsRushHoldUntil = 0.0f;			// in cover from a rush, waiting for its team, until (at most)
	float m_lsRushHoldCheckAt = 0.0f;
	void LsRushHoldTick();				// a plugin's knife out on purpose, left alone until then
	bool m_lsReloadCrouched = false;			// ...and it did (up again when it's done)
	bool LsRushed(const Vector *threat);		// many more of them coming at it than its side has here: back to cover
	float m_lsRushHeardAt[MAX_CLIENTS];			// each enemy last heard running toward it, close
	float m_lsRushCheckAt = 0.0f;
	float m_lsRushedAt = -999.0f;
	bool LsRunnerGlance();						// a quick urgent turn (a runner heard, shot from the side) that beats the enemy aim
	void LsUrgentTurn(const Vector &at, float secs, CBasePlayer *who);	// ...started (who: fought if they come into view)
	EntityHandle<CBasePlayer> m_lsRunner;		// ...who, where, until when, and when it last did
	Vector m_lsRunnerSpot;
	float m_lsRunnerUntil = 0.0f;
	float m_lsRunnerAt = 0.0f;
	float m_lsHurtCheckAt = 0.0f;
	EntityHandle<CBasePlayer> m_lsSniperHeard;	// an enemy heard scoping a sniper rifle, where, when
	Vector m_lsSniperSpot;
	float m_lsSniperHeardAt = -999.0f;
	float LsObjUseAt() const { return m_lsObjUseAt; }	// planting/defusing started again then (after a threat)
	void LsObjectiveBegins() { m_lsObjStops = 0; m_lsForTimeRoll = -1; }	// a plant or defuse starts
	void LsFightBegins() {}							// a new fight (playing for time is decided once a round)
	bool LsTeamGame() const;						// a mode with teams and objectives (not deathmatch, whatever bot_ls_solo)
	bool LsFree() const;							// free to go somewhere for team play: not busy, saving, puppeted
	void LsCheckBeforeDefuse();						// a look round the site before defusing
	bool LsObjectiveThreat(bool defusing);			// planting/defusing: a threat around it (true: stop for now)
	void LsWhy(const char *why);			// a decision's label (bot_ls_trace: written as it happens)
	void LsTrace();										// bot_ls_trace: what it's doing, to lstrace.csv
	bool LsShirksObjective();						// a rogue who leaves the objective to others
	CNavArea *LsPickHuntArea();					// every bot for itself: where to hunt
	bool LsTeammateCloser(const Vector &pos);
	bool LsShouldSpread();
	float LsDodgeChance();							// fights like people (bot_ls_fight)
	float LsDodgeDuration(bool steady);
	bool LsPickSteady();
	bool LsMayBackOff(const Vector &toEnemy);
	bool LsNarrowFooting();				// Loner-Strike: on a beam or ledge about a stride wide with a drop beside it that would hurt
	float LsNarrowAlong() const { return m_lsNarrowAlong; }	// ...the way along it (yaw), as it found it
	bool LsLadderAt(const Vector &spot) const;	// Loner-Strike: a spot on the wall a ladder's on
	bool LsHoldFireToStop(float range);
	void LsTraceNote(const char *note);				// a line in the trace (a #why) without changing what it's doing
	void LsThreatNote();							// backed off from someone: remembered (cs_bot_ls_team.cpp)
	void LsThreatTick();							// and what to do about them
	bool LsThreatActive() const;					// still remembered
	float m_lsStopNoteAt = -10.0f;					// trace: when its stop habit was last noted
	int LsStopStyle();								// its habit for stopping to shoot: 0 run, 1 crouch, 2 walk, 3 counter-strafe
	bool LsStopGun() const;							// a gun worth stopping for (rifles, the Deagle)
	void LsCrouchStop();							// a crouch to stop for the shot
	bool LsSpaceOut();								// in a fight on top of a teammate: a step aside (true while stepping)
	float LsReloadCoverChance();					// how likely it gets out of sight to reload
	bool LsSaveFreeShot(CBasePlayer *pEnemy);		// saving its gun: a free shot at one who hasn't seen it
	void LsBarrelCheck(CBasePlayer *pPlayer);		// only a bit showing (gun, shoulder, knee, foot): now and then noticed
	float LsCrouchFactor();
	float LsPinnedDownTime();
	void LsFightHabits(float range, bool crouchAndHold);	// crouch bursts, jump peeks
	bool LsCornerPeek();							// strafe round a corner looking into it
	void LsSneak();									// shift-walk where enemies may be
	bool LsCounterStrafe(int fromState);			// a tap the other way to stop dead
	void LsStopDead();								// keys against its own velocity (counter-strafe)
	float LsBuyPace() { return LsTrait(23); }		// 0 quick to buy, 1 slow
	float LsReloadRatio();							// reloads below this much ammo (habit)
	float LsReloadWait();							// after this long without seeing anyone
	float LsAimDamping();							// 1 stock; lower overshoots like a flick
	float LsLookHeight();							// how high it holds its crosshair
	float LsSmoothYaw(float target);				// heading eases round instead of snapping
	bool LsPushes(float range);						// runs at them pressing W (habit or weapon)
	float LsChaseExtra();							// chases a lost enemy longer (eager ones)
	struct LsDanger { unsigned int id = 0; float amount = 0.0f; float time = 0.0f; };

private:
	struct LsPuppet
	{
		bool any = false;
		float holdUntil = 0.0f;
		float crouchStart = 0.0f, crouchUntil = 0.0f, crouchEvery = 0.0f;
		Vector lookAt;
		float lookStart = 0.0f, lookUntil = 0.0f, nodEvery = 0.0f;
		int jumpsLeft = 0;
		float nextJump = 0.0f, jumpEvery = 0.0f, jumpUntil = 0.0f;
		float walkUntil = 0.0f;
		float followUntil = 0.0f;
		char holdTag[8] = "";					// who asked for the hold (gm_botfun bf, gm_humanbots hb, gm_botchat bc)
		bool fast = false;							// a quick look (a flick, a spin): not the calm turn
		bool urgent = false;						// an urgent turn (look ... 2): goes on through a fight, till an enemy is in sight
	};
	LsPuppet m_lsPuppet;
	// a person's order (go somewhere, follow me): kept apart from a plugin's habits, so a fight
	// or another plugin's release doesn't end it; picked back up after a fight
	struct LsOrder
	{
		int kind = 0;								// 0 none, 1 go to a spot, 2 follow someone, 3 holding where it was sent
		Vector at;
		EntityHandle<CBasePlayer> leader;
		float until = 0.0f;
		float holdSecs = 0.0f;						// a go: then hold the spot that long
		float nextTry = 0.0f;
		int tries = 0;								// times sent back on its way (without getting closer)
		float bestDist = 1e9f;						// the closest it's been to where it was sent
	};
	LsOrder m_lsOrder;
	bool m_lsOrderFields = false;					// fuser4/vuser1 hold this code's order marks (cleared on respawn)
	void LsOrderTick();
	void LsCheckMap();
	void LsAddDangerToArea(const CNavArea *area, float amount);
	LsDanger m_lsDanger[96];						// this bot's own danger memory, by area id (a fixed table:
													// the game frees a bot without running its destructor)
	char m_lsMap[64] = "";									// the map that memory is for
	float m_lsMapCheckedAt = -1.0f;						// the frame the map was last checked (once a frame is enough)
	float m_lsDiedAt = -100.0f;						// when it was last killed
	float m_lsBombCallAt = 0.0f;
	int m_lsLastApproach = -1;						// the opening it watched last while holding (not again straight away)
	float m_lsBehindSince = 0.0f;					// when the view went over its shoulder while running (0: it isn't)
	float m_lsHuhUntil = 0.0f;						// a "huh?" look at a noise off its way, until then (0: none)
	float m_lsHuhNoise = -1.0f;						// the noise (its timestamp) it last looked at like that
	float m_lsBehindNext = 0.0f;					// the next over-the-shoulder glance allowed					// next look for a teammate's "bomb's at A" (after a plant)
	bool m_lsWasBlind = false;
	float m_lsOverwhelmedAt = -10.0f;				// several enemies facing it at once, then
	float m_lsFlankReactAt = 0.0f;
	float m_lsOverwhelmRollAt = 0.0f;
	float m_lsThreatSince = 0.0f;					// when it last changed whom it's fighting					// next check of a hit from someone it isn't fighting
	struct LsHand
	{
		bool moving = false;
		float y0 = 0.0f, p0 = 0.0f;					// where the move started
		float y1 = 0.0f, p1 = 0.0f;					// where it's going to land (the goal, plus the hand's error)
		float goalY = 0.0f, goalP = 0.0f;				// the goal when it was planned (a moving one is followed)
		float t0 = 0.0f, dur = 0.1f;
		int corrections = 0;
		float restUntil = 0.0f;
		float reactUntil = 0.0f;					// a new thing to look at: not before then
		float lastY = 0.0f, lastP = 0.0f;			// last frame's goal (a sudden change is something new)
	};
	LsHand m_lsHandM;
	bool m_lsHandMoving = false;
	EntityHandle<CBasePlayer> m_lsTrackOf;			// whose positions the aim lag buffer holds
	Vector m_lsTrackPos[8];
	float m_lsTrackAt[8] = {};
	int m_lsTrackN = 0;
	Vector m_lsEstPos;								// the tracking filter's estimate of the enemy
	Vector m_lsEstVel;
	float m_lsEstAt = 0.0f;
	float m_lsTremorYaw = 0.0f;
	float m_lsTremorPitch = 0.0f;
	float m_lsWhiffUntil = 0.0f;					// a spray that hit nothing: shakier till then
	int m_lsShotsNoHit = 0;						// calm looks: a burst of the mouse under way
	float m_lsHandRestUntil = 0.0f;					// ...and the hand resting till then
	float m_lsSnipeStepAt = -10.0f;
	int m_lsSnipeStepDir = 0;
	float m_lsBlindTurnAt = 0.0f;					// flashed: next change of what it's doing
	int m_lsBlindWay = 0;
	bool m_lsBlindSpray = false;
	Vector m_lsFightHeard;							// gunfire it heard last (deathmatch hunting)
	float m_lsFightHeardAt = -100.0f;
	int m_lsEnemyDeaths = -1;						// its enemy's deaths when last seen (a respawned one isn't still there)
	float m_lsHeBlockedSince = -1.0f;				// Gun Game's grenade level: no safe throw since then
	float m_lsHeBackAt = 0.0f;						// ...and not back out before then
	float m_lsDangerUntil = -1.0f;						// when all of that memory will have faded (nothing to look up after)
	char m_lsSeedAs[32] = "";						// the name to seed from instead of the shown one (gm_people)
	string_t m_lsSeedName = 0;								// the name LsSeed was worked out from
	unsigned int m_lsSeed = 0;
	float m_lsPauseUntil = 0.0f;
	float m_lsNextPauseCheck = 0.0f;
	float m_lsNextSpreadCheck = 0.0f;
	float m_lsCrouchBurstUntil = 0.0f;
	float m_lsNextHabit = 0.0f;
	float m_lsCounterUntil = 0.0f;
	float m_lsHoldSince = 0.0f;						// stopping to shoot since (0: not)
	float m_lsSpaceCheckAt = 0.0f;					// next look at whether it's on top of a teammate
	float m_lsSpaceUntil = 0.0f;					// stepping aside until
	int m_lsSpaceDir = 0;							// +1 right, -1 left
	float m_lsReloadRollAt = -10.0f;				// the reload it decided about (getting out of sight)
	bool m_lsSaveShot = false;						// saving its gun, but taking this free shot
	int m_lsSaveShotWho = 0;
	float m_lsSaveSeenAt[MAX_CLIENTS] = {};			// saving: when each enemy was last in view (a fresh sighting rolls)
	float m_lsSaveShotAt = 0.0f;					// when it fires the free shot (0: not taking one)
	float m_lsBarrelCheckAt[MAX_CLIENTS] = {};		// per enemy: next look for a gun sticking out
	// threat memory: an enemy it backed off from (cs_bot_ls_team.cpp LsThreatNote)
	Vector m_lsThreatPos;							// where it last saw them
	Vector m_lsThreatFrom;							// where it was when it saw them
	float m_lsThreatAt = 0.0f;						// when (0: nothing remembered)
	float m_lsThreatUntil = 0.0f;					// remembered until
	float m_lsThreatDecideAt = 0.0f;				// when it makes up its mind about them
	int m_lsThreatCount = 0;						// how many it saw
	int m_lsThreatWho = 0;							// the one it was fighting (entity index)
	int m_lsThreatRound = -1;
	bool m_lsThreatSniper = false;					// they had a sniper rifle
	bool m_lsThreatDecided = true;
	float m_lsThreatDecidedAt = -100.0f;			// when it last made up its mind (a fall back it chose isn't noted again)
	float m_lsPushUntil = 0.0f;						// running at them until
	float m_lsYaw = 0.0f;							// eased heading
	float m_lsYawAt = 0.0f;							// when it was last eased
	float m_lsPinnedExtended = 0.0f;				// how much a winning fight has been extended
	float m_lsVetoNoiseAt = -1.0f;					// the noise the teammate veto was rolled for
	bool m_lsVetoNoise = false;
	int m_lsCounterDir = 0;							// 1 left, -1 right
	float m_lsNextSneakCheck = 0.0f;
	bool m_lsSneaking = false;
	float m_lsAggro = -1.0f;						// who this bot is, from gm_people (ls_bot persona): -1 not told
	float m_lsTeam = -1.0f;
	int m_lsStyle = -1;								// rusher roamer lurker careful camper awper knifer nader ninja objective weirdgun
	int m_lsTemper = 1;								// 0 chill, 1 normal, 2 tilty
	int m_lsHumor = 1;								// 0 none, 1 dry, 2 goofy, 3 troll
	int m_lsSkillTrait = -1;						// gm_people's skill: 0 newbie .. 3 pro, -1 not told
	// Loner-Strike team (cs_bot_ls_team.cpp): members
	float m_lsRotateAt = 0.0f;						// going to a teammate's call then (after a look)
	Vector m_lsRotateTo;
	float m_lsTradeAt = -10.0f;						// last went to trade a teammate
	int m_lsRetakeRound = -1;						// the round it waited for teammates before a retake
	int m_lsPickupRound = -1;						// the round it went for a dropped gun after
	int m_lsTeamRound = -1;							// the round LsTeamTick last saw
	int m_lsStagedRound = -1;						// the round it waited near the site for the push
	int m_lsPushRound = -1;							// the round it went in with the push
	const CCSBotManager::Zone *m_lsExecSite = nullptr;	// the site those were for (the team can switch)
	enum { LSR_NONE, LSR_ENTRY, LSR_TRADE, LSR_FLANK, LSR_SNIPER };
	int m_lsRole = LSR_NONE;						// its part in the push this round (picked from what the others are doing)
	int m_lsRoleRound = -1;
	int m_lsHostStageRound = -1;					// the round a CT waited for its team on the way in to the hostages
	float m_lsRotateHitAt = -1.0f;					// the site hit (its time) it decided about
	int m_lsCallRound = -1;							// the round it said yes to a person's "rush b" / "go b" (in the push, whatever its habits)
	int m_lsCallSkipRound = -1;						// the round it heard that call and did its own thing instead
	float LsEntryScore();							// how fit it is to go in first (style, nerve, gun, health)
	float m_lsTradeGoAt = 0.0f;						// a trader goes in a beat after the entry
	int m_lsPerchRound = -1;						// the round an AWPer took a perch
	float m_lsPerchTryAt = 0.0f;
	void LsExecuteGo(const CCSBotManager::Zone *site);
	int LsPickRole();
	bool LsTakePerch(const CCSBotManager::Zone *site, bool tSide);
	float m_lsDefuseNoticeAt = -1.0f;				// when it takes in the defuse it hears
	int m_lsWinReloadRound = -1;
	int m_lsMoraleHideRound = -1;					// the round it hid on low morale (once is enough)
	float m_lsSwapCheckAt = 0.0f;
	float m_lsEmptyTagAt = 0.0f;					// out of bullets: when it next looks for a teammate to stay behind
	int m_lsEmptyRound = -1;
	bool m_lsEmptyBold = false;						// this round, out of bullets, it keeps on with the knife
	EntityHandle<CBaseEntity> m_lsSwapBox;		// the gun it's walking over to swap for
	EntityHandle<CBaseEntity> m_lsSwapLast[4];	// the last few it went for (one try each)
	float m_lsSwapLastAt[4] = {};
	int m_lsSwapLastSlot = 0;
	float m_lsCoverPickAt = 0.0f;					// when it last picked a spot to take cover at
	Vector m_lsLastCornerLook;						// the last corner it checked, and when
	float m_lsLastCornerLookAt = 0.0f;
	float m_lsNoiseHoldUntil = 0.0f;				// holding an angle on a sound instead of walking to it, until
	Vector m_lsNoiseHoldAt;							// where that sound was
	float m_lsNoiseRollAt = 0.0f;					// when it last decided hold-or-not for a sound, and where
	Vector m_lsNoiseRollPos;
	float m_lsPressure = 0.0f;						// how much is coming at it right now (hits taken, teammates dropping, outnumbered)
	float m_lsPressureAt = 0.0f;
	float m_lsPressureHp = 100.0f;
	float m_lsPressureDeathAt = 0.0f;
	void LsPressureTick();							// builds and fades it; publishes the strain for the aim plugin
	float m_lsSwapUntil = 0.0f;
	float m_lsSwapDropAt = 0.0f;					// when it dropped its own for the swap
	Vector m_lsSwapDropPos;					// mid-round: next look for a better gun on the floor
	int m_lsEscortRound = -1;						// the round it went with a late carrier
	float m_lsEscortCheckAt = 0.0f;					// the round it reloaded after winning
	float m_lsFetchCheckAt = 0.0f;
	float m_lsCarrierCheckAt = 0.0f;				// the carrier: next look at whether to let someone go first
	int m_lsCarrierRound = -1;
	int m_lsCarrierWaits = 0;
	int m_lsCarrierFollowRound = -1;
	const CCSBotManager::Zone *m_lsPlantOrderZone = nullptr;	// a person told it where to plant
	int m_lsPlantOrderRound = -1;
	EntityHandle<CBasePlayer> m_lsGiveTo;			// a person asked for the bomb: handing it over
	float m_lsGiveUntil = 0.0f;
	float m_lsGiveMoveAt = 0.0f;
	char m_lsGiveItem[24] = "";						// handing over a gun it bought for them (empty: the bomb)
	bool m_lsGivePickup = false;					// its own gun dropped when it bought that one: picked up after
	Vector m_lsGivePickupAt;				// the round the carrier went along with a teammate
	float m_lsCarrierFollowUntil = 0.0f;
	EntityHandle<CBasePlayer> m_lsCarrierLeader;					// a T elsewhere: next look at a dropped bomb
	float m_lsDefuseCheckAt = 0.0f;					// (looked at four times a second, not every frame)
	float m_lsDefuseActedAt = -100.0f;				// went for a defuse it heard (not while hiding) then
	float m_lsDefuseHeardLast = -10.0f;			// last time it was told of a defuse going on
	int m_lsForTimeRoll = -1;						// this fight: does it play for time (-1 not rolled)
	float m_lsLateBombAt = 0.0f;					// a late carrier: next look at whether it's off to a site
	int m_lsClockRound = -1;
	float m_lsHostHoldAt = 0.0f;					// hostage maps: a T's next go at holding near them
	int m_lsHostRushRound = -1;					// the round a CT hurried for the hostages late						// the round a CT settled in to run the clock down
	int m_lsPlantRound = -1;						// the round it picked a plant spot
	Vector m_lsPlantSpot;
	float m_lsPlantBy = 0.0f;						// ...and plants wherever it is by then
	int m_lsFloorRound = -1;						// the round it went for a floor gun
	int m_lsYieldRound = -1;						// the round its kit yields were counted for
	int m_lsYields = 0;							// times it left the bomb to a kit this round (twice at most)
	float m_lsStaleCheckAt = 0.0f;
	int m_lsStaleRound = -1;						// the round it went looking to end a stalemate
	int m_lsStaleLast = -1;							// ...and as the last one alive (again every 20 s)
	float m_lsStaleLastAt = -100.0f;
	EntityHandle<CBaseEntity> m_lsFloorGun;			// the one it's going for (others leave it to it)
	// (end team members)
	// Loner-Strike nades (cs_bot_ls_nades.cpp): members
	int m_lsNadePhase = 0;							// 0 none, 1 getting it out, 2 throwing, 3 looking away from its flash
	int m_lsNadeId = 0;								// the grenade it's throwing
	Vector m_lsNadeAim;								// where it looks to throw it
	Vector m_lsNadeBurst;							// where it should go off
	float m_lsNadePhaseAt = 0.0f;
	int m_lsNadeAmmoType = 0;
	int m_lsNadeAmmo = 0;							// how many it had when the throw started
	float m_lsNadeNextThink = 0.0f;
	float m_lsNadeNextAt = 0.0f;					// no other grenade till then
	bool m_lsNadeSmokeRolled = false;				// decided about the site smoke this round
	bool m_lsNadeCornerRolled = false;				// decided about a flash for this corner
	Vector m_lsNadeCornerAt;
	int m_lsCalledPlan = 0;							// a person's buy call (0 buy, 1 save, 2 force), for this round:
	int m_lsCalledRound = -1;						//   (LsRoundSerial; -1 none)
	float m_lsThrowUntil = 0.0f;					// a grenade a person asked for: thrown once it's in hand, until then
	Vector m_lsThrowAt;								//   where
	int m_lsBuyPlan = 0;							// this round: 0 the usual buy, 1 save, 2 force
	bool m_lsSaving = false;						// alone against many late on: keeping its gun
	bool m_lsSaveDecided = false;					// decided about that this round
	float m_lsPlantHideSince = 0.0f;				// hiding with the bomb down since (LsPostPlantAware)
	float m_lsSaveNext = 0.0f;
	float m_lsClearSawAt = -10.0f;					// its enemy last seen with no smoke in the way
	float m_lsSmokeLostAt = -10.0f;					// ...and lost (behind smoke or a wall) after that
	float m_lsSmokeUntil = 0.0f;					// still shooting into the smoke where they went till then
	Vector m_lsNadeFrom;							// where it was when it got the grenade out
	Vector m_lsNadeAng;								// the throw's view angles, worked out there
	float m_lsNadeRetryAt = 0.0f;
	bool m_lsPistolBought = false;					// this round's pistol-round buy done					// a smoke or HE plan that didn't fit: not again till then
	// (end nades members)
	// Loner-Strike moves (cs_bot_ls_moves.cpp): members
	Vector m_lsCornerAt;							// the corner the plan is for
	int m_lsCornerPlan = 0;							// what it does there
	Vector2D m_lsCornerShift;						// a wide swing or the outside line: off the path by this much
	bool m_lsCornerBlind = false;					// it couldn't see past the corner when the plan started
	float m_lsJiggleUntil = 0.0f;					// a jiggle peek under way until
	float m_lsJigglePhaseEnd = 0.0f;
	int m_lsJigglePhase = 0;						// 0 out, 1 back
	int m_lsJiggleLeft = 0;							// out-and-backs left
	Vector2D m_lsJiggleOut;							// the way out round the corner
	Vector m_lsJiggleLook;							// where someone past it would be
	float m_lsNextJiggle = 0.0f;
	Vector m_lsPrefireAt;							// prefiring this spot
	float m_lsPrefireUntil = 0.0f;
	float m_lsPrefireFireEnd = 0.0f;				// the burst ends then (0: not aimed yet)
	float m_lsNextPrefire = 0.0f;
	Vector m_lsBackFrom;							// falling back from a threat here
	float m_lsBackUntil = 0.0f;
	float m_lsNextGlance = 0.0f;
	int m_lsBackStyle = 0;							// 2 eyes on it, 1 glances back
	float m_lsPanicUntil = 0.0f;					// a newer player panicking until
	int m_lsPanicKind = 0;							// 1 walk, 2 crouch
	float m_lsLadderFor = -1.0f;					// the ladder (its start time) the climb was rolled for
	int m_lsLadderSide = 0;
	const CNavLadder *m_lsBadLadder = nullptr;		// a ladder it got stuck on (not used again till m_lsBadLadderUntil)
	float m_lsBadLadderUntil = 0.0f;
	const CNavArea *m_lsFailArea = nullptr;			// the last area it gave up getting into
	float m_lsFailAt = -100.0f;
	const CNavArea *m_lsBadAreas[4] = {};			// areas it got stuck on twice (costly till m_lsBadAreaUntil)
	float m_lsBadAreaUntil[4] = {};
	// (end moves members)
	float m_lsBangHeardAt = -10.0f;					// last sprayed a wall at someone heard behind it
	float m_lsPreAimAt = -10.0f;					// last pre-aimed at footsteps
	float m_lsHeardAt = -10.0f;						// last weighed something heard
	EntityHandle<CBasePlayer> m_lsShotBy;			// whose bullets those were
	Vector m_lsPeekCorner;							// the corner its peek was decided for
	bool m_lsPeekThis = false;
	int m_lsSneakRound = -1;						// the round the late-round sneak was rolled for
	bool m_lsSneakLate = false;
	float m_lsSneakLateEnd = 0.0f;					// ...until this far into the round
	Vector m_lsLastPos;							// where it was last frame (teleports)
	float m_lsLastPosAt = -1.0f;
	float m_lsBangUntil = 0.0f;						// spraying through a wall at where the enemy went until then
	float m_lsBangRolledAt = -1.0f;					// the lost sight it decided about
	Vector m_lsBangAt;
	float m_lsShotAtAt = -1.0f;						// enemy bullets landed (or passed) close by then
	Vector m_lsShotFrom;							// ...fired from here
	float m_lsCheckUntil = 0.0f;					// planting or defusing on hold: checking a threat until then
	float m_lsObjUseAt = 0.0f;						// ...started using again then
	float m_lsObjRolledAt = -1.0f;
	int m_lsObjStops = 0;							// how often this plant or defuse has stopped for a threat					// the last threat it decided about
	const char *m_lsWhy = "";						// why it's going where it's going (bot_ls_trace)
	float m_lsWhyAt = 0.0f;
	float m_lsTraceAt = 0.0f;

public:
	// Loner-Strike modes (cs_bot_ls_modes.cpp): what a game mode's components ask of the bot
	// through ls_mode (hold here, capture that, loot this, stay in the zone, shoot those). All of
	// it is off until a mode sends something: no goals, no zones, no change from the plain brain
	struct LsModeGoal
	{
		int kind = 0;								// LS_GOAL_*, 0 the slot is free
		Vector at;
		float radius = 0.0f;						// what counts as there (capture, extract, hold area)
		int ent = 0;								// the entity it's about (a crate, a platform), 0 none
		EntityHandle<CBaseEntity> thing;			// ...kept by handle (gone: the goal ends)
		Vector onMesh;								// the point on the nav mesh it walks to
		int prio = 50;								// 0-100: 80 and up every bot does it, below 30 a hint
		float until = 0.0f;							// 0 until cleared
		bool care = true;							// this bot took it up (its persona decided, once)
		int phase = 0;								// 0 on its way, 1 there (holding, using, riding)
		float phaseUntil = 0.0f;					// ...that lasts till then
		float nextTry = 0.0f;						// the next MoveTo, if it's been sent off its way
		int tries = 0;								// times sent back on its way without getting there
	};
	enum { LS_MODE_GOALS = 4 };
	void LsModeSetGoal(int kind, const Vector &at, float radius, int ent, int prio, float secs);
	void LsModeClearGoals(int kind, int ent);		// kind 0 and ent 0: all of them
	bool LsModeHasGoal() const;
	void LsModeTick();								// each think after a person's order (cs_bot_update.cpp)
	void LsModeInside(unsigned int *avoidMask) const;	// the avoid zones it's standing in (paths may leave them)
	void LsModePostUpdate();						// after the state machine: a stand-still or step a goal asked for
	bool LsModePeace();								// a mode's peace (a loot-up), or the moment after it: no player enemies
	bool LsModeGearing();							// just after the peace: gearing up before hunting
	bool LsModeKitted();							// armor, a pistol and a spare magazine beside a main gun
	bool LsModeOfferTick();							// a perk, the bench, the box, the trap, a turret: when to take one
	float m_lsModeOfferAt = 0.0f;
	bool LsModePickHigh(Vector *out);				// a sightline spot on high ground
	bool LsModePerchTick();							// high ground held a while, then the next along
	float m_lsModePerchUntil = 0.0f;				// holding its high spot till then
	float m_lsModePerchAt = 0.0f;					// next perch decision
	int LsModeLayerWant();							// the layer it would rather loot on (-1 anywhere: a hot dropper)
	bool LsModeLetsBe(CBasePlayer *pPlayer);		// ...someone in sight left be (not right on it, nor coming at it)
	const Vector *LsModeSpot() const;				// the spot it holds for a station (teammates pick others)
	int LsModeGoalNow() const { return m_lsModeGoalOn >= 0 ? m_lsModeGoal[m_lsModeGoalOn].kind : 0; }	// what it's on (trace)
	const LsModeGoal &LsModeGoalAt(int i) const { return m_lsModeGoal[i]; }
private:
	LsModeGoal m_lsModeGoal[LS_MODE_GOALS];
	int m_lsModeGoalOn = -1;						// the slot it's acting on now (-1 none)
	Vector m_lsModeMoveTo;							// where its last mode MoveTo was headed
	float m_lsModeMoveAt = -1.0f;					// ...sent then
	float m_lsModeEscapeAt = -1.0f;					// last sent out of a zone then
	float m_lsModeEscapeState = -1.0f;				// ...the MoveTo that started (its state time)
	float m_lsModeScanAt = 0.0f;					// next look for a mode enemy (a creature)
	EntityHandle<CBaseEntity> m_lsModeTarget;		// the mode enemy it's shooting
	float m_lsModeStillUntil = 0.0f;				// standing still till then (on a platform, waiting for it)
	Vector m_lsModeStepTo;							// stepping onto a platform...
	float m_lsModeStepUntil = 0.0f;					// ...till then
	bool LsModeMove(const Vector &to, bool hurry);	// a MoveTo of its own (false: already on its way there)
	bool LsModeHeadedTo(const Vector &to) const;
	bool LsModeEscape();							// out of a no-go zone, into the safe zone (true: on its way)
	void LsModeGoalTick(LsModeGoal &g);
	bool LsModeAttractTick(bool gearing = false);	// hunting: off to somewhere a mode says is worth going (true: sent)
	float m_lsModeAttractAt = 0.0f;					// next roll for that
	bool LsModeSees(CBaseEntity *pEntity, bool testFOV, Vector *at, int *traces);	// a point on a mode enemy in sight
	bool LsModeOursHiding() const;					// the hide it's in is a station's
	bool LsModePickSpot(const Vector &center, float radius, Vector *out);	// a spot of its own in an area
	void LsModeStationTick(LsModeGoal &g, float dist);	// hold, defend, capture, extract, meeting
	bool LsModePushTick(const LsModeGoal &g);		// defending: out to a creature now and then
	bool LsModeRoamTick();							// a creature mode's free players: roam like a person
	bool LsModeHome(Vector *home);					// where its side is based (teammates' held spots)
	float m_lsModeRoamAt = 0.0f;					// next roam decision
	float m_lsModeStrafeUntil = 0.0f;				// a sidestep between bursts till then
	bool m_lsModeStrafeLeft = false;
	float m_lsModeBlockedSince = -1.0f;				// its mode enemy's been behind something bullets stop since
	float m_lsModeAngleAt = 0.0f;					// next move for another angle
	void LsModePeaceTick();
	void LsModeRoomTick(float reach);				// room from anyone close (a peace, gearing up)
	float m_lsModePeaceMoveAt = 0.0f;				// next "giving them room" check
	float m_lsModeGearFor = -1.0f;					// the peace end its gearing up is for
	bool m_lsModeGearDone = false;
	int m_lsModePressed = 0;						// buttons the mode pressed this think (1 fire, 2 use, 4 reload)
	float m_lsModeHideState = -1.0f;				// the station's hide began then (its state time)
	Vector m_lsModeSpot;							// ...at this spot
	bool m_lsModeSpotOn = false;
	float m_lsModeSawAt = -100.0f;					// a mode enemy last in sight then
	float m_lsModeCoverAt = 0.0f;					// next "hurt: to cover" allowed
	Vector m_lsModeBackFrom;						// backing off from a creature close up...
	float m_lsModeBackUntil = 0.0f;					// ...till then
	float m_lsModePushUntil = 0.0f;					// out pushing (or tagging along) till then: the station waits
	float m_lsModePushAt = 0.0f;					// next push roll
	bool LsModeShootTick();							// a mode enemy (a creature) in sight: shoot it (true: it is)
	float LsModeGreed();							// 0-1: how much loot and pickups pull it
	bool LsModeCares(int kind, int prio);			// a goal's taken up or not (once, by its persona)

public:
	// Loner-Strike deathrun (cs_bot_ls_deathrun.cpp, bot_ls_deathrun): runners run the course, the
	// terrorist works the trap buttons as a player does. Nothing unless it's on, on a deathrun map
	bool LsDrTick();								// each think (cs_bot_update.cpp): true while it's standing still on purpose
	void LsDrPostUpdate();							// after the state machine: waiting, stepping up to a button, pressing it
	void LsDrPathTick();							// in path movement: a gap to jump, a teleport, a floor not there yet
	bool LsDrRunner() const;						// a runner (deathrun players pass through each other: no queueing)
private:
	bool LsDrActivatorTick();
	bool LsDrJumpGap(int i);						// the floor ahead is gone: jump to the next one, if it's in reach
	bool LsDrFloorGone(int i, const CNavArea *B);	// waited long enough for it: another way, or (none) a jump / keep waiting
	int m_lsDrLife = -1;							// the life its run is for (a respawn starts a new one)
};

// Loner-Strike: how far a person could see from one point toward another, 1 all the way: doors,
// breakable windows and glass looked through (cs_bot_vision.cpp)
float LsClearTo(const Vector &from, const Vector &to, edict_t *self);

// Loner-Strike: each living teammate of a bot, other than itself (cs_bot_ls*.cpp)
template <typename F>
inline void LsForTeammates(CCSBot *me, F f)
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer *pPlayer = UTIL_PlayerByIndex(i);
		if (!pPlayer || pPlayer == me || !pPlayer->IsAlive() || me->BotRelationship(pPlayer) != CCSBot::BOT_TEAMMATE)
			continue;
		f(pPlayer);
	}
}

// Loner-Strike: the playstyles gm_people tells the bot brain (LsStyle)
enum { LS_STYLE_RUSHER, LS_STYLE_ROAMER, LS_STYLE_LURKER, LS_STYLE_CAREFUL, LS_STYLE_CAMPER, LS_STYLE_AWPER,
	LS_STYLE_KNIFER, LS_STYLE_NADER, LS_STYLE_NINJA, LS_STYLE_OBJECTIVE, LS_STYLE_WEIRDGUN };

// Loner-Strike: the goals a mode can give a bot (ls_mode goal, cs_bot_ls_modes.cpp)
enum { LS_GOAL_NONE, LS_GOAL_HOLD, LS_GOAL_DEFEND, LS_GOAL_CAPTURE, LS_GOAL_EXTRACT, LS_GOAL_LOOT, LS_GOAL_USE,
	LS_GOAL_RIDE, LS_GOAL_INVESTIGATE, LS_GOAL_FLEE, LS_GOAL_MEETING, LS_GOAL_KINDS };

// Loner-Strike (cs_bot_ls.cpp)
// A bot's taste in routes for one path search (worked out once, used for every area)
struct LsRoute { unsigned int seed, driftSeed; float most; };
float LsRouteFactor(const LsRoute &r, const CNavArea *area);
extern cvar_t cv_bot_ls_own_memory;
extern cvar_t cv_bot_ls_routes;
extern cvar_t cv_bot_ls_lines;
extern cvar_t cv_bot_ls_pauses;
extern cvar_t cv_bot_ls_solo;
extern cvar_t cv_bot_ls_fight;
extern cvar_t cv_bot_ls_match;
extern cvar_t cv_bot_ls_trace;
extern const char LS_PEEK[];					// the look-at of a corner peek
bool LsFight();
bool LsSpotCrowded(CBaseEntity *me, const Vector *pos);	// a teammate at or heading to somewhere right by it
bool LsBulletGetsThrough(CBasePlayer *shooter, int weaponId, const Vector &from, const Vector &to, TraceResult *first);	// by the game's penetration rule
// Loner-Strike: what's known about each map (cs_bot_ls.cpp, "map knowledge"), worked out a
// little each frame and saved next to the map
enum { LS_KNOW_DIRS = 16, LS_KNOW_CORNERS = 8, LS_KNOW_ANGLES = 8, LS_KNOW_OBJECTS = 8 };
enum { LSO_LOW = 1, LSO_DOOR, LSO_BREAKABLE, LSO_BOX, LSO_LADDER };	// object types
enum { LSP_CROUCHED = 1 };				// pair flags: a crouched head is in view too
#pragma pack(push, 1)
struct LsKnownCorner { short x, y, z; unsigned char yaw; };	// where, and which way it opens (0-255 round)
struct LsKnownAngle { unsigned char yaw; signed char pitch; unsigned short dist; unsigned char weight; unsigned short area; };
struct LsKnownObject { unsigned char type; short x, y, z, top; unsigned char mat; };
struct LsAreaKnow
{
	unsigned char coverLow[LS_KNOW_DIRS];	// open distance / 8 at a crouched head, all round
	unsigned char coverHigh[LS_KNOW_DIRS];	// ... at standing eyes
	unsigned char mat[LS_KNOW_DIRS];		// what's there close by (CHAR_TEX_*, 0 none)
	unsigned char ceiling;					// / 4
	unsigned short sees;					// areas in sight
	unsigned char high;						// share (0-255) of those that are lower: high ground
	unsigned char cornerCount, angleCount, objectCount;
	LsKnownCorner corners[LS_KNOW_CORNERS];
	LsKnownAngle angles[LS_KNOW_ANGLES];	// where enemies appear from, the busiest first
	LsKnownObject objects[LS_KNOW_OBJECTS];
	// which of the area's points (3 x 3 across it, LsExposurePoint) each angle sees: at a
	// standing head, and a crouched one. Which side to hug, and when crouching hides you
	unsigned short exposedStand[LS_KNOW_ANGLES];
	unsigned short exposedCrouch[LS_KNOW_ANGLES];
	// cover right next to each of those points, all round (bit d: something solid within 64
	// units that way): at a crouched head, and at standing eyes. Low cover is low and not high
	unsigned short pointCoverLow[9];
	unsigned short pointCoverHigh[9];
};
#pragma pack(pop)
void LsSightStep();
void LsSurfStep();				// bot/cs_bot_surf.cpp: surf maps explored a little each frame (bot_ls_surf)
// Loner-Strike deathrun (bot/cs_bot_ls_deathrun.cpp): obstacle-course nav learning (bot_ls_nav_course)
// and the brain playing deathrun (bot_ls_deathrun)
bool LsNavCourse();
bool LsMoverFloor(const TraceResult &result);
void LsCourseLearnStart();
class CNavNode;
CNavNode *LsCourseSeedSearch();
void LsCourseLinkAreas();
bool LsDrOn();
void LsDrStep();
void LsDrCommand();
void LsDrDrive(CBasePlayer *bot, float *forward, float *strafe, unsigned short *buttons);
bool LsDrCosts();
float LsDrAreaCost(const CNavArea *area);
float LsDrLinkCost(const CNavArea *area, const CNavArea *fromArea);
bool LsDrTeleport(const CNavArea *area, const CNavArea *fromArea);
bool LsSightReady();
bool LsSees(const CNavArea *a, const CNavArea *b);	// someone in one area could see into the other
int LsSightQuality(const CNavArea *a, const CNavArea *b);	// 0-255, how much
const LsAreaKnow *LsKnow(const CNavArea *a);
CNavArea *LsKnowArea(unsigned short index);
bool LsWallbangKnown(const CNavArea *a, const CNavArea *b, float *thick, char *mat);
bool LsExposureReady();
// Loner-Strike: what kind of map this is (by its name and objectives): how bots play it
enum { LSMAP_OBJECTIVE, LSMAP_AIM, LSMAP_FY, LSMAP_AWP, LSMAP_KNIFE, LSMAP_OTHER };
int LsMapKind();
extern int g_lsNavLoads;					// navs loaded so far (a same-name reload is a new nav)
const Vector *LsEnemySpawn(int team);	// the middle of the other team's spawn (null if none)
float LsArrival(const CNavArea *area, int team);	// seconds that team needs, running from its spawn, to get there (-1 unknown)
enum { LSG_NONE, LSG_PISTOL, LSG_DEAGLE, LSG_SMG, LSG_SHOTGUN, LSG_RIFLE, LSG_SNIPER, LSG_MG };
int LsGunClass(CBasePlayer *pPlayer);
void LsNoteAnyDeath(CBasePlayer *pVictim);		// a person's death for the "died there" memory (bots note their own)
bool LsRecentDeathNear(int team, const Vector &at, float range, float secs);	// a player of that side died near there lately	// what kind of gun someone has out
void LsCallHurt(CBasePlayer *pEnemy, int team, float hp, const Vector &where);	// a team was told an enemy's hurt ("he's lit", "1 low"), and where
bool LsKnownHurt(CBasePlayer *pEnemy, int team, float *hp = nullptr, Vector *where = nullptr, float *when = nullptr);	// ...and still knows it (no healing in CS)
int LsAlive(int team);					// how many of a team are alive (the scoreboard shows it)
bool LsHabitNear(const Vector &at, float range);	// a person's spot this map (where they killed a bot from)
int LsMapSerial();						// changes when the map does
float LsMapSafeTime();
bool LsCrossesSky(const Vector &from, const Vector &to, float step);	// a line through sky brushes (traces pass, eyes don't)
bool LsSkyInPlay();
bool LsHeadroom(const CNavArea *area);
int LsLightAt(const Vector &pos);			// how lit a spot is (0 dark - 255), from the map's lighting	// room to stand there (not a nav area wedged under sky or a ceiling)						// this map has sky brushes between places people stand
int LsMapLean();						// +1 a long map (rifles, AWPs), -1 a close one (SMGs, shotguns), 0 neither or unknown					// how long the round's early "safe" part lasts on this map
void LsExposurePoint(const CNavArea *area, int point, Vector *out);	// one of 9 across it (the outer ones by its edges), on the floor
bool LsMatch();
bool LsSolo();
bool LsOwnMemory();
bool LsHeSafe(CCSBot *me, const Vector &burst);
float LsTimeLeft();	// the round's time left (a lot, when the round never runs out)
int LsRoundSerial();
const CCSBotManager::Zone *LsTSite();	// the Ts' site this round
void LsTeamCall(const CCSBotManager::Zone *zone, bool rush);	// a person called the site ("rush b"): the team's site, rushed or gathered
void LsTeamCallOff();					// "nvm": the call's rush is off (the site it named stays)
bool LsTeamCalled(bool *rush);			// a person called this round's site (and whether it was a rush)
void LsCtSiteDeath(CBasePlayer *pVictim);	// a CT down by a site: two in seconds and the site's being hit
void LsTSiteDeath(CBasePlayer *pVictim);	// a T down at it early: two and the team goes for the other	// a count of rounds that only goes up (a restart sets the game's own back to 0)
void Bot_LsRegisterCVars();
void LsTraceMark();						// ls_mark [note]: a moment flagged in lstrace.csv
void Bot_LsServerCommand();
void Bot_LsModeCommand();				// ls_mode (cs_bot_ls_modes.cpp)
void LsModeMapStart();					// a new map: every mode fact forgotten
void LsModePlayerGone(int index);		// a player left: their role and ties forgotten (client.cpp)
bool LsModeCosts();						// any avoid, block or zone set (path costs to add)
bool LsModeNoBuy();						// a mode said there's nothing to buy (ls_mode nobuy 1)
float LsModeAreaCost(const CNavArea *area, unsigned int insideMask);	// 1 plain, more inside an avoid or outside the zone, -1 never

// Inlines
inline float CCSBot::GetCombatRange() const
{
	return m_combatRange;
}

inline void CCSBot::SetRogue(bool rogue)
{
	m_isRogue = rogue;
}

inline void CCSBot::Hurry(float duration)
{
	m_hurryTimer.Start(duration);
}

inline float CCSBot::GetSafeTime() const
{
	return m_safeTime;
}

inline bool CCSBot::IsCarryingBomb() const
{
	return m_bHasC4;
}

inline bool CCSBot::IsFollowing() const
{
	return m_isFollowing;
}

inline CBasePlayer *CCSBot::GetFollowLeader()
{
	return m_leader;
}

inline float CCSBot::GetFollowDuration() const
{
	return gpGlobals->time - m_followTimestamp;
}

inline bool CCSBot::CanAutoFollow() const
{
	return (gpGlobals->time > m_allowAutoFollowTime);
}

inline void CCSBot::AimAtEnemy()
{
	m_isAimingAtEnemy = true;
}

inline void CCSBot::StopAiming()
{
	m_isAimingAtEnemy = false;
}

inline bool CCSBot::IsAimingAtEnemy() const
{
	return m_isAimingAtEnemy;
}

inline bool CCSBot::IsSurprised() const
{
	if (gpGlobals->time - m_surpriseTimestamp < 5.0f)
		return true;

	return false;
}

inline float CCSBot::GetSurpriseDelay() const
{
	if (!IsSurprised())
		return 0.0f;

	return m_surpriseDelay;
}

inline void CCSBot::ClearSurpriseDelay()
{
	m_surpriseDelay = 0.0f;
	m_surpriseTimestamp = 0.0f;
}

inline float CCSBot::GetStateTimestamp() const
{
	return m_stateTimestamp;
}

inline CSGameState *CCSBot::GetGameState()
{
	return &m_gameState;
}

inline const CSGameState *CCSBot::GetGameState() const
{
	return &m_gameState;
}

inline bool CCSBot::IsAtBombsite()
{
	return (m_signals.GetState() & SIGNAL_BOMB) == SIGNAL_BOMB;
}

inline CCSBot::MoraleType CCSBot::GetMorale() const
{
#ifdef REGAMEDLL_ADD
	if (cv_bot_excellent_morale.value != 0.0f)
		return EXCELLENT;
#endif

	return m_morale;
}

inline bool CCSBot::IsNoiseHeard() const
{
	if (m_noiseTimestamp <= 0.0f)
		return false;

	// primitive reaction time simulation - cannot "hear" noise until reaction time has elapsed
	if (gpGlobals->time - m_noiseTimestamp >= GetProfile()->GetReactionTime())
		return true;

	return false;
}

inline void CCSBot::SetTask(TaskType task, CBaseEntity *pEntity)
{
	m_task = task;
	m_taskEntity = pEntity;
}

inline CCSBot::TaskType CCSBot::GetTask() const
{
	return m_task;
}

inline CBaseEntity *CCSBot::GetTaskEntity()
{
	return m_taskEntity;
}

inline CNavArea *CCSBot::GetNoiseArea() const
{
	return m_noiseArea;
}

inline void CCSBot::ForgetNoise()
{
	m_noiseTimestamp = 0.0f;
}

inline PriorityType CCSBot::GetNoisePriority() const
{
	return m_noisePriority;
}

inline BotChatterInterface *CCSBot::GetChatter()
{
	return &m_chatter;
}

inline bool CCSBot::IsUsingVoice() const
{
	return (m_voiceFeedbackEndTimestamp != 0.0f);
}

inline CBasePlayer *CCSBot::GetEnemy()
{
	return m_enemy;
}

inline int CCSBot::GetNearbyEnemyCount() const
{
	return Q_min(GetEnemiesRemaining(), m_nearbyEnemyCount);
}

inline unsigned int CCSBot::GetEnemyPlace() const
{
	return m_enemyPlace;
}

inline bool CCSBot::CanSeeBomber() const
{
	return m_bomber.IsValid();
}

inline CBasePlayer *CCSBot::GetBomber() const
{
	return m_bomber;
}

inline int CCSBot::GetNearbyFriendCount() const
{
	return Q_min(GetFriendsRemaining(), m_nearbyFriendCount);
}

inline CBasePlayer *CCSBot::GetClosestVisibleFriend() const
{
	return m_closestVisibleFriend;
}

inline CBasePlayer *CCSBot::GetClosestVisibleHumanFriend() const
{
	return m_closestVisibleHumanFriend;
}

inline float CCSBot::GetTimeSinceAttacked() const
{
	return gpGlobals->time - m_attackedTimestamp;
}

inline float CCSBot::GetFirstSawEnemyTimestamp() const
{
	return m_firstSawEnemyTimestamp;
}

inline float CCSBot::GetLastSawEnemyTimestamp() const
{
	return m_lastSawEnemyTimestamp;
}

inline float CCSBot::GetTimeSinceLastSawEnemy() const
{
	return gpGlobals->time - m_lastSawEnemyTimestamp;
}

inline float CCSBot::GetTimeSinceAcquiredCurrentEnemy() const
{
	return gpGlobals->time - m_currentEnemyAcquireTimestamp;
}

inline const Vector &CCSBot::GetLastKnownEnemyPosition() const
{
	return m_lastEnemyPosition;
}

inline bool CCSBot::IsEnemyVisible() const
{
	return m_isEnemyVisible;
}

inline float CCSBot::GetEnemyDeathTimestamp() const
{
	return m_enemyDeathTimestamp;
}

inline int CCSBot::GetLastVictimID() const
{
	return m_lastVictimID;
}

#ifdef REGAMEDLL_ADD
inline bool CCSBot::CanSeeSniper(void) const
{
	return m_isEnemySniperVisible;
}

inline bool CCSBot::HasSeenSniperRecently(void) const
{
	return !m_sawEnemySniperTimer.IsElapsed();
}
#endif

inline bool CCSBot::HasPath() const
{
	return m_pathLength != 0;
}

inline void CCSBot::DestroyPath()
{
	m_pathLength = 0;
	m_pathLadder = nullptr;
}

inline CNavArea *CCSBot::GetLastKnownArea() const
{
	return m_lastKnownArea;
}

inline const Vector &CCSBot::GetPathEndpoint() const
{
	return m_path[m_pathLength - 1].pos;
}

inline const Vector &CCSBot::GetPathPosition(int numpath) const
{
	return m_path[numpath].pos;
}

inline bool CCSBot::IsUsingLadder() const
{
	return m_pathLadder != nullptr;
}

inline void CCSBot::SetGoalEntity(CBaseEntity *pEntity)
{
	m_goalEntity = pEntity;
}

template <typename T>
inline T *CCSBot::GetGoalEntity()
{
	return m_goalEntity.Get<T>();
}

inline void CCSBot::ForceRun(float duration)
{
	Run();
	m_mustRunTimer.Start(duration);
}

inline void CCSBot::SetLookAngles(float yaw, float pitch)
{
	m_lookYaw = yaw;
	m_lookPitch = pitch;
}

inline void CCSBot::SetForwardAngle(float angle)
{
	m_forwardAngle = angle;
}

inline void CCSBot::SetLookAheadAngle(float angle)
{
	m_lookAheadAngle = angle;
}

inline void CCSBot::ClearLookAt()
{
	//PrintIfWatched("ClearLookAt()\n");
	m_lookAtSpotState = NOT_LOOKING_AT_SPOT;
	m_lookAtDesc = nullptr;
}

inline bool CCSBot::IsLookingAtSpot(PriorityType pri) const
{
	if (m_lookAtSpotState != NOT_LOOKING_AT_SPOT && m_lookAtSpotPriority >= pri)
		return true;

	return false;
}

inline bool CCSBot::IsViewMoving(float angleVelThreshold) const
{
	if (m_lookYawVel < angleVelThreshold && m_lookYawVel > -angleVelThreshold &&
		m_lookPitchVel < angleVelThreshold && m_lookPitchVel > -angleVelThreshold)
	{
		return false;
	}

	return true;
}

inline bool CCSBot::IsSignificantlyCloser(const CBasePlayer *testPlayer, const CBasePlayer *referencePlayer) const
{
	if (!referencePlayer || !testPlayer)
		return true;

	float testDist = (pev->origin - testPlayer->pev->origin).Length();
	float referenceDist = (pev->origin - referencePlayer->pev->origin).Length();

	const float significantRangeFraction = 0.7f;
	if (testDist < referenceDist * significantRangeFraction)
		return true;

	return false;
}

inline void CCSBot::ClearApproachPoints()
{
	m_approachPointCount = 0;
}

inline bool CCSBot::IsThrowingGrenade() const
{
	return m_isWaitingToTossGrenade;
}

inline void CCSBot::StartRapidFire()
{
	m_isRapidFiring = true;
}

inline void CCSBot::StopRapidFire()
{
	m_isRapidFiring = false;
}

inline CCSBot::ZoomType CCSBot::GetZoomLevel() const
{
	if (m_iFOV > 60.0f)
		return NO_ZOOM;

	if (m_iFOV > 25.0f)
		return LOW_ZOOM;

	return HIGH_ZOOM;
}

inline int CCSBot::GetHostageEscortCount() const
{
	return m_hostageEscortCount;
}

inline void CCSBot::IncreaseHostageEscortCount()
{
	m_hostageEscortCount++;
}

inline void CCSBot::ResetWaitForHostagePatience()
{
	m_isWaitingForHostage = false;
	m_inhibitWaitingForHostageTimer.Invalidate();
}

inline float CCSBot::GetFeetZ() const
{
	if (IsCrouching())
	{
		const Vector crouch(0, 0, -StepHeight);
		return (pev->origin + crouch).z;
	}
	else
	{
		const Vector stand(0, 0, -HalfHumanHeight);
		return (pev->origin + stand).z;
	}
}

inline const Vector *CCSBot::GetNoisePosition() const
{
	if (m_noiseTimestamp > 0.0f)
		return &m_noisePosition;

	return nullptr;
}

inline bool CCSBot::IsAwareOfEnemyDeath() const
{
	if (GetEnemyDeathTimestamp() == 0.0f)
		return false;

	if (!m_enemy.IsValid())
		return true;

	if (!m_enemy->IsAlive() && gpGlobals->time - GetEnemyDeathTimestamp() > (1.0f - GetProfile()->GetSkill()))
		return true;

	return false;
}

inline bool CCSBot::IsNotMoving() const
{
	const float stillSpeed = 10.0f;
	return pev->velocity.IsLengthLessThan(stillSpeed);
}

inline bool CCSBot::HasAnyAmmo(CBasePlayerWeapon *weapon) const
{
	return (weapon->m_iClip != 0 || m_rgAmmo[weapon->m_iPrimaryAmmoType] > 0);
}

class CollectRetreatSpotsFunctor
{
public:
	CollectRetreatSpotsFunctor(CCSBot *me, float range)
	{
		m_me = me;
		m_count = 0;
		m_range = range;
	}

	enum { MAX_SPOTS = 256 };

	bool operator()(CNavArea *area)
	{
		// collect all the hiding spots in this area
		for (auto const spot : *area->GetHidingSpotList())
		{
			if (m_count >= MAX_SPOTS)
				break;

			// make sure hiding spot is in range
			if (m_range > 0.0f)
			{
				if ((*spot->GetPosition() - m_me->pev->origin).IsLengthGreaterThan(m_range))
					continue;
			}

			// if a Player is using this hiding spot, don't consider it
			if (IsSpotOccupied(m_me, spot->GetPosition()))
			{
				// player is in hiding spot
				// TODO: Check if player is moving or sitting still
				continue;
			}

			// don't select spot if an enemy can see it
			if (UTIL_IsVisibleToTeam(*spot->GetPosition() + Vector(0, 0, HalfHumanHeight), OtherTeam(m_me->m_iTeam)))
				continue;

			// don't select spot if it is closest to an enemy
			CBasePlayer *owner = UTIL_GetClosestPlayer(spot->GetPosition());
			if (owner && m_me->m_iTeam != owner->m_iTeam)
				continue;

			m_spot[m_count++] = spot->GetPosition();
		}

		// if we've filled up, stop searching
		if (m_count == MAX_SPOTS)
			return false;

		return true;
	}

	CCSBot *m_me;
	float m_range;

	const Vector *m_spot[MAX_SPOTS];
	int m_count;
};

class FarthestHostage
{
public:
	FarthestHostage(const CCSBot *me)
	{
		m_me = me;
		m_farRange = -1.0f;
	}
	bool operator()(CHostage *hostage)
	{
		if (hostage->IsFollowing(m_me))
		{
			float range = (hostage->Center() - m_me->pev->origin).Length();

			if (range > m_farRange)
			{
				m_farRange = range;
			}
		}

		return true;
	}

	const CCSBot *m_me;
	float m_farRange;
};

// Functor used with NavAreaBuildPath()
// Loner-Strike: glass or wooden floors that broke this round (cs_bot_ls.cpp): routes go round them
bool LsNoteBrokenFloor(CBaseEntity *pEntity);
bool LsOverBrokenFloor(const CNavArea *area);
void LsClearBrokenFloors();

class PathCost
{
public:
	PathCost(CCSBot *pBot, RouteType route = SAFEST_ROUTE, bool lsTaste = false)
	{
		m_bot = pBot;
		m_route = route;
		// Loner-Strike: taste in routes only for the path the bot walks (not for "how far is
		// it" checks, whose ranges were tuned for plain distances)
		m_lsTaste = lsTaste && pBot->LsRouteTaste(route, &m_lsRoute);
		// Loner-Strike: where teammates are now, so the bot doesn't walk in a line behind one
		// down the same corridor (in team play a little; every bot for itself, more)
		m_lsCrowdCount = 0;
		if (m_lsTaste)
			m_lsCrowdCount = pBot->LsCrowdAreas(m_lsCrowd, 32, &m_lsCrowdCost);
		// Loner-Strike: a mode's avoid zones, blocked doors and safe zone, for the path the bot
		// walks (none set: nothing changes); the zones it's already in it may walk out of
		m_lsMode = lsTaste && LsModeCosts();
		// Loner-Strike deathrun: never through a deadly pit, the long way round where people fell
		m_lsDr = lsTaste && LsDrCosts();
		m_lsModeInside = 0;
		if (m_lsMode)
			pBot->LsModeInside(&m_lsModeInside);
	}
	float operator()(CNavArea *area, CNavArea *fromArea, const CNavLadder *ladder)
	{
		const float baseDangerFactor = 100.0f;

		// respond to the danger modulated by our aggression (even super-aggressives pay SOME attention to danger)
		float dangerFactor = (1.0f - (0.95f * m_bot->LsAggro())) * baseDangerFactor;

		if (fromArea == nullptr)
		{
			if (m_route == FASTEST_ROUTE)
				return 0.0f;

			// first area in path, cost is just danger (Loner-Strike: the bot's own, with bot_ls_own_memory)
			return dangerFactor * (LsOwnMemory() ? m_bot->LsGetDanger(area) : area->GetDanger(m_bot->m_iTeam - 1));
		}
		else if ((fromArea->GetAttributes() & NAV_JUMP) && (area->GetAttributes() & NAV_JUMP))
		{
			// cannot actually walk in jump areas - disallow moving from jump area to jump area
			return -1.0f;
		}
		else if (LsOverBrokenFloor(area))
		{
			// (Loner-Strike: over a glass or wooden floor that broke this round: a hole)
			return -1.0f;
		}
		else
		{
			// compute distance from previous area to this area
			float dist;
			if (ladder)
			{
				// (Loner-Strike: a ladder this bot just got stuck on is out for a while)
				// ladders are slow to use
				const float ladderPenalty = 1.0f;
				dist = ladderPenalty * ladder->m_length;
				if (m_bot->LsBadLadder(ladder))
					dist *= 20.0f;
				// (Loner-Strike: with enemies about, a ladder - slow, no gun up - is avoided by the
				// ones who know better)
				if (m_lsTaste && m_bot->LsLadderExposed())
					dist *= 4.0f;

				// if we are currently escorting hostages, avoid ladders (hostages are confused by them)
				//if (m_bot->GetHostageEscortCount())
				//	dist *= 100.0f;
			}
			else
			{
				dist = (*area->GetCenter() - *fromArea->GetCenter()).Length();
				// (Loner-Strike: somewhere it keeps getting stuck getting into: the long way)
				if (m_bot->LsBadArea(area))
					dist *= 20.0f;
			}

			// Loner-Strike: a mode's zones: costlier inside an avoid zone or outside the safe zone,
			// never through a no-go one (a damage zone, a closed door)
			if (m_lsMode)
			{
				float modeCost = LsModeAreaCost(area, m_lsModeInside);
				if (modeCost < 0.0f)
					return -1.0f;
				dist *= modeCost;
			}
			if (m_lsDr)
			{
				float drCost = LsDrAreaCost(area);
				if (drCost < 0.0f)
					return -1.0f;
				dist = dist * drCost + LsDrLinkCost(area, fromArea);
			}

			// Loner-Strike: this bot's favourite (and not so favourite) parts of the map
			if (m_lsTaste)
				dist *= LsRouteFactor(m_lsRoute, area);
			// Loner-Strike: a teammate already there: a bit further round instead
			for (int c = 0; c < m_lsCrowdCount; c++)
				if (m_lsCrowd[c] == area)
					dist += m_lsCrowdCost;

			// compute distance travelled along path so far
			float cost = dist + fromArea->GetCostSoFar();

			// zombies ignore all path penalties
			if (cv_bot_zombie.value > 0.0f)
				return cost;

			// add cost of "jump down" pain unless we're jumping into water
			// (Loner-Strike deathrun: nor through a teleport - a link one way, but no fall)
			if (!area->IsConnected(fromArea, NUM_DIRECTIONS) && !(m_lsDr && LsDrTeleport(area, fromArea)))
			{
				// this is a "jump down" (one way drop) transition - estimate damage we will take to traverse it
				float fallDistance = -fromArea->ComputeHeightChange(area);

				// if it's a drop-down ladder, estimate height from the bottom of the ladder to the lower area
				//if (ladder && ladder->m_bottom.z < fromArea->GetCenter()->z && ladder->m_bottom.z > area->GetCenter()->z)
				//{
				//	fallDistance = ladder->m_bottom.z - area->GetCenter()->z;
				//}

				float fallDamage = m_bot->GetApproximateFallDamage(fallDistance);

				if (fallDamage > 0.0f)
				{
					// if the fall would kill us, don't use it
					const float deathFallMargin = m_bot->LsFallMargin();
					if (fallDamage + deathFallMargin >= m_bot->pev->health)
						return -1.0f;

					// if we need to get there in a hurry, ignore minor pain
					// (Loner-Strike: less of it hurt)
					const float painTolerance = (15.0f * m_bot->LsAggro() + 10.0f) * (LsFight() ? Q_min(1.0f, float(m_bot->pev->health) / 100.0f) : 1.0f);
					if (m_route != FASTEST_ROUTE || fallDamage > painTolerance)
					{
						// cost is proportional to how much it hurts when we fall
						// 10 points - not a big deal, 50 points - ouch!
						cost += 100.0f * fallDamage * fallDamage;
					}
				}
			}

			// if this is a "crouch" area, add penalty
			if (area->GetAttributes() & NAV_CROUCH)
			{
				// these areas are very slow to move through
				real_t crouchPenalty = (m_route == FASTEST_ROUTE) ? 20.0f : 5.0f;

				// avoid crouch areas if we are rescuing hostages
				if (m_bot->GetHostageEscortCount())
				{
					crouchPenalty *= 3.0f;
				}

				cost += crouchPenalty * dist;
			}

			// if this is a "jump" area, add penalty
			if (area->GetAttributes() & NAV_JUMP)
			{
				// jumping can slow you down
				//const float jumpPenalty = (m_route == FASTEST_ROUTE) ? 100.0f : 0.5f;
				const float jumpPenalty = 1.0f;
				cost += jumpPenalty * dist;
			}

			if (m_route == SAFEST_ROUTE)
			{
				// add in the danger of this path - danger is per unit length travelled
				cost += dist * dangerFactor * (LsOwnMemory() ? m_bot->LsGetDanger(area) : area->GetDanger(m_bot->m_iTeam - 1));
			}
			else if (LsOwnMemory() && LsSolo())
			{
				// (Loner-Strike: deathmatch takes the quick way, but not back down the lane it
				// keeps dying in)
				cost += dist * dangerFactor * 0.03f * m_bot->LsGetDanger(area);
			}

			if (!m_bot->IsAttacking())
			{
				// add in cost of teammates in the way
				// approximate density of teammates based on area
				float size = (area->GetSizeX() + area->GetSizeY()) / 2.0f;

				// degenerate check
				if (size >= 1.0f)
				{
					// cost is proportional to the density of teammates in this area
					const float costPerFriendPerUnit = 50000.0f;
					cost += costPerFriendPerUnit * float(area->GetPlayerCount(m_bot->m_iTeam, m_bot)) / size;
				}
			}

			return cost;
		}

		return 0.0f;
	}

private:
	CCSBot *m_bot;
	RouteType m_route;
	bool m_lsTaste;
	LsRoute m_lsRoute;
	CNavArea *m_lsCrowd[32];
	int m_lsCrowdCount;
	float m_lsCrowdCost;
	bool m_lsMode;
	unsigned int m_lsModeInside;
	bool m_lsDr;
};

class FollowTargetCollector
{
public:
	FollowTargetCollector(CBasePlayer *pPlayer)
	{
		m_player = pPlayer;
		m_forward.x = pPlayer->pev->velocity.x;
		m_forward.y = pPlayer->pev->velocity.y;

		float speed = m_forward.NormalizeInPlace();

		const float walkSpeed = 100.0f;
		if (speed < walkSpeed)
		{
			m_cutoff.x = pPlayer->pev->origin.x;
			m_cutoff.y = pPlayer->pev->origin.y;

			m_forward.x = 0.0f;
			m_forward.y = 0.0f;
		}
		else
		{
			const float k = 1.5f;
			real_t trimSpeed = (speed < 200.0f) ? speed : 200.0f;

			m_cutoff.x = pPlayer->pev->origin.x + k * trimSpeed * m_forward.x;
			m_cutoff.y = pPlayer->pev->origin.y + k * trimSpeed * m_forward.y;
		}

		m_targetAreaCount = 0;
	}

	enum { MAX_TARGET_AREAS = 128 };
	bool operator()(CNavArea *area)
	{
		if (m_targetAreaCount >= MAX_TARGET_AREAS)
			return false;

		// only use two-way connections
		if (!area->GetParent() || area->IsConnected(area->GetParent(), NUM_DIRECTIONS))
		{
			if (m_forward.IsZero())
			{
				m_targetArea[m_targetAreaCount++] = area;
			}
			else
			{
				// collect areas in the direction of the player's forward motion
				Vector2D to(((*area->GetCenter()).x - m_cutoff.x), (*area->GetCenter()).y - m_cutoff.y);
				to.NormalizeInPlace();

				if (DotProduct(to, m_forward) > 0.7071f)
				{
					m_targetArea[m_targetAreaCount++] = area;
				}
			}
		}

		return (m_targetAreaCount < MAX_TARGET_AREAS);
	}

	CBasePlayer *m_player;
	Vector2D m_forward;
	Vector2D m_cutoff;
	CNavArea *m_targetArea[MAX_TARGET_AREAS];
	int m_targetAreaCount;
};

void InstallBotControl();
void Bot_ServerCommand();
void Bot_RegisterCVars();
int GetBotFollowCount(CBasePlayer *pLeader);
const Vector *FindNearbyRetreatSpot(CCSBot *me, float maxRange);

void drawProgressMeter(float progress, char *title);
void startProgressMeter(const char *title);
void hideProgressMeter();

bool isSniperRifle(CBasePlayerItem *item);
float StayOnLadderLine(CCSBot *me, const CNavLadder *ladder);
