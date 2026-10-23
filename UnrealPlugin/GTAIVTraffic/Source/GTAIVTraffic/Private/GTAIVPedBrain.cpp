#include "GTAIVPlayerComponent.h"

#include "Components/AudioComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "GameFramework/NavMovementInterface.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Character.h"
#include "GTAIVTrafficCar.h"
#include "GTAIVVehicle.h"

namespace
{
	const TCHAR* VoicePath = TEXT("/Game/GTAIV/Characters/Voices/DA_GTAIV_Voices.DA_GTAIV_Voices");
	const FName FleeTag(TEXT("GTAIV_Fleeing"));
	const FName FightTag(TEXT("GTAIV_Fighting"));

	USoundAttenuation* VoiceAttenuation()
	{
		static TStrongObjectPtr<USoundAttenuation> Att;
		if (!Att.IsValid())
		{
			Att.Reset(NewObject<USoundAttenuation>());
			Att->Attenuation.bAttenuate = true;
			Att->Attenuation.bSpatialize = true;
			Att->Attenuation.AttenuationShape = EAttenuationShape::Sphere;
			Att->Attenuation.AttenuationShapeExtents = FVector(400.f);
			Att->Attenuation.FalloffDistance = 3500.f;
		}
		return Att.Get();
	}

	/** GTA IV's female peds: f_* models and the female story characters. */
	bool IsFemaleModel(const FString& Model)
	{
		static const TSet<FString> Story = {TEXT("ig_anna"), TEXT("ig_charise"), TEXT("ig_gracie"), TEXT("ig_ilyena"),
											TEXT("ig_katemc"), TEXT("ig_mallorie"), TEXT("ig_mamc"), TEXT("ig_marnie"),
											TEXT("ig_michelle"), TEXT("ig_sarah"), TEXT("ig_kate"), TEXT("ig_carmen"),
											TEXT("ig_kiki"), TEXT("ig_alex"), TEXT("ig_ashley"), TEXT("ig_ilyena")};
		return Model.StartsWith(TEXT("f_")) || Story.Contains(Model);
	}

	template <typename T>
	T* Pick(const TArray<TObjectPtr<T>>& Items)
	{
		return Items.Num() ? Items[FMath::RandRange(0, Items.Num() - 1)].Get() : nullptr;
	}
}

UGTAIVPedBrain::UGTAIVPedBrain()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UGTAIVPedBrain::BeginPlay()
{
	Super::BeginPlay();
	CreatedAt = GetWorld()->GetTimeSeconds();
}

bool UGTAIVPedBrain::IsPed(const AActor* Actor)
{
	if (!Actor || Actor->IsA<AGTAIVVehicle>() || Actor->IsA<AGTAIVTrafficCar>())
	{
		return false;
	}
	if (Actor->IsA<ACharacter>())
	{
		return true;
	}
	// Mover characters (GASP) are pawns with a skinned body that has a physics asset
	if (!Actor->IsA<APawn>())
	{
		return false;
	}
	TArray<const USkeletalMeshComponent*> Meshes;
	Actor->GetComponents(Meshes, false);
	for (const USkeletalMeshComponent* M : Meshes)
	{
		if (M && M->GetSkeletalMeshAsset() && M->GetPhysicsAsset())
		{
			return true;
		}
	}
	return false;
}

UGTAIVPedBrain* UGTAIVPedBrain::Get(AActor* Ped)
{
	if (!IsPed(Ped))
	{
		return nullptr;  // never on cars or props
	}
	UGTAIVPedBrain* B = Ped->FindComponentByClass<UGTAIVPedBrain>();
	if (!B)
	{
		B = NewObject<UGTAIVPedBrain>(Ped, TEXT("GTAIV_PedBrain"));
		B->RegisterComponent();
	}
	return B;
}

