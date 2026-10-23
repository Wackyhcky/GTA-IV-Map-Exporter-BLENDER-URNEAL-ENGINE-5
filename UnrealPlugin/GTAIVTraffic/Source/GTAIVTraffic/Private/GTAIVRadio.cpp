#include "GTAIVRadio.h"

#include "Async/Async.h"
#include "Components/AudioComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundWaveProcedural.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Sound/SoundAttenuation.h"

// ------------------------------------------------------------------ codec

namespace
{
	const int32 Steps[89] = {
		7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
		130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
		1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
		7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
	const int32 Adjust[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

	FORCEINLINE int16 Nibble(int32 N, int32& Pred, int32& Index)
	{
		const int32 Step = Steps[Index];
		int32 D = Step >> 3;
		if (N & 4) D += Step;
		if (N & 2) D += Step >> 1;
		if (N & 1) D += Step >> 2;
		Pred = FMath::Clamp(N & 8 ? Pred - D : Pred + D, -32768, 32767);
		Index = FMath::Clamp(Index + Adjust[N & 7], 0, 88);
		return int16(Pred);
	}

	uint32 ReadU32(const uint8* P) { uint32 V; FMemory::Memcpy(&V, P, 4); return V; }
}

bool FGTAIVRadioStream::Open(TArray<uint8>&& InFile)
{
	File = MoveTemp(InFile);
	const int64 Size = File.Num();
	const uint8* P = File.GetData();
	if (Size < 16 || FMemory::Memcmp(P, "GTR1", 4) != 0)
	{
		return false;
	}
	Rate = int32(ReadU32(P + 4));
	uint16 Ch16;
	FMemory::Memcpy(&Ch16, P + 8, 2);
	Channels = Ch16;
	Frames = ReadU32(P + 12);
	if (Rate <= 0 || Channels <= 0 || Channels > 8 || Frames <= 0)
	{
		return false;
	}
	int64 O = 16;
	for (int32 C = 0; C < Channels; ++C)
	{
		FChannel Ch;
		if (O + 4 > Size) return false;
		Ch.SegCount = ReadU32(P + O);
		O += 4;
		Ch.SegTable = O;
		O += 8 * Ch.SegCount;
		if (O + 4 > Size) return false;
		Ch.Bytes = ReadU32(P + O);
		O += 4;
		Ch.Data = O;
		O += Ch.Bytes;
		if (O > Size) return false;
		Chans.Add(Ch);
	}
	return true;
}

int16 FGTAIVRadioStream::Next(FChannel& C)
{
	const uint8* P = File.GetData();
	while (C.Seg < C.SegCount)
	{
		const int64 N = ReadU32(P + C.SegTable + 8 * C.Seg);
		const int64 Keep = ReadU32(P + C.SegTable + 8 * C.Seg + 4);
		if (C.Sample >= 2 * N || C.SegByte + C.Sample / 2 >= C.Bytes)
		{
			C.SegByte += N;
			++C.Seg;
			C.Sample = 0;
			continue;
		}
		const uint8 B = P[C.Data + C.SegByte + C.Sample / 2];
		// the decoder runs through every nibble (padding included); only the kept ones are heard
		const int16 S = Nibble((C.Sample & 1) ? B >> 4 : B & 15, C.Pred, C.Index);
		if (C.Sample++ < Keep)
		{
			return S;
		}
	}
	return 0;
}

int32 FGTAIVRadioStream::Read(int16* Out, int32 Count)
{
	const int32 N = int32(FMath::Clamp<int64>(Frames - Position, 0, Count));
	for (int32 F = 0; F < N; ++F)
	{
		for (int32 C = 0; C < Channels; ++C)
		{
			Out[F * Channels + C] = Next(Chans[C]);
		}
	}
	Position += N;
	return N;
}

void FGTAIVRadioStream::Skip(int64 Count)
{
	const int64 N = FMath::Clamp<int64>(Frames - Position, 0, Count);
	for (int64 F = 0; F < N; ++F)
	{
		for (int32 C = 0; C < Channels; ++C)
		{
			Next(Chans[C]);
		}
	}
	Position += N;
}

// ------------------------------------------------------------------ loading

UGTAIVRadio* UGTAIVRadio::Get(const UObject* WorldContext)
{
	const UWorld* W = WorldContext ? WorldContext->GetWorld() : nullptr;
	return W ? W->GetSubsystem<UGTAIVRadio>() : nullptr;
}

void UGTAIVRadio::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (GetWorld() && GetWorld()->IsGameWorld())
	{
		Load();
	}
}

void UGTAIVRadio::Deinitialize()
{
	StopAll();
	Super::Deinitialize();
}

TStatId UGTAIVRadio::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGTAIVRadio, STATGROUP_Tickables);
}

