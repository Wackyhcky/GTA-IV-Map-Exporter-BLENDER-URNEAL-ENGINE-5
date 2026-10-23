#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/DataAsset.h"
#include "Engine/HitResult.h"
#include "InputCoreTypes.h"
#include "GTAIVPlayerComponent.generated.h"

class UAnimSequence;
class UPointLightComponent;
class UPoseableMeshComponent;
class USkeletalMesh;
class USkeletalMeshComponent;
class USoundBase;
class UStaticMesh;
class UStaticMeshComponent;
class UTexture2D;
class UMaterialInterface;
class UDecalComponent;
class UInstancedStaticMeshComponent;

/** One GTA IV animation clip baked for the shared GTA ped skeleton: each bone's local transform
 *  per frame (Keys[Frame * Bones.Num() + Bone]). */
USTRUCT(BlueprintType)
struct FGTAIVAnimClip
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float Fps = 30.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") int32 Frames = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<FName> Bones;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<FTransform> Keys;
	/** Clips with root motion (getting in and out of cars): the pelvis's movement per frame,
	 *  component space, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<FVector> PelvisOffset;

	float Duration() const { return Frames > 1 ? (Frames - 1) / Fps : 0.f; }
};

/** A GTA IV weapon for the player: model, animations (gun@handgun) and sounds, written by the
 *  GTA IV Exporter from the game's files. */
UCLASS(BlueprintType)
class GTAIVTRAFFIC_API UGTAIVWeaponData : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Model") TObjectPtr<UStaticMesh> Mesh;
	/** Bone of the GTA ped skeleton the weapon is held by. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Model") FName HandBone = TEXT("Char_R_Hand");
	/** Weapon model relative to the hand bone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Model") FTransform Grip;
	/** Barrel end in the weapon model's space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Model") FVector Muzzle = FVector(15, 0, 3);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Model") TObjectPtr<UTexture2D> Icon;

	/** fire, reload, unholster, holster (+ _crouch variants), by name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation") TMap<FName, FGTAIVAnimClip> Clips;
	/** The same clips as Animation Sequences on the ped skeleton (for use in your own Blueprints). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation") TMap<FName, TObjectPtr<UAnimSequence>> Sequences;
	/** Frame of the fire clip held while aiming (after the gun is raised). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Animation") int32 AimFrame = 20;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sound") TObjectPtr<USoundBase> ShotSound;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sound") TObjectPtr<USoundBase> ShotLowSound;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sound") TObjectPtr<USoundBase> EchoSound;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sound") TObjectPtr<USoundBase> DrySound;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sound") TObjectPtr<USoundBase> ClipInSound;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sound") TObjectPtr<USoundBase> CockSound;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sound") TArray<TObjectPtr<USoundBase>> ImpactSounds;

	/** GTA IV's blood (fxprojtex.wtd): a decal material (Tex, Atlas, Opacity) and its textures. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blood") TObjectPtr<UMaterialInterface> BloodDecal;
	/** blood_01: 2x2 splatter atlas. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blood") TObjectPtr<UTexture2D> BloodSplat;
	/** blood_pool_a/b: pools spreading under the dead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blood") TArray<TObjectPtr<UTexture2D>> BloodPools;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blood") TObjectPtr<UMaterialInterface> BloodDrop;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Blood") TObjectPtr<UStaticMesh> BloodDropMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gameplay") int32 ClipSize = 17;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gameplay") int32 StartAmmo = 170;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gameplay") float Damage = 25.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gameplay") float Range = 12000.f;
	/** Seconds between shots (semi-automatic). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gameplay") float FireInterval = 0.22f;
	/** Push given to what's hit (ragdolls, physics objects). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gameplay") float Impulse = 9000.f;
};

/** Makes a Game Animation Sample character GTA IV's player: shows it as Niko (the GTA IV ped
 *  visual override) and gives it GTA IV's pistol, with the game's draw, aim, fire and reload
 *  animations on the upper body over GASP's locomotion. Shots ragdoll characters they hit
 *  (SandboxCharacter_Mover_Ragdoll's own ragdoll, physics for others) and push physics objects.
 *
 *  Controls: 2 / mouse wheel / D-pad up draw the pistol, 1 puts it away; aim with GASP's aim
 *  (right mouse / left trigger), fire with left mouse / right trigger, reload with T / X
 *  (reloads by itself when the clip is empty). */