FString UGTAIVPedBrain::GetStateName() const
{
	switch (State)
	{
	case EState::Calm: return TEXT("Calm");
	case EState::Fleeing: return TEXT("Fleeing");
	case EState::Fighting: return TEXT("Fighting");
	default: return TEXT("Hurt");
	}
}

void UGTAIVPedBrain::ResolveVoice()
{
	if (bVoiceResolved)
	{
		return;
	}
	Data = LoadObject<UGTAIVVoiceData>(nullptr, VoicePath);
	// the ped's GTA IV model (its visual override's mesh SK_PED_<model>) picks its voice
	FString Model;
	TArray<UChildActorComponent*> Kids;
	GetOwner()->GetComponents(Kids);
	for (UChildActorComponent* C : Kids)
	{
		if (AActor* Child = C->GetChildActor())
		{
			TArray<USkinnedMeshComponent*> Meshes;
			Child->GetComponents(Meshes);
			for (USkinnedMeshComponent* M : Meshes)
			{
				if (M->GetSkinnedAsset())
				{
					const FString N = M->GetSkinnedAsset()->GetName();
					if (N.StartsWith(TEXT("SK_PED_")))
					{
						Model = N.Mid(7).ToLower();
					}
				}
			}
		}
	}
	if (Model.IsEmpty())
	{
		TArray<USkeletalMeshComponent*> Meshes;
		GetOwner()->GetComponents(Meshes, false);
		for (USkeletalMeshComponent* M : Meshes)
		{
			if (M->GetSkeletalMeshAsset() && M->GetSkeletalMeshAsset()->GetName().Contains(TEXT("Quinn")))
			{
				Model = TEXT("f_");  // GASP's female mannequin
			}
		}
	}
	// a GTA IV ped not dressed yet (its look is applied a moment after spawning): ask again later
	bool bHasPedVisual = !Model.IsEmpty() && Model != TEXT("f_");
	if (!bHasPedVisual && Kids.Num() > 0 && GetWorld()->GetTimeSeconds() - CreatedAt < 2.0)
	{
		return;
	}
	bVoiceResolved = true;
	bFemale = IsFemaleModel(Model);
	if (!Data)
	{
		return;
	}
	if (const FGTAIVVoice* V = Data->Voices.Find(FName(*Model)))
	{
		Voice = *V;
		return;
	}
	TArray<const FGTAIVVoice*> Same;
	for (const TPair<FName, FGTAIVVoice>& P : Data->Voices)
	{
		if (P.Value.bFemale == bFemale)
		{
			Same.Add(&P.Value);
		}
	}
	if (Same.Num())
	{
		Voice = *Same[FMath::RandRange(0, Same.Num() - 1)];
	}
}

const FGTAIVPainSet& UGTAIVPedBrain::Pain() const
{
	static FGTAIVPainSet None;
	return Data ? (bFemale ? Data->PainFemale : Data->PainMale) : None;
}

void UGTAIVPedBrain::Say(const TArray<TObjectPtr<USoundBase>>& Lines, float Cooldown)
{
	ResolveVoice();
	USoundBase* S = Pick(Lines);
	if (!S)
	{
		return;
	}
	LastLine = S->GetName();
	USceneComponent* At = GetOwner()->GetRootComponent();
	UGameplayStatics::SpawnSoundAttached(S, At, NAME_None, FVector(0, 0, 60.f), EAttachLocation::KeepRelativeOffset, true,
										 1.f, FMath::FRandRange(0.97f, 1.03f), 0.f, VoiceAttenuation());
	LineCooldown = Cooldown;
}

void UGTAIVPedBrain::SayPain(bool bHigh)
{
	ResolveVoice();
	Say(bHigh ? Pain().PainHigh : Pain().PainLow, 0.8f);
}

void UGTAIVPedBrain::SayDeath()
{
	ResolveVoice();
	Say(FMath::RandBool() ? Pain().DeathHigh : Pain().DeathLow, 3.f);
}