void UGTAIVRadio::Load()
{
	DataDir = FPaths::ProjectContentDir() / TEXT("GTAIV/RadioData");
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *(DataDir / TEXT("radio.json"))))
	{
		return;  // the radio wasn't exported
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		return;
	}
	auto Clips = [](const TSharedPtr<FJsonObject>& O, const FString& Field)
	{
		TArray<FClip> Out;
		const TArray<TSharedPtr<FJsonValue>>* A;
		if (O.IsValid() && O->TryGetArrayField(Field, A))
		{
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				const TSharedPtr<FJsonObject>& E = V->AsObject();
				if (E.IsValid())
				{
					FClip C;
					C.File = E->GetStringField(TEXT("file"));
					C.Len = float(E->GetNumberField(TEXT("len")));
					if (C.Len > 0.2f)
					{
						Out.Add(C);
					}
				}
			}
		}
		return Out;
	};
	auto Groups = [&Clips](const TSharedPtr<FJsonObject>& O, const FString& Field, TMap<FString, TArray<FClip>>& Out)
	{
		const TSharedPtr<FJsonObject>* G;
		if (O.IsValid() && O->TryGetObjectField(Field, G))
		{
			for (const auto& KV : (*G)->Values)
			{
				const FString Key(KV.Key);
				Out.Add(Key, Clips(*G, Key));
			}
		}
	};
	Groups(Root, TEXT("adverts"), Adverts);
	Groups(Root, TEXT("weather"), Weather);
	News = Clips(Root, TEXT("news"));

	// GTA IV's rules (game.dat16): what may follow a song, and how soon things may come back
	const TPair<const TCHAR*, EKind> Cats[] = {{TEXT("advert"), EKind::Advert}, {TEXT("ident"), EKind::Ident},
		{TEXT("music"), EKind::Music}, {TEXT("news"), EKind::News}, {TEXT("weather"), EKind::Weather},
		{TEXT("solo"), EKind::Solo}, {TEXT("to_ad"), EKind::ToAd}, {TEXT("to_news"), EKind::ToNews}};
	AfterMusic[(int32)EKind::Advert] = 1.f; AfterMusic[(int32)EKind::Music] = 2.f; AfterMusic[(int32)EKind::News] = 1.f;
	AfterMusic[(int32)EKind::Weather] = 2.f; AfterMusic[(int32)EKind::Solo] = 1.f;
	FMemory::Memcpy(AfterTwoSongs, AfterMusic, sizeof(AfterMusic));
	AfterTwoSongs[(int32)EKind::Music] = 0.f;
	MinRepeat[(int32)EKind::News] = MinRepeat[(int32)EKind::Weather] = MinRepeat[(int32)EKind::ToNews] = 300.f;
	MinRepeat[(int32)EKind::Solo] = MinRepeat[(int32)EKind::ToAd] = 90.f;
	const TSharedPtr<FJsonObject>* Rules;
	if (Root->TryGetObjectField(TEXT("rules"), Rules))
	{
		auto Read = [&](const TCHAR* Field, float* Into, float Scale)
		{
			const TSharedPtr<FJsonObject>* R;
			if ((*Rules)->TryGetObjectField(Field, R))
			{
				for (const auto& C : Cats)
				{
					double V;
					if ((*R)->TryGetNumberField(C.Key, V))
					{
						Into[(int32)C.Value] = float(V) * Scale;
					}
				}
			}
		};
		Read(TEXT("after_music"), AfterMusic, 1.f);
		Read(TEXT("after_two_songs"), AfterTwoSongs, 1.f);
		Read(TEXT("min_repeat_ms"), MinRepeat, 0.001f);
	}

	RadioData = LoadObject<UGTAIVRadioData>(nullptr, TEXT("/Game/GTAIV/Radio/DA_GTAIV_Radio.DA_GTAIV_Radio"), nullptr,
											LOAD_NoWarn | LOAD_Quiet);
	const UGTAIVRadioData* Data = RadioData;
	const TArray<TSharedPtr<FJsonValue>>* List;
	if (!Root->TryGetArrayField(TEXT("stations"), List))
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& V : *List)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid())
		{
			continue;
		}
		FStation S;
		S.Id = FName(*O->GetStringField(TEXT("id")));
		S.Name = O->GetStringField(TEXT("name"));
		S.bTalk = O->GetBoolField(TEXT("talk"));
		S.AdPool = O->GetStringField(TEXT("adverts"));
		S.Music = Clips(O, TEXT("music"));
		S.Idents = Clips(O, TEXT("idents"));
		S.Solo = Clips(O, TEXT("solo"));
		S.General = Clips(O, TEXT("general"));
		S.ToAd = Clips(O, TEXT("to_ad"));
		S.ToNews = Clips(O, TEXT("to_news"));
		S.ToWeather = Clips(O, TEXT("to_weather"));
		Groups(O, TEXT("time"), S.Time);
		if (Data)
		{
			if (const FGTAIVRadioStationInfo* I = Data->Stations.FindByPredicate([&](const FGTAIVRadioStationInfo& X) { return X.Id == S.Id; }))
			{
				S.Logo = I->Logo;
				S.LogoGrey = I->LogoGrey;
				if (!I->Name.IsEmpty())
				{
					S.Name = I->Name;
				}
			}
		}
		if (S.Music.Num())
		{
			Stations.Add(MoveTemp(S));
		}
	}
}