UCLASS(ClassGroup = (GTAIV), meta = (BlueprintSpawnableComponent, DisplayName = "GTA IV Player"))
class GTAIVTRAFFIC_API UGTAIVPlayerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGTAIVPlayerComponent();

	/** Show the character as Niko (switches the visual override to the GTA IV ped). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	bool bDressAsNiko = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TSoftObjectPtr<USkeletalMesh> PlayerMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV")
	TSoftClassPtr<AActor> PedVisualClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV|Weapon")
	TObjectPtr<UGTAIVWeaponData> Weapon;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV|Weapon")
	bool bStartArmed = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV|Input") FKey DrawKey = EKeys::Two;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV|Input") FKey HolsterKey = EKeys::One;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV|Input") FKey FireKey = EKeys::LeftMouseButton;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV|Input") FKey ReloadKey = EKeys::T;
	/** Used when the character has no GASP aim state. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV|Input") FKey AimKey = EKeys::RightMouseButton;

	UFUNCTION(BlueprintCallable, Category = "GTA IV") void DrawWeapon();
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void HolsterWeapon();
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void Reload();
	/** Fires once if the weapon is drawn, aimed and ready. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void Fire();

	/** 0..100. Shots from armed peds take it down; at 0 the player is wasted. */
	UPROPERTY(Replicated, EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float Health = 100.f;

	/** Take a hit (from an NPC's gun). */
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void TakeHit(float Damage, FVector From);
	UFUNCTION(BlueprintPure, Category = "GTA IV") bool IsWasted() const { return bWasted; }

	/** Aim at a point instead of where the camera looks (AI). */
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void SetAimTarget(FVector Location) { AimTarget = Location; bHasAimTarget = true; }
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void ClearAimTarget() { bHasAimTarget = false; }
	/** AI shot at the aim target (with some spread); returns true if it fired. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV") bool FireAtTarget(float Spread = 4.f);
	/** Detach the weapon and let it fall (when shot). */
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void DropWeapon();

	/** Reach a hand to a world point (e.g. holding a wound), blended in over a moment. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void SetHandTarget(bool bLeftHand, FVector Location)
	{
		bHandTarget = true; bHandLeft = bLeftHand; HandTarget = Location;
	}
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void ClearHandTarget() { bHandTarget = false; }

	/** Aim without the aim input (scripts, cutscenes, AI). */
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void SetAimOverride(bool bAim) { bAimOverride = bAim; }

	UFUNCTION(BlueprintPure, Category = "GTA IV") bool IsArmed() const { return bArmed; }
	UFUNCTION(BlueprintPure, Category = "GTA IV") FString GetDebugState() const;
	UFUNCTION(BlueprintPure, Category = "GTA IV") bool IsAiming() const { return bAiming; }
	UFUNCTION(BlueprintPure, Category = "GTA IV") int32 GetClipAmmo() const { return ClipAmmo; }
	UFUNCTION(BlueprintPure, Category = "GTA IV") int32 GetSpareAmmo() const { return SpareAmmo; }

	/** Gets into the nearest GTA IV vehicle (taking over a traffic car); false if none is close. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV") bool EnterNearestVehicle();
	/** Spawns the next (Step 1) / previous (-1) vehicle of the exported set in front. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void SpawnVehicleInFront(int32 Step = 1);
	/** Game time until which F doesn't get into a car (set when getting out, so the same key press
	 *  doesn't put the player straight back in). */
	double VehicleCooldownUntil = 0.0;

	/** Ragdolls a character (its own ragdoll when it has one, otherwise physics) and pushes it. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	static void RagdollActor(AActor* Actor, FVector Impulse, FVector Location);

	/** Turns a poseable GTA ped mesh's bones by a clip (rotations only), blended by Weight. */
	static void PoseClip(UPoseableMeshComponent* Mesh, const FGTAIVAnimClip& Clip, float Time, float Weight,
						 const TSet<FName>* Mask);

	/** Seats the character at a vehicle's wheel (shown sitting, GTA IV's driving animations);
	 *  nullptr gets it out. Returns false when it can't be shown sitting (hide it then). */
	bool SetSeat(class AGTAIVVehicle* Vehicle, bool bJack = false);
	bool IsSeated() const { return SeatVehicle.IsValid(); }
	/** GTA IV's get-out (opening the door, out, closing it); the car lets go when it's done. */
	bool StartExit();
	/** GTA IV's jump out of a moving car; the car throws the player clear when it's done. */
	bool StartJumpOut();
	/** Getting in or out (the car's controls wait). */
	bool IsBusyInVehicle() const { return SeatPhase != ESeatPhase::None && SeatPhase != ESeatPhase::Seated; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Automated tests: an action done on the next tick ("draw", "holster", "fire", "enter",
	 *  "spawn", "aim1", "aim0"), as if the player pressed the key (scripts can't send RPCs). */
	UPROPERTY(Transient, BlueprintReadWrite, Category = "GTA IV|Debug") FString DebugAction;

	/** The character this player is shown as (multiplayer: the host is Niko, everyone else a
	 *  GTA IV ped); null keeps the character's own look (NPCs). */
	UPROPERTY(ReplicatedUsing = OnRep_Model, BlueprintReadOnly, Category = "GTA IV") TObjectPtr<USkeletalMesh> ModelMesh;

	// -- multiplayer: the owning player asks, the server decides
	UFUNCTION(Server, Reliable) void ServerDraw();
	UFUNCTION(Server, Reliable) void ServerHolster();
	UFUNCTION(Server, Reliable) void ServerReload();
	UFUNCTION(Server, Unreliable) void ServerSetAim(bool bAim, FVector_NetQuantize Point);
	UFUNCTION(Server, Reliable) void ServerFire(FVector_NetQuantize Start, FVector_NetQuantizeNormal Dir);
	UFUNCTION(Server, Reliable) void ServerEnterVehicle();
	UFUNCTION(Server, Reliable) void ServerSpawnVehicle(int32 Step);
	/** Everyone sees and hears a shot (but the shooter, who played it already). */
	UFUNCTION(NetMulticast, Unreliable) void MulticastShot(FVector_NetQuantize Impact, bool bHit);
	UFUNCTION(NetMulticast, Reliable) void MulticastRagdoll(AActor* Actor, FVector_NetQuantize Impulse, FVector_NetQuantize Location);
	UFUNCTION(NetMulticast, Unreliable) void MulticastPain(bool bDeath);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	enum class EAction : uint8 { None, Draw, Holster, Reload };

	UPROPERTY(ReplicatedUsing = OnRep_Armed) bool bArmed = false;
	UPROPERTY(Replicated) bool bAimNet = false;
	UPROPERTY(Replicated) FVector_NetQuantize AimPointNet;
	UPROPERTY(ReplicatedUsing = OnRep_Wasted) bool bWastedNet = false;
	UPROPERTY(ReplicatedUsing = OnRep_Seat) TObjectPtr<class AGTAIVVehicle> SeatVehicleNet;
	UPROPERTY(ReplicatedUsing = OnRep_Seat) uint8 SeatPhaseNet = 0;
	UPROPERTY(Replicated) bool bJackNet = false;
	UFUNCTION() void OnRep_Model();
	UFUNCTION() void OnRep_Armed();
	UFUNCTION() void OnRep_Wasted();
	UFUNCTION() void OnRep_Seat();
	void ShotTrace(const FVector& Start, const FVector& Dir);
	bool IsOwningClient() const;
	void PublishSeat();
	void AssignModel();
	double RespawnAt = 0.0;
	TWeakObjectPtr<APlayerController> DeadController;  // whose pawn this was when it died
	float AimSendTimer = 0.f;
	bool bLastAimSent = false;
	bool bAiming = false;
	bool bAimOverride = false;
	TWeakObjectPtr<class AGTAIVVehicle> LastSpawned;
	TWeakObjectPtr<class AGTAIVVehicle> SeatVehicle;
	enum class ESeatPhase : uint8 { None, OpenDoor, Jack, GetIn, CloseIn, Seated, GetOut, CloseOut, JumpOut };
	ESeatPhase SeatPhase = ESeatPhase::None;
	float PhaseTime = 0.f;
	bool bJackPending = false;
	bool bDoorSound = false;
	void UpdateSeatPhase(float Dt, class AGTAIVVehicle* Car, const struct FGTAIVVehicleAnimSet& Set);
	float SeatTime = 0.f;
	float ReverseAlpha = 0.f;
	bool PlaceInSeat();
	bool bHandTarget = false;
	bool bHandLeft = false;
	FVector HandTarget = FVector::ZeroVector;
	float HandAlpha = 0.f;
	void ApplyHandIK(float Dt);
	bool bHasAimTarget = false;
	FVector AimTarget = FVector::ZeroVector;
	bool bWasted = false;
	float WastedTime = 0.f;
	double LastDownCheck = 0.0;
	float ThreatTimer = 0.f;
	void ScanThreats(bool bGunshot);
	FString LastHit;
	EAction Action = EAction::None;
	float ActionTime = 0.f;
	float AimAlpha = 0.f;
	float ArmedAlpha = 0.f;
	float Recoil = 0.f;
	float FireCooldown = 0.f;
	float FlashTime = 0.f;
	float DressTimer = 0.f;
	int32 ClipAmmo = 0;
	int32 SpareAmmo = 0;
	bool bReloadSoundPlayed = false;
	bool bCockSoundPlayed = false;

	TSet<FName> ArmBones, UpperBones;
	TWeakObjectPtr<UObject> MaskAsset;
	TWeakObjectPtr<AActor> PedActor;
	TWeakObjectPtr<USkeletalMeshComponent> Source;
	UPROPERTY() TObjectPtr<UPoseableMeshComponent> Poseable;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Gun;
	UPROPERTY() TObjectPtr<UPointLightComponent> Flash;

	struct FPendingPush
	{
		TWeakObjectPtr<AActor> Actor;
		FVector Impulse;
		FVector Location;
		float TimeLeft;
	};
	static TArray<FPendingPush> Pending;
	static bool bLocalRagdoll;   // a client playing the server's ragdoll (don't forward it again)

	USkeletalMeshComponent* OwnerBodyMesh() const;
	bool IsLocalPlayer() const;
	bool OwnerRagdolling() const;
	bool ReadGaspAim(bool& bOutAim) const;
	void Dress();
	void SetupVisual(AActor* Ped);
	void TeardownVisual();
	void HandleInput(float Dt);
	void UpdatePose(float Dt);
	void ApplyClip(const FGTAIVAnimClip& Clip, float Time, float Weight, const TSet<FName>* Mask);
	void PlaySound(USoundBase* Sound, float Volume = 1.f) const;
	FTransform MuzzleTransform() const;
	static void ProcessPending(float Dt);
};


/** Added to characters the GTA IV Player shoots: they bleed (drops falling from the wounds,
 *  splatter), try to get up and collapse again (with the Game Animation Sample's ragdoll
 *  character), then die and lie in a spreading pool of blood. */
UCLASS(ClassGroup = (GTAIV), meta = (BlueprintSpawnableComponent, DisplayName = "GTA IV Wound"))
class GTAIVTRAFFIC_API UGTAIVWoundComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGTAIVWoundComponent();

	/** 0..100; shots take the weapon's damage, a head shot kills. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float Health = 100.f;
	/** Attempts to stand up before dying (random between these). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") int32 MinStruggles = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") int32 MaxStruggles = 2;

	UFUNCTION(BlueprintPure, Category = "GTA IV") bool IsDead() const { return State == EState::Dead; }
	UFUNCTION(BlueprintPure, Category = "GTA IV") FString GetStateName() const;
	UFUNCTION(BlueprintPure, Category = "GTA IV") FString GetDebugState() const;

	/** A shot hit this character (adds the component when needed). */
	static void ApplyHit(AActor* Target, const FHitResult& Hit, FVector Dir, UGTAIVWeaponData* Weapon, AActor* Shooter);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	enum class EState : uint8 { Alive, Down, Struggling, Staggering, Dying, Dead };
	struct FWound { FName Bone; FVector Local; float Age; FName VisualBone; FVector VisualLocal; };
	struct FDrop { FVector Pos; FVector Vel; float Life; float Size; };

	EState State = EState::Alive;
	float StateTime = 0.f;
	float StruggleTime = 1.5f;
	float StaggerTime = 5.f;
	float MoanTimer = 0.f;
	bool bFailGetUp = false;
	int8 HandSide = -1;  // -1 not chosen yet, 0 right, 1 left
	FVector StaggerDir = FVector::ForwardVector;
	int32 StrugglesLeft = 1;
	float DripTimer = 0.f;
	float DeadTime = 0.f;
	float PoolSize = 90.f;
	bool bSettled = false;
	double LastCollapse = -10.0;
	bool bSaidDeath = false;
	TArray<FWound> Wounds;
	TArray<FDrop> Drops;
	UPROPERTY() TObjectPtr<UGTAIVWeaponData> Data;
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> DropMeshes;
	UPROPERTY() TObjectPtr<UDecalComponent> Pool;

	void OnHit(const FHitResult& Hit, FVector Dir);
	USkeletalMeshComponent* Body() const;
	FVector WoundLocation(const FWound& W) const;
	void Spray(FVector From, FVector Dir, int32 Count, float Speed);
	UDecalComponent* Splat(FVector Location, FVector Normal, float Size, UTexture2D* Tex, int32 Cell, float Opacity = 1.f);
	void Collapse(float Push);
	void UpdateDrops(float Dt);
	void HoldWound(bool bHold);
	bool IsStanding() const;
	USkeletalMeshComponent* Visual() const;
};


/** A GTA IV voice: lines from its speech bank (speech.rpf) by context. */
USTRUCT(BlueprintType)
struct FGTAIVVoice
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") bool bFemale = false;
	/** GUN_RUN: panicking at a gun */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<TObjectPtr<USoundBase>> Panic;
	/** SHOCKED: screams */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<TObjectPtr<USoundBase>> Scream;
	/** FIGHT: squaring up */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<TObjectPtr<USoundBase>> Fight;
};