void UGTAIVPedBrain::SetGaspFlag(const TCHAR* Flag, bool bValue)
{
	// GASP's CharacterInputState / PlayerInputState: Blueprint struct members carry GUID suffixes
	AActor* Owner = GetOwner();
	for (TFieldIterator<FStructProperty> It(Owner->GetClass()); It; ++It)
	{
		const FString N = It->GetName();
		if (!N.StartsWith(TEXT("CharacterInputState")) && !N.StartsWith(TEXT("PlayerInputState")))
		{
			continue;
		}
		void* Struct = It->ContainerPtrToValuePtr<void>(Owner);
		for (TFieldIterator<FBoolProperty> B(It->Struct); B; ++B)
		{
			if (B->GetName().StartsWith(Flag))
			{
				B->SetPropertyValue_InContainer(Struct, bValue);
			}
		}
	}
}

void UGTAIVPedBrain::Threaten(APawn* Ped, AActor* Player, bool bAimed)
{
	if (!IsPed(Ped) || Ped == Player || Ped->Tags.Contains(TEXT("GTAIV_Dead")))
	{
		return;
	}
	if (const UGTAIVWoundComponent* W = Ped->FindComponentByClass<UGTAIVWoundComponent>())
	{
		return;  // already shot
	}
	if (const UGTAIVPlayerComponent* P = Ped->FindComponentByClass<UGTAIVPlayerComponent>())
	{
		if (Ped->IsPlayerControlled())
		{
			return;
		}
	}
	UGTAIVPedBrain* B = Get(Ped);
	if (!B)
	{
		return;
	}
	B->Threat = Player;
	B->ResolveVoice();
	if (B->State == EState::Calm)
	{
		B->StateTime = 0.f;
		if (FMath::FRand() < B->FightChance * (bAimed ? 1.f : 0.5f))
		{
			B->State = EState::Fighting;
			B->ShotTimer = FMath::FRandRange(1.6f, 2.4f);  // draw first
			Ped->Tags.AddUnique(FightTag);
			B->Say(B->Voice.Fight, FMath::FRandRange(4.f, 7.f));
		}
		else
		{
			B->State = EState::Fleeing;
			Ped->Tags.AddUnique(FleeTag);
			B->Say(FMath::RandBool() || B->Voice.Panic.Num() == 0 ? B->Voice.Scream : B->Voice.Panic, FMath::FRandRange(2.5f, 4.f));
		}
	}
	else if (B->State == EState::Fleeing)
	{
		B->StateTime = 0.f;  // keeps running
		if (bAimed && B->LineCooldown <= 0.f)
		{
			B->Say(B->Voice.Panic, FMath::FRandRange(3.f, 5.f));
		}
	}
}

void UGTAIVPedBrain::React(AActor* Player, bool bFight)
{
	const float Old = FightChance;
	FightChance = bFight ? 2.f : 0.f;
	State = EState::Calm;
	Threaten(Cast<APawn>(GetOwner()), Player, true);
	FightChance = Old;
}

void UGTAIVPedBrain::Move(FVector Dir, float Speed)
{
	// GASP's Mover characters take moves through their nav movement (like AI path following);
	// character movement takes plain movement input
	APawn* Pawn = Cast<APawn>(GetOwner());
	for (UActorComponent* C : Pawn->GetComponents())
	{
		if (C && !C->IsA<UCharacterMovementComponent>())
		{
			if (INavMovementInterface* Nav = Cast<INavMovementInterface>(C))
			{
				Nav->RequestDirectMove(Dir * Speed, Speed >= 600.f);
				return;
			}
		}
	}
	Pawn->AddMovementInput(Dir, FMath::Clamp(Speed / 600.f, 0.1f, 1.f));
}