// ------------------------------------------------------------------ queries

FString UGTAIVRadio::GetStationName(int32 Index) const
{
	return Stations.IsValidIndex(Index) ? Stations[Index].Name : FString(TEXT("Radio Off"));
}

UTexture2D* UGTAIVRadio::GetStationLogo(int32 Index, bool bColour) const
{
	return Stations.IsValidIndex(Index) ? (bColour ? Stations[Index].Logo.Get() : Stations[Index].LogoGrey.Get()) : nullptr;
}

FString UGTAIVRadio::GetDebugState() const
{
	if (!Stations.IsValidIndex(Current))
	{
		return FString::Printf(TEXT("%d stations, %s, radio off"), Stations.Num(), bListening ? TEXT("listening") : TEXT("not listening"));
	}
	const FStation& S = Stations[Current];
	const TCHAR* Kinds[] = {TEXT("music"), TEXT("ident"), TEXT("solo"), TEXT("advert"), TEXT("news"), TEXT("weather"), TEXT("to_ad"), TEXT("to_news"), TEXT("to_weather")};
	if (S.Queue.Num() == 0)
	{
		return S.Name + TEXT(": nothing queued");
	}
	const FSlot& Slot = S.Queue[0];
	return FString::Printf(TEXT("%s: %s %s at %.1f/%.1fs%s, next %s %s | music %s voice %s duck %.2f"), *S.Name, Kinds[(int32)Slot.Kind], *Slot.Clip.File,
		float(Clock() - Slot.Start), Slot.Clip.Len, Slot.Voice.File.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (DJ %s at %.1fs)"), *Slot.Voice.File, Slot.VoiceAt),
		S.Queue.Num() > 1 ? Kinds[(int32)S.Queue[1].Kind] : TEXT("-"), S.Queue.Num() > 1 ? *S.Queue[1].Clip.File : TEXT(""),
		MusicAudio && MusicAudio->IsPlaying() ? TEXT("playing") : TEXT("silent"), VoiceAudio && VoiceAudio->IsPlaying() ? TEXT("playing") : TEXT("silent"), Duck);
}

