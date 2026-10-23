#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "GTAIVTrafficData.h"
class USkeletalMesh;
#include "GTAIVVehicle.generated.h"

class UBoxComponent;
class UCameraComponent;
class USpringArmComponent;
class UStaticMeshComponent;
class AGTAIVTrafficCar;

/** A drivable GTA IV vehicle: its body and wheels from the game, driven by a raycast-wheel
 *  model that takes every value from the vehicle's handling.dat row: mass and centre of mass,
 *  suspension (force, compression/rebound damping, upper/lower limits, raise, bias), the
 *  traction curve (max/min grip, peak slip angle, front/rear bias), drive (bias, gears, force,
 *  inertia, top speed), brakes (force, bias), handbrake, steering lock and drag.
 *
 *  Controls: W/S throttle and brake/reverse, A/D steer, Space handbrake, F get out, mouse looks
 *  around (the camera swings back behind the car by itself). Gamepad: RT/LT, left stick, A, Y. */
UCLASS()
class GTAIVTRAFFIC_API AGTAIVVehicle : public APawn
{
	GENERATED_BODY()

public:
	AGTAIVVehicle();

	/** Builds the vehicle (call right after spawning). */
	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void Setup(const FGTAIVVehicleDef& InDef, const FGTAIVPaint& InPaint);

	/** A drivable copy of a traffic car (which is removed). Its driver is pulled out and runs
	 *  from Jacker (or fights back). */
	static AGTAIVVehicle* TakeOver(AGTAIVTrafficCar* Traffic, APawn* Jacker = nullptr);

	/** Spawns a vehicle by name from the exported vehicle set (DA_GTAIV_Vehicles). */
	UFUNCTION(BlueprintCallable, Category = "GTA IV", meta = (WorldContext = "WorldContext"))
	static AGTAIVVehicle* SpawnVehicle(UObject* WorldContext, FName VehicleName, FTransform Transform);

	/** The exported vehicle set (loaded from /Game/GTAIV/Traffic/DA_GTAIV_Vehicles). */
	static UGTAIVVehicleSet* LoadVehicleSet();

	/** Puts a character in the driver's seat (hidden) and gives its controller the car. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void EnterDriver(APawn* Character);

	/** The driver gets out on the left; returns false when going too fast. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	bool ExitDriver(bool bForce = false);

	/** Like F in GTA IV: stops the car first, then the driver gets out. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void RequestExit() { bExitRequested = true; }

	/** Like holding F at speed in GTA IV: the driver jumps out of the moving car. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV")
	void BailOut();

	/** The driver's door: 0 shut .. 1 open. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void SetDoorOpen(float Amount);
	UFUNCTION(BlueprintPure, Category = "GTA IV") float GetDoorOpen() const { return DoorOpen; }
	/** Where someone stands to get in at the driver's door (on the ground). */
	UFUNCTION(BlueprintPure, Category = "GTA IV") FVector GetEntryLocation() const;
	/** The door's open / close sound. */
	void PlayDoorSound(bool bOpen);
	/** The animations hold the controls while getting in and out. */
	void SetInputLocked(bool bLocked) { bInputLocked = bLocked; }
	/** The get-out animation finished: the driver is let go. */
	void FinishExit() { ExitDriver(true); }
	/** The jump-out animation reached the door: throw the driver clear. */
	void FinishBail();
	/** The jacked driver (from the traffic car) starts getting pulled out. */
	void StartVictim();
	bool HasVictim() const { return VictimMesh != nullptr; }

	UPROPERTY(VisibleAnywhere, Category = "GTA IV") TObjectPtr<UStaticMeshComponent> DoorMesh;
	UPROPERTY(VisibleAnywhere, Category = "GTA IV") TObjectPtr<class UPoseableMeshComponent> VictimMesh;

	UFUNCTION(BlueprintPure, Category = "GTA IV") APawn* GetDriver() const { return Driver.Get(); }
	/** Someone (a GTA ped mesh) sitting in the front passenger seat, e.g. Roman in the first mission. */
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void SetPassenger(USkeletalMesh* Mesh);
	UFUNCTION(BlueprintCallable, Category = "GTA IV") void ClearPassenger();
	UFUNCTION(BlueprintPure, Category = "GTA IV") bool HasPassenger() const { return PassengerMesh != nullptr; }
	/** Places the car so its body sits at BodyWorld (the body mesh's own placement, e.g. from a cutscene). */
	void PlaceBodyAt(const FTransform& BodyWorld);
	UFUNCTION(BlueprintPure, Category = "GTA IV") float GetSpeedKmh() const;
	UFUNCTION(BlueprintPure, Category = "GTA IV") int32 GetGear() const { return Gear; }
	UFUNCTION(BlueprintPure, Category = "GTA IV") FString GetDisplayName() const;
	UFUNCTION(BlueprintPure, Category = "GTA IV") FString GetDebugState() const;
	/** Where the driver's pelvis goes (world). */
	UFUNCTION(BlueprintPure, Category = "GTA IV") FVector GetSeatLocation() const;
	/** The front wheels' angle as -1 (full left) .. 1 (full right). */
	UFUNCTION(BlueprintPure, Category = "GTA IV") float GetSteerAmount() const;