FString UGTAIVPedBrain::GetDebugState() const
{
	return FString::Printf(TEXT("%s data=%d female=%d panic=%d scream=%d fight=%d last=%s"), *GetStateName(), Data != nullptr,
						   bFemale, Voice.Panic.Num(), Voice.Scream.Num(), Voice.Fight.Num(), *LastLine);
}

void UGTAIVPedBrain::OnShot()
{
	if (State == EState::Fleeing)
	{
		SetGaspFlag(TEXT("WantsToSprint"), false);
	}
	State = EState::Hurt;
	GetOwner()->Tags.Remove(FleeTag);
	GetOwner()->Tags.Remove(FightTag);
	if (Gun)
	{
		Gun->DropWeapon();
	}
}

void UGTAIVPedBrain::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	StateTime += DeltaTime;
	LineCooldown -= DeltaTime;
	APawn* Pawn = Cast<APawn>(GetOwner());
	AActor* T = Threat.Get();
	if (!Pawn)
	{
		return;
	}
	switch (State)
	{
	case EState::Fleeing:
		if (T)
		{
			FVector Away = Pawn->GetActorLocation() - T->GetActorLocation();
			Away.Z = 0.f;
			Move(Away.GetSafeNormal());
		}
		SetGaspFlag(TEXT("WantsToWalk"), false);
		SetGaspFlag(TEXT("WantsToSprint"), true);
		if (LineCooldown <= 0.f && StateTime < FleeSeconds * 0.6f && FMath::FRand() < 0.4f)
		{
			Say(Voice.Panic, FMath::FRandRange(3.f, 6.f));
		}
		else if (LineCooldown <= 0.f)
		{
			LineCooldown = 2.f;
		}
		if (StateTime > FleeSeconds)
		{
			State = EState::Calm;
			SetGaspFlag(TEXT("WantsToSprint"), false);
			SetGaspFlag(TEXT("WantsToWalk"), true);
			Pawn->Tags.Remove(FleeTag);
		}
		break;
	case EState::Fighting:
	{
		if (!Gun)
		{
			Gun = Pawn->FindComponentByClass<UGTAIVPlayerComponent>();
			if (!Gun)
			{
				Gun = NewObject<UGTAIVPlayerComponent>(Pawn, TEXT("GTAIV_PedGun"));
				Gun->bDressAsNiko = false;
				if (const UGTAIVPlayerComponent* PG = T ? T->FindComponentByClass<UGTAIVPlayerComponent>() : nullptr)
				{
					Gun->Weapon = PG->Weapon;
				}
				Gun->RegisterComponent();
			}
			Gun->DrawWeapon();
		}
		const UGTAIVPlayerComponent* Target = T ? T->FindComponentByClass<UGTAIVPlayerComponent>() : nullptr;
		if (!T || (Target && Target->IsWasted()) || FVector::Dist(T->GetActorLocation(), Pawn->GetActorLocation()) > 6000.f)
		{
			if (StateTime > 3.f)
			{
				Gun->SetAimOverride(false);
				Gun->HolsterWeapon();
				State = EState::Calm;
				Pawn->Tags.Remove(FightTag);
			}
			break;
		}
		// face the threat and shoot at it
		const FVector Chest = T->GetActorLocation() + FVector(0, 0, 40.f);
		FRotator Want = (Chest - Pawn->GetActorLocation()).Rotation();
		Want.Pitch = Want.Roll = 0.f;
		Pawn->SetActorRotation(FMath::RInterpTo(Pawn->GetActorRotation(), Want, DeltaTime, 8.f));
		Gun->SetAimOverride(true);
		Gun->SetAimTarget(Chest);
		ShotTimer -= DeltaTime;
		if (ShotTimer <= 0.f && Gun->FireAtTarget(8.f))
		{
			ShotTimer = FMath::FRandRange(0.9f, 1.8f);
		}
		if (LineCooldown <= 0.f)
		{
			Say(Voice.Fight, FMath::FRandRange(5.f, 9.f));
		}
		break;
	}
	default:
		break;
	}
}