float UGTAIVRadio::GetTimeSinceTuned() const
{
	return GetWorld() ? float(GetWorld()->GetRealTimeSeconds() - TunedAt) : 100.f;
}

int32 UGTAIVRadio::RandomStation() const
{
	return Stations.Num() ? FMath::RandRange(0, Stations.Num() - 1) : INDEX_NONE;
}

double UGTAIVRadio::Clock() const
{
	return GetWorld() ? GetWorld()->GetRealTimeSeconds() : 0.0;
}

// ------------------------------------------------------------------ the programme

const UGTAIVRadio::FClip* UGTAIVRadio::Pick(const TArray<FClip>& From, TArray<FString>* Recent, int32 Avoid) const
{
	if (From.Num() == 0)
	{
		return nullptr;
	}
	for (int32 Try = 0; Try < 12; ++Try)
	{
		const FClip& C = From[FMath::RandRange(0, From.Num() - 1)];
		if (!Recent || !Recent->Contains(C.File) || Try == 11)
		{
			if (Recent)
			{
				Recent->Add(C.File);
				while (Recent->Num() > Avoid)
				{
					Recent->RemoveAt(0);
				}
			}
			return &C;
		}
	}
	return &From[0];
}

void UGTAIVRadio::Schedule(FStation& S)
{
	double T = S.Queue.Num() ? S.Queue.Last().Start + S.Queue.Last().Clip.Len : Clock();
	auto Add = [&](const FClip* C, EKind K) -> FSlot*
	{
		if (!C)
		{
			return nullptr;
		}
		FSlot& Slot = S.Queue.AddDefaulted_GetRef();
		Slot.Clip = *C;
		Slot.Kind = K;
		Slot.Start = T;
		T += C->Len;
		S.History.Add(K);
		if (S.History.Num() > 8)
		{
			S.History.RemoveAt(0);
		}
		S.LastAired.Add((int32)K, Slot.Start);
		return &Slot;
	};
	auto Song = [&]()
	{
		FSlot* Slot = Add(Pick(S.Music, &S.RecentSongs, S.Music.Num() / 2), EKind::Music);
		// the DJ talks over the intro now and then: the time of day or general chat
		if (Slot && !S.bTalk && Slot->Clip.Len > 60.f && FMath::FRand() < 0.45f)
		{
			const int32 Hour = FDateTime::Now().GetHour();
			const TCHAR* Part = Hour >= 5 && Hour < 12 ? TEXT("morning") : Hour < 17 ? TEXT("afternoon") : Hour < 21 ? TEXT("evening") : TEXT("night");
			const TArray<FClip>* TimeLines = S.Time.Find(Part);
			const FClip* V = TimeLines && TimeLines->Num() && FMath::FRand() < 0.35f ? Pick(*TimeLines) : Pick(S.General);
			if (V && V->Len < Slot->Clip.Len * 0.4f)
			{
				Slot->Voice = *V;
				Slot->VoiceAt = FMath::FRandRange(0.8f, 2.5f);
			}
		}
	};
	auto Since = [&](EKind K)
	{
		const double* At = S.LastAired.Find((int32)K);
		return At ? T - *At : 1e9;
	};

	const EKind Last = S.History.Num() ? S.History.Last() : EKind::Ident;
	if (Last != EKind::Music)
	{
		Song();
		return;
	}
	// after a song: GTA IV's weights for what comes next
	const bool bTwo = S.History.Num() >= 2 && S.History[S.History.Num() - 2] == EKind::Music;
	const float* Weights = bTwo ? AfterTwoSongs : AfterMusic;
	const TArray<FClip>* Ads = Adverts.Find(S.AdPool);
	const TArray<FClip>* Generic = Adverts.Find(TEXT("generic"));
	const bool bHasAds = (Ads && Ads->Num()) || (Generic && Generic->Num());
	float W[(int32)EKind::Count] = {};
	float Total = 0.f;
	for (int32 K = 0; K < (int32)EKind::Count; ++K)
	{
		const EKind Kind = (EKind)K;
		bool bOk = Weights[K] > 0.f && Since(Kind) >= MinRepeat[K];
		bOk &= Kind != EKind::Music || S.Music.Num() > 1;
		bOk &= Kind != EKind::Advert || bHasAds;
		bOk &= Kind != EKind::News || News.Num() > 0;
		bOk &= Kind != EKind::Weather || Weather.Num() > 0;
		bOk &= Kind != EKind::Solo || S.Solo.Num() > 0;
		bOk &= Kind != EKind::Ident;
		W[K] = bOk ? Weights[K] : 0.f;
		Total += W[K];
	}
	EKind Next = EKind::Music;
	if (Total > 0.f)
	{
		float R = FMath::FRand() * Total;
		for (int32 K = 0; K < (int32)EKind::Count; ++K)
		{
			R -= W[K];
			if (W[K] > 0.f && R <= 0.f)
			{
				Next = (EKind)K;
				break;
			}
		}
	}
	auto Ident = [&](float Chance)
	{
		if (S.Idents.Num() && FMath::FRand() < Chance)
		{
			Add(Pick(S.Idents), EKind::Ident);
		}
	};
	switch (Next)
	{
	case EKind::Advert:
	{
		if (S.ToAd.Num() && Since(EKind::ToAd) >= MinRepeat[(int32)EKind::ToAd])
		{
			Add(Pick(S.ToAd), EKind::ToAd);
		}
		const int32 Count = FMath::FRand() < 0.5f ? 2 : 1;
		for (int32 I = 0; I < Count; ++I)
		{
			const bool bOwn = Ads && Ads->Num() && (S.AdPool == TEXT("generic") || FMath::FRand() < 0.6f);
			Add(Pick(bOwn ? *Ads : *Generic), EKind::Advert);
		}
		Ident(0.6f);
		break;
	}
	case EKind::News:
		if (S.ToNews.Num() && Since(EKind::ToNews) >= MinRepeat[(int32)EKind::ToNews])
		{
			Add(Pick(S.ToNews), EKind::ToNews);
		}
		Add(Pick(News), EKind::News);
		Ident(0.5f);
		break;
	case EKind::Weather:
	{
		if (S.ToWeather.Num())
		{
			Add(Pick(S.ToWeather), EKind::ToWeather);
		}
		// no weather system to ask: mostly fair, sometimes not
		const float R = FMath::FRand();
		const TCHAR* Type = R < 0.35f ? TEXT("sun") : R < 0.6f ? TEXT("cloud") : R < 0.8f ? TEXT("wind") : R < 0.92f ? TEXT("rain") : TEXT("fog");
		const TArray<FClip>* Reports = Weather.Find(Type);
		if (!Reports || !Reports->Num())
		{
			for (const auto& KV : Weather)
			{
				Reports = &KV.Value;
				break;
			}
		}
		Add(Reports ? Pick(*Reports) : nullptr, EKind::Weather);
		Ident(0.5f);
		break;
	}
	case EKind::Solo:
		Add(Pick(S.Solo), EKind::Solo);
		break;
	default:
		Song();
		break;
	}
}