/** Pain and death cries (PAIN_MALE_## / PAIN_FEMALE_## banks). */
USTRUCT(BlueprintType)
struct FGTAIVPainSet
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<TObjectPtr<USoundBase>> PainHigh;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<TObjectPtr<USoundBase>> PainLow;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<TObjectPtr<USoundBase>> DeathHigh;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") TArray<TObjectPtr<USoundBase>> DeathLow;
};

/** GTA IV's ped voices, written by the GTA IV Exporter: voices by ped model (lower case). */
UCLASS(BlueprintType)
class GTAIVTRAFFIC_API UGTAIVVoiceData : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voices") TMap<FName, FGTAIVVoice> Voices;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voices") FGTAIVPainSet PainMale;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Voices") FGTAIVPainSet PainFemale;
};

/** How a ped reacts to the player's gun: screams and runs away, or pulls a pistol and shoots
 *  back; speaks with its GTA IV voice, cries out when shot and when dying. Added to peds by the
 *  GTA IV Player component when it aims at them or fires near them. */
UCLASS(ClassGroup = (GTAIV), meta = (BlueprintSpawnableComponent, DisplayName = "GTA IV Ped Brain"))
class GTAIVTRAFFIC_API UGTAIVPedBrain : public UActorComponent
{
	GENERATED_BODY()

public:
	UGTAIVPedBrain();