	/** Driving input (-1..1); the player's controller fills these, AI/Blueprints can too. */
	UPROPERTY(BlueprintReadWrite, Category = "GTA IV") float ThrottleInput = 0.f;
	UPROPERTY(BlueprintReadWrite, Category = "GTA IV") float SteerInput = 0.f;
	UPROPERTY(BlueprintReadWrite, Category = "GTA IV") bool bHandbrake = false;
	/** Leaves the driving input to Blueprints/AI even while a player is in the car. */
	UPROPERTY(BlueprintReadWrite, Category = "GTA IV") bool bExternalInput = false;

	UPROPERTY(VisibleAnywhere, Category = "GTA IV") TObjectPtr<UBoxComponent> Chassis;
	UPROPERTY(VisibleAnywhere, Category = "GTA IV") TObjectPtr<UStaticMeshComponent> Body;
	UPROPERTY(VisibleAnywhere, Category = "GTA IV") TObjectPtr<USpringArmComponent> CameraArm;
	UPROPERTY(VisibleAnywhere, Category = "GTA IV") TObjectPtr<UCameraComponent> Camera;
	UPROPERTY(VisibleAnywhere, Category = "GTA IV") TArray<TObjectPtr<UStaticMeshComponent>> WheelMeshes;
	UPROPERTY(Transient) TObjectPtr<class UPoseableMeshComponent> PassengerMesh;

	FGTAIVVehicleDef Def;
	FGTAIVPaint Paint;

	virtual void Tick(float DeltaSeconds) override;

private:
	struct FWheelState
	{
		FVector Rest = FVector::ZeroVector;  // wheel centre at static ride height (actor space, cm)
		float Radius = 30.f;
		bool bFront = false;
		bool bSteers = false;
		bool bRight = false;
		float Compression = 0.f;   // m
		float LastCompression = 0.f;
		float Load = 0.f;          // N
		float Spin = 0.f;          // degrees
		float SpinSpeed = 0.f;     // deg/s
		bool bContact = false;
		bool bSkid = false;
		float LatDeflect = 0.f;    // tyre's sideways deflection, m (traction spring)
		FVector Contact = FVector::ZeroVector;
		FVector Normal = FVector::UpVector;
	};
	TArray<FWheelState> Wheels;
	TWeakObjectPtr<APawn> Driver;
	float PassengerTime = 0.f;
	int32 Gear = 1;
	float SteerAngle = 0.f;
	float Revs = 0.f;          // 0..1 engine response (drive inertia)
	float LookIdle = 10.f;     // seconds since the player last moved the camera
	float Travel = 0.31f;      // suspension travel, m
	bool bBuilt = false;
	bool bExitRequested = false;
	float ExitHeld = 0.f;      // seconds the exit key has been held
	double NoExitUntil = 0.0;  // the key press that got in doesn't also get out
	float AirTime = 0.f;       // seconds with no wheel on the ground
	int32 RadioStation = -2;   // what this car's radio is on (-2: not chosen yet, INDEX_NONE: off)
	float DoorOpen = 0.f;
	bool bInputLocked = false;
	bool bExitBraking = false;   // held still while the driver gets out
	float VictimTime = -1.f;   // < 0: not pulled out yet
	FTransform VictimFrom, VictimTo;
	TWeakObjectPtr<APawn> Jacker;

	// engine sound: GTA IV's low / high engine and exhaust loops and the idle, crossfaded by revs
	UPROPERTY() TArray<TObjectPtr<class UAudioComponent>> EngineAudio;   // EngLow, EngHigh, ExLow, ExHigh, Idle, ExIdle
	UPROPERTY() TObjectPtr<class UAudioComponent> HornAudio;
	UPROPERTY() TObjectPtr<class UAudioComponent> SkidAudio;
	bool bEngineOn = false;
	float EngineRpm = 0.f;
	float EngineFade = 0.f;
	float StartDelay = 0.f;
	float RevsOffCooldown = 0.f;
	int32 LastGear = 1;
	float LastThrottle = 0.f;
	void SetEngine(bool bOn);
	void UpdateSounds(float Dt);
	void UpdateVictim(float Dt);
	void OneShot(USoundBase* Sound, float Volume = 1.f, float Pitch = 1.f);

	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	TMap<TWeakObjectPtr<AActor>, double> RecentHits;

	void HitPeds();
	void SetPlayerCooldown(APawn* P) const;

	void ReadInput(float Dt);
	void Simulate(float Dt);
	void UpdateCamera(float Dt);
	void UpdateWheelVisuals(float Dt);
	float GearTopSpeed(int32 G) const;  // m/s
};