void UGTAIVRadio::Advance(FStation& S, double Now)
{
	if (!S.bStarted)
	{
		// the station has been on air for a while: somewhere in a song
		S.bStarted = true;
		S.Queue.Reset();
		S.History.Reset();
		S.History.Add(EKind::Ident);
		Schedule(S);
		if (S.Queue.Num())
		{
			S.Queue[0].Start = Now - FMath::FRandRange(0.05f, 0.8f) * S.Queue[0].Clip.Len;
		}
	}
	for (int32 Guard = 0; Guard < 100000; ++Guard)
	{
		while (S.Queue.Num() < 2)
		{
			const int32 Before = S.Queue.Num();
			Schedule(S);
			if (S.Queue.Num() == Before)
			{
				return;  // nothing to play
			}
		}
		if (Now < S.Queue[0].Start + S.Queue[0].Clip.Len)
		{
			return;
		}
		S.Queue.RemoveAt(0);
	}
}

// ------------------------------------------------------------------ playing

UAudioComponent* UGTAIVRadio::MakeAudio(const TCHAR* Name)
{
	UAudioComponent* A = NewObject<UAudioComponent>(this, Name);
	A->bAutoActivate = false;
	A->bAutoDestroy = false;
	A->bAllowSpatialization = false;
	A->bIsUISound = false;
	A->RegisterComponentWithWorld(GetWorld());
	if (AActor* From = Source.Get())
	{
		Source = nullptr;
		SetPositional(From);  // created while playing from a parked car
	}
	return A;
}