	/** Chance (0..1) that a threatened ped fights back instead of fleeing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float FightChance = 0.3f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GTA IV") float FleeSeconds = 12.f;

	/** The player aimed at this ped (bAimed) or fired nearby. */
	static void Threaten(APawn* Ped, AActor* Player, bool bAimed);
	static UGTAIVPedBrain* Get(AActor* Ped);
	/** Whether an actor is a person (a character, not a vehicle or a prop): only they get brains,
	 *  wounds and threats. */
	static bool IsPed(const AActor* Actor);

	UFUNCTION(BlueprintCallable, Category = "GTA IV", meta = (DisplayName = "Get Or Add Ped Brain"))
	static UGTAIVPedBrain* GetOrAddBrain(AActor* Ped) { return Get(Ped); }

	/** React to Player now: fight back (pistol) or run away screaming. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void React(AActor* Player, bool bFight);

	void SayPain(bool bHigh);
	void SayDeath();
	/** Walk/run in a direction this frame (Speed in cm/s; GASP Mover or character movement). */
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void MoveInDirection(FVector Dir, float Speed) { Move(Dir, Speed); }

	/** Shot: stop running/fighting, drop the gun. */
	void OnShot();

	UFUNCTION(BlueprintPure, Category = "GTA IV") FString GetStateName() const;
	UFUNCTION(BlueprintPure, Category = "GTA IV") FString GetDebugState() const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	enum class EState : uint8 { Calm, Fleeing, Fighting, Hurt };
	FString LastLine;
	double CreatedAt = 0.0;
	virtual void BeginPlay() override;
	void Move(FVector Dir, float Speed = 600.f);
	EState State = EState::Calm;
	float StateTime = 0.f;
	float LineCooldown = 0.f;
	float ShotTimer = 1.5f;
	bool bVoiceResolved = false;
	bool bFemale = false;
	FGTAIVVoice Voice;
	TWeakObjectPtr<AActor> Threat;
	UPROPERTY() TObjectPtr<UGTAIVVoiceData> Data;
	UPROPERTY() TObjectPtr<UGTAIVPlayerComponent> Gun;

	void ResolveVoice();
	void Say(const TArray<TObjectPtr<USoundBase>>& Lines, float Cooldown);
	void SetGaspFlag(const TCHAR* Flag, bool bValue);
	const FGTAIVPainSet& Pain() const;
};