void UGTAIVRadio::Stream(UAudioComponent* Target, FFeed& Into, const FClip& Clip, float Offset)
{
	if (!Target)
	{
		return;
	}
	Into.Stream.Reset();
	Into.Wave.Reset();
	const int32 Serial = ++LoadSerial;
	Target->ComponentTags.Reset();
	Target->ComponentTags.Add(FName(*FString::FromInt(Serial)));
	const FString Path = DataDir / Clip.File;
	const double Requested = Clock();
	const bool bMusic = &Into == &MusicFeed;
	TWeakObjectPtr<UAudioComponent> WeakTarget = Target;
	TWeakObjectPtr<UGTAIVRadio> WeakThis = this;
	Async(EAsyncExecution::ThreadPool, [Path, Offset, Requested, Serial, bMusic, WeakTarget, WeakThis]()
	{
		TArray<uint8> Bytes;
		TSharedPtr<FGTAIVRadioStream> S = MakeShared<FGTAIVRadioStream>();
		bool bOk = FFileHelper::LoadFileToArray(Bytes, *Path) && S->Open(MoveTemp(Bytes));
		if (bOk)
		{
			S->Skip(int64(Offset * S->Rate));
		}
		AsyncTask(ENamedThreads::GameThread, [bOk, S, Requested, Serial, bMusic, WeakTarget, WeakThis]()
		{
			UGTAIVRadio* Radio = WeakThis.Get();
			UAudioComponent* A = WeakTarget.Get();
			if (!bOk || !Radio || !A || !A->ComponentHasTag(FName(*FString::FromInt(Serial))))
			{
				return;  // gone, or something else was tuned in meanwhile
			}
			// the station didn't wait while this loaded
			S->Skip(int64(float(Radio->Clock() - Requested) * S->Rate));
			if (S->Position >= S->Frames)
			{
				return;
			}
			USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(A);
			Wave->SetSampleRate(S->Rate);
			Wave->NumChannels = S->Channels;
			Wave->Duration = INDEFINITELY_LOOPING_DURATION;
			Wave->SoundGroup = SOUNDGROUP_Music;
			Wave->bLooping = false;
			FFeed& F = bMusic ? Radio->MusicFeed : Radio->VoiceFeed;
			F.Stream = S;
			F.Wave = Wave;
			Radio->Feed(F);
			A->SetSound(Wave);
			A->Play();
		});
	});
}

void UGTAIVRadio::Feed(FFeed& F)
{
	USoundWaveProcedural* Wave = F.Wave.Get();
	if (!Wave || !F.Stream.IsValid())
	{
		return;
	}
	FGTAIVRadioStream& S = *F.Stream;
	const int32 Ahead = S.Rate * S.Channels * int32(sizeof(int16)) * 2;  // two seconds queued
	TArray<int16> Chunk;
	while (Wave->GetAvailableAudioByteCount() < Ahead && S.Position < S.Frames)
	{
		Chunk.SetNumUninitialized(S.Rate / 4 * S.Channels);
		const int32 Got = S.Read(Chunk.GetData(), S.Rate / 4);
		if (Got <= 0)
		{
			break;
		}
		Wave->QueueAudio(reinterpret_cast<const uint8*>(Chunk.GetData()), Got * S.Channels * int32(sizeof(int16)));
	}
}

void UGTAIVRadio::Play(const FSlot& Slot, double Now)
{
	if (!MusicAudio)
	{
		MusicAudio = MakeAudio(TEXT("GTAIV_RadioMusic"));
	}
	PlayingFile = Slot.Clip.File;
	PlayingStart = Slot.Start;
	const float Offset = float(Now - Slot.Start);
	bVoicePlayed = Slot.Voice.File.IsEmpty() || Offset > Slot.VoiceAt + 0.5f;
	MusicAudio->Stop();
	Stream(MusicAudio, MusicFeed, Slot.Clip, Offset);
}

void UGTAIVRadio::PlayVoice(const FClip& Clip)
{
	if (!VoiceAudio)
	{
		VoiceAudio = MakeAudio(TEXT("GTAIV_RadioDJ"));
	}
	VoiceAudio->Stop();
	VoiceAudio->SetVolumeMultiplier(Volume);
	Stream(VoiceAudio, VoiceFeed, Clip, 0.f);
}

void UGTAIVRadio::PlayStatic()
{
	if (!StaticAudio)
	{
		StaticAudio = MakeAudio(TEXT("GTAIV_RadioStatic"));
	}
	// the hiss between stations
	const int32 Rate = 32000;
	TArray<int16> Noise;
	Noise.SetNumUninitialized(Rate * 3 / 10);
	float Low = 0.f;
	for (int32 I = 0; I < Noise.Num(); ++I)
	{
		Low = Low * 0.6f + FMath::FRandRange(-1.f, 1.f) * 0.4f;
		const float Env = FMath::Min(1.f, (Noise.Num() - I) / 2000.f);
		Noise[I] = int16(Low * 5000.f * Env);
	}
	USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(StaticAudio);
	Wave->SetSampleRate(Rate);
	Wave->NumChannels = 1;
	Wave->Duration = INDEFINITELY_LOOPING_DURATION;
	Wave->bLooping = false;
	Wave->QueueAudio(reinterpret_cast<const uint8*>(Noise.GetData()), Noise.Num() * sizeof(int16));
	StaticAudio->SetSound(Wave);
	StaticAudio->SetVolumeMultiplier(Volume);
	StaticAudio->Play();
}

void UGTAIVRadio::StopAll()
{
	for (UAudioComponent* A : {MusicAudio.Get(), VoiceAudio.Get()})
	{
		if (A)
		{
			A->Stop();
			A->ComponentTags.Reset();
		}
	}
	PlayingFile.Reset();
	MusicFeed = FFeed();
	VoiceFeed = FFeed();
}

void UGTAIVRadio::SetListening(bool bOn, int32 Station)
{
	if (!bOn)
	{
		bListening = false;
		SetPositional(nullptr);
		StopAll();
		return;
	}
	const bool bSame = bListening && Source.IsValid() && Station == Current;
	SetPositional(nullptr);   // in a car: the radio is in your ears again
	if (bSame)
	{
		return;  // back in the car it was playing from: it just carries on
	}
	bListening = true;
	SetStation(Station);
}

void UGTAIVRadio::LeaveInVehicle(AActor* Vehicle)
{
	if (!bListening || !Stations.IsValidIndex(Current) || !Vehicle)
	{
		SetListening(false, INDEX_NONE);
		return;
	}
	SetPositional(Vehicle);
}

void UGTAIVRadio::SetPositional(AActor* From)
{
	Source = From;
	FSoundAttenuationSettings Att;
	Att.bAttenuate = From != nullptr;
	Att.bSpatialize = From != nullptr;
	Att.AttenuationShape = EAttenuationShape::Sphere;
	Att.AttenuationShapeExtents = FVector(150.f, 0.f, 0.f);   // full volume next to the car
	Att.FalloffDistance = FMath::Max(500.f, HearingDistance - 150.f);
	Att.bAttenuateWithLPF = true;                                 // muffled from outside, more so far away
	Att.LPFRadiusMin = 150.f;
	Att.LPFRadiusMax = HearingDistance;
	Att.LPFFrequencyAtMin = 9000.f;
	Att.LPFFrequencyAtMax = 1800.f;
	for (UAudioComponent* A : {MusicAudio.Get(), VoiceAudio.Get(), StaticAudio.Get()})
	{
		if (!A)
		{
			continue;
		}
		if (From)
		{
			A->AttachToComponent(From->GetRootComponent(), FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		}
		else
		{
			A->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		}
		A->bAllowSpatialization = From != nullptr;
		A->bOverrideAttenuation = true;
		A->AttenuationOverrides = Att;
		A->AdjustAttenuation(Att);
	}
}

void UGTAIVRadio::SetStation(int32 Index)
{
	Current = Stations.IsValidIndex(Index) ? Index : INDEX_NONE;
	TunedAt = GetWorld() ? GetWorld()->GetRealTimeSeconds() : 0.0;
	StopAll();
	if (bListening && Stations.Num())
	{
		PlayStatic();
	}
}

void UGTAIVRadio::Retune(int32 Step)
{
	if (Stations.Num() == 0)
	{
		return;
	}
	const int32 N = Stations.Num() + 1;  // + Radio Off
	const int32 From = Current == INDEX_NONE ? Stations.Num() : Current;
	const int32 To = ((From + Step) % N + N) % N;
	SetStation(To == Stations.Num() ? INDEX_NONE : To);
}

void UGTAIVRadio::Tick(float DeltaTime)
{
	Feed(MusicFeed);
	Feed(VoiceFeed);
	if (!bListening || !Stations.IsValidIndex(Current))
	{
		return;
	}
	// a parked car's radio: off once the car is gone or the player has walked away
	if (Source.IsValid() || Source.IsStale())
	{
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		const APawn* Me = PC ? PC->GetPawn() : nullptr;
		AActor* From = Source.Get();
		if (!From || (Me && FVector::Dist(Me->GetActorLocation(), From->GetActorLocation()) > HearingDistance + 500.f))
		{
			SetListening(false, INDEX_NONE);
			return;
		}
	}
	FStation& S = Stations[Current];
	const double Now = Clock();
	Advance(S, Now);
	if (S.Queue.Num() == 0)
	{
		return;
	}
	const FSlot& Slot = S.Queue[0];
	// a short pause after retuning (the hiss), then whatever the station is playing now
	if ((Slot.Clip.File != PlayingFile || Slot.Start != PlayingStart) && GetTimeSinceTuned() > 0.25f)
	{
		Play(Slot, Now);
	}
	const bool bOnAir = Slot.Clip.File == PlayingFile && Slot.Start == PlayingStart;  // (not during the hiss)
	if (bOnAir && !bVoicePlayed && Now - Slot.Start >= Slot.VoiceAt && !Slot.Voice.File.IsEmpty())
	{
		bVoicePlayed = true;
		PlayVoice(Slot.Voice);
	}
	// the music dips under the DJ
	const bool bTalking = bOnAir && VoiceAudio && VoiceAudio->IsPlaying() && VoiceFeed.Stream.IsValid() &&
		VoiceFeed.Stream->Position < VoiceFeed.Stream->Frames;
	Duck = FMath::FInterpTo(Duck, bTalking ? 0.45f : 1.f, DeltaTime, 4.f);
	if (MusicAudio)
	{
		MusicAudio->SetVolumeMultiplier(Volume * Duck);
	}
}
