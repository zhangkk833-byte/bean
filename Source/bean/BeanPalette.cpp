#include "BeanPalette.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	struct FDefaultPaletteEntry
	{
		const TCHAR* Name;
		const TCHAR* Hex;
	};

	// 48 curated colors: hue groups first (rainbow order for wheel cycling), then neutrals.
	const FDefaultPaletteEntry DefaultPalette[] = {
		{TEXT("深红"), TEXT("#8B0000")},
		{TEXT("红"), TEXT("#FF0000")},
		{TEXT("亮红"), TEXT("#FF4444")},
		{TEXT("玫红"), TEXT("#FF1493")},
		{TEXT("粉红"), TEXT("#FF99BB")},
		{TEXT("珊瑚"), TEXT("#FF7F50")},
		{TEXT("深橙"), TEXT("#CC5500")},
		{TEXT("橙"), TEXT("#FF7F00")},
		{TEXT("亮橙"), TEXT("#FFA64D")},
		{TEXT("橙黄"), TEXT("#FFB300")},
		{TEXT("深黄"), TEXT("#CCAA00")},
		{TEXT("黄"), TEXT("#FFD400")},
		{TEXT("亮黄"), TEXT("#FFF176")},
		{TEXT("黄绿"), TEXT("#9ACD32")},
		{TEXT("深绿"), TEXT("#1E7A1E")},
		{TEXT("绿"), TEXT("#00A000")},
		{TEXT("亮绿"), TEXT("#66CC66")},
		{TEXT("薄荷绿"), TEXT("#98FB98")},
		{TEXT("青绿"), TEXT("#008080")},
		{TEXT("深青"), TEXT("#005F5F")},
		{TEXT("青"), TEXT("#00C8C8")},
		{TEXT("亮青"), TEXT("#99FFFF")},
		{TEXT("湖蓝"), TEXT("#00BFFF")},
		{TEXT("天蓝"), TEXT("#87CEFA")},
		{TEXT("蓝"), TEXT("#1E90FF")},
		{TEXT("亮蓝"), TEXT("#66B2FF")},
		{TEXT("深蓝"), TEXT("#1F3A93")},
		{TEXT("藏蓝"), TEXT("#0B1D4F")},
		{TEXT("蓝紫"), TEXT("#7B68EE")},
		{TEXT("深紫"), TEXT("#4B0082")},
		{TEXT("紫"), TEXT("#8A2BE2")},
		{TEXT("亮紫"), TEXT("#B266FF")},
		{TEXT("紫罗兰"), TEXT("#EE82EE")},
		{TEXT("品红"), TEXT("#FF00FF")},
		{TEXT("洋红"), TEXT("#C71585")},
		{TEXT("深棕"), TEXT("#4A2A0A")},
		{TEXT("棕"), TEXT("#8B5A2B")},
		{TEXT("浅棕"), TEXT("#C19A6B")},
		{TEXT("米黄"), TEXT("#EEDC82")},
		{TEXT("金"), TEXT("#FFD700")},
		{TEXT("白"), TEXT("#FFFFFF")},
		{TEXT("亮灰"), TEXT("#D0D0D0")},
		{TEXT("银灰"), TEXT("#C0C0C0")},
		{TEXT("灰"), TEXT("#808080")},
		{TEXT("深灰"), TEXT("#404040")},
		{TEXT("碳灰"), TEXT("#303030")},
		{TEXT("墨色"), TEXT("#1A1A1A")},
		{TEXT("黑"), TEXT("#000000")},
	};

	constexpr int32 NumDefaultColors = sizeof(DefaultPalette) / sizeof(DefaultPalette[0]);

	TArray<FBeanPaletteColor> BuildDefaults()
	{
		TArray<FBeanPaletteColor> Result;
		Result.Reserve(NumDefaultColors);
		for (const FDefaultPaletteEntry& Entry : DefaultPalette)
		{
			FBeanPaletteColor Item;
			Item.Name = Entry.Name;
			const FColor Srgb = FColor::FromHex(Entry.Hex);
			Item.Color = FLinearColor::FromSRGBColor(Srgb);
			Result.Add(Item);
		}
		return Result;
	}

	FString ColorToHex(const FLinearColor& Color)
	{
		const FColor Srgb = Color.ToFColor(true);
		return FString::Printf(TEXT("#%02X%02X%02X"), Srgb.R, Srgb.G, Srgb.B);
	}

	// -- CIE L*a*b* (D65) helpers ---------------------------------------------
	// sRGB is not perceptually uniform: two colors the eye considers "close" can be
	// far apart in raw RGB numbers (and vice versa). Matching in Lab instead keeps
	// nearest-palette picks in line with what a human sees (Q1).

	/** sRGB-encoded [0,1] -> linear [0,1]. */
	inline float SRGBEncodedToLinearUnit(float C)
	{
		return (C <= 0.04045f) ? (C / 12.92f)
			: static_cast<float>(FMath::Pow((C + 0.055f) / 1.055f, 2.4));
	}

	/** linear [0,1] -> sRGB-encoded [0,1] (inverse of SRGBEncodedToLinearUnit). */
	inline float LinearToSRGBEncodedUnit(float C)
	{
		return (C <= 0.0031308f) ? (C * 12.92f)
			: static_cast<float>(1.055 * FMath::Pow(C, 1.0 / 2.4) - 0.055);
	}

	/** Standard CIE f(t) used by the Lab formula (cube root with a linear toe near 0). */
	inline float LabF(float T)
	{
		constexpr float Epsilon = 216.0f / 24389.0f;  // (6/29)^3
		constexpr float Kappa = 24389.0f / 27.0f;     // 903.3
		return (T > Epsilon) ? static_cast<float>(FMath::Pow(T, 1.0 / 3.0))
			: ((Kappa * T + 16.0f) / 116.0f);
	}

	/** CIE L*a*b* (D65) from sRGB-encoded channels in [0,1]. Returns (L, a, b). */
	FVector3f LabFromSRGB(float R, float G, float B)
	{
		R = SRGBEncodedToLinearUnit(R);
		G = SRGBEncodedToLinearUnit(G);
		B = SRGBEncodedToLinearUnit(B);

		// sRGB (D65) -> XYZ.
		const float X = 0.4124564f * R + 0.3575761f * G + 0.1804375f * B;
		const float Y = 0.2126729f * R + 0.7151522f * G + 0.0721750f * B;
		const float Z = 0.0193339f * R + 0.1191920f * G + 0.9503041f * B;

		constexpr float RefX = 0.95047f;
		constexpr float RefY = 1.0f;
		constexpr float RefZ = 1.08883f;

		const float Fx = LabF(X / RefX);
		const float Fy = LabF(Y / RefY);
		const float Fz = LabF(Z / RefZ);

		FVector3f Lab;
		Lab.X = 116.0f * Fy - 16.0f;      // L*
		Lab.Y = 500.0f * (Fx - Fy);       // a*
		Lab.Z = 200.0f * (Fy - Fz);       // b*
		return Lab;
	}

	/** CIE L*a*b* from a linear-space color (re-encoded to sRGB first). */
	FVector3f LabFromLinear(const FLinearColor& Color)
	{
		return LabFromSRGB(
			LinearToSRGBEncodedUnit(FMath::Clamp(Color.R, 0.0f, 1.0f)),
			LinearToSRGBEncodedUnit(FMath::Clamp(Color.G, 0.0f, 1.0f)),
			LinearToSRGBEncodedUnit(FMath::Clamp(Color.B, 0.0f, 1.0f)));
	}

	/** Index of the closest palette point to Color by squared Lab distance. */
	int32 NearestLabPaletteIndex(const TArray<FVector3f>& LabPalette, const FVector3f& Color)
	{
		if (LabPalette.Num() == 0) return 0;

		int32 BestIndex = 0;
		float BestSq = TNumericLimits<float>::Max();
		for (int32 i = 0; i < LabPalette.Num(); ++i)
		{
			const FVector3f& C = LabPalette[i];
			const float DL = C.X - Color.X;
			const float DA = C.Y - Color.Y;
			const float DB = C.Z - Color.Z;
			const float Sq = DL * DL + DA * DA + DB * DB;
			if (Sq < BestSq)
			{
				BestSq = Sq;
				BestIndex = i;
			}
		}
		return BestIndex;
	}

	/** CIE chroma (sqrt(a^2 + b^2)). ~0 for neutrals (greys/white/black), high for vivid colors. */
	inline float LabChroma(const FVector3f& L)
	{
		return FMath::Sqrt(L.Y * L.Y + L.Z * L.Z);
	}

	/**
	 * Weighted mode of each sample's nearest palette color, with an optional "colour-edge
	 * neutral suppression" pass.
	 *
	 * Palette colors whose chroma is below NeutralChroma are near-neutral (greys/white/black).
	 * If a block contains strongly saturated samples (MaxChroma above ColorfulChroma) but the
	 * mode lands on a neutral color, the anti-aliased "grey in-between" pixels won the vote even
	 * though the block is really a coloured edge. In that case we fall back to the highest-weight
	 * non-neutral palette color. Blocks that genuinely are grey/white (low MaxChroma) are untouched.
	 */
	// "Muddy" catch-all: palette colors below this chroma are treated as ambiguous transition
	// colors (greys AND the brownish 50/50 blend of two neighbouring colors). On a colourful
	// block whose top pick is muddy, we snap to the strongest genuinely-vivid palette color
	// present (see the suppression below). This is what removes the dirty brown/grey halo along
	// colour edges without touching genuinely muddy regions (no vivid alternative -> unchanged).
	constexpr float NeutralChroma       = 46.0f;
	constexpr float ColorfulChroma      = 40.0f;  // block is "colourful" above this max sample chroma
	constexpr float NeutralFallbackMin  = 0.25f;  // fallback color must hold > this fraction of the mode

	int32 WeightedModePaletteIndex(const TArray<FVector3f>& Palette, const TArray<float>& PaletteChroma,
		const TArray<FVector3f>& Labs, const TArray<float>& Weights,
		bool bSuppressNeutral, TArray<float>& ScratchWins)
	{
		const int32 NumPalette = Palette.Num();
		if (Labs.Num() == 0)
		{
			return 0;
		}

		FMemory::Memzero(ScratchWins.GetData(), ScratchWins.Num() * sizeof(float));
		float MaxChroma = 0.0f;
		for (int32 i = 0; i < Labs.Num(); ++i)
		{
			MaxChroma = FMath::Max(MaxChroma, LabChroma(Labs[i]));
			ScratchWins[NearestLabPaletteIndex(Palette, Labs[i])] += Weights[i];
		}

		int32 BestIndex = 0;
		float BestW = ScratchWins[0];
		for (int32 i = 1; i < NumPalette; ++i)
		{
			if (ScratchWins[i] > BestW)
			{
				BestW = ScratchWins[i];
				BestIndex = i;
			}
		}

		if (bSuppressNeutral && MaxChroma >= ColorfulChroma && BestW > 0.0f
			&& PaletteChroma[BestIndex] < NeutralChroma)
		{
			// Coloured block, but the top pick was a neutral grey: that grey is likely the
			// anti-aliased in-between band. Prefer the strongest genuinely-coloured palette color.
			int32 Fallback = -1;
			float FallbackW = -1.0f;
			for (int32 i = 0; i < NumPalette; ++i)
			{
				if (ScratchWins[i] > 0.0f && PaletteChroma[i] >= NeutralChroma && ScratchWins[i] > FallbackW)
				{
					FallbackW = ScratchWins[i];
					Fallback = i;
				}
			}
			if (Fallback >= 0 && FallbackW > BestW * NeutralFallbackMin)
			{
				return Fallback;
			}
		}

		return BestIndex;
	}

	/**
	 * Block-level refinement: cluster the block's sampled Lab colors into K=2 groups, take the
	 * larger-weight cluster, then pick its (weighted) mode — the palette color the members are
	 * actually closest to — rather than snapping a synthetic centroid. This avoids inventing a
	 * blended "grey in-between" colour at green/brown edges (Q2), and combined with the neutral
	 * suppression above keeps the block on one of its two real colors.
	 */
	int32 KMeansLargestPaletteIndex(const TArray<FVector3f>& Palette, const TArray<float>& PaletteChroma,
		const TArray<FVector3f>& Labs, const TArray<float>& Weights, int32 Iters,
		bool bSuppressNeutral, TArray<FVector3f>& SubLabs, TArray<float>& SubWeights,
		TArray<float>& ScratchWins)
	{
		const int32 Num = Labs.Num();
		if (Num == 0)
		{
			return 0;
		}
		if (Num == 1)
		{
			return NearestLabPaletteIndex(Palette, Labs[0]);
		}

		// Initialise the two centroids with two of the most distant samples.
		FVector3f C0 = Labs[0];
		FVector3f C1 = Labs[0];
		{
			float FarSq = -1.0f;
			int32 FarId = 0;
			for (int32 i = 0; i < Num; ++i)
			{
				const FVector3f& L = Labs[i];
				const float DL = C0.X - L.X;
				const float DA = C0.Y - L.Y;
				const float DB = C0.Z - L.Z;
				const float Sq = DL * DL + DA * DA + DB * DB;
				if (Sq > FarSq) { FarSq = Sq; FarId = i; }
			}
			C1 = Labs[FarId];
		}

		for (int32 Iter = 0; Iter < Iters; ++Iter)
		{
			FVector3f S0(0.0f, 0.0f, 0.0f), S1(0.0f, 0.0f, 0.0f);
			int32 N0 = 0, N1 = 0;
			for (int32 i = 0; i < Num; ++i)
			{
				const FVector3f& L = Labs[i];
				const float D0 = (C0.X - L.X) * (C0.X - L.X) + (C0.Y - L.Y) * (C0.Y - L.Y) + (C0.Z - L.Z) * (C0.Z - L.Z);
				const float D1 = (C1.X - L.X) * (C1.X - L.X) + (C1.Y - L.Y) * (C1.Y - L.Y) + (C1.Z - L.Z) * (C1.Z - L.Z);
				if (D1 < D0) { S1 += L; ++N1; } else { S0 += L; ++N0; }
			}
			if (N0 == 0 || N1 == 0)
			{
				break; // degenerate (all points collapsed onto one cluster)
			}
			const FVector3f Old0 = C0, Old1 = C1;
			C0 = S0 / (float)N0;
			C1 = S1 / (float)N1;
			if ((C0 - Old0).SizeSquared() < 1e-6f && (C1 - Old1).SizeSquared() < 1e-6f)
			{
				break;
			}
		}

		// Final assignment: total weight per cluster, keep the larger.
		double W0 = 0.0, W1 = 0.0;
		for (int32 i = 0; i < Num; ++i)
		{
			const FVector3f& L = Labs[i];
			const float D0 = (C0.X - L.X) * (C0.X - L.X) + (C0.Y - L.Y) * (C0.Y - L.Y) + (C0.Z - L.Z) * (C0.Z - L.Z);
			const float D1 = (C1.X - L.X) * (C1.X - L.X) + (C1.Y - L.Y) * (C1.Y - L.Y) + (C1.Z - L.Z) * (C1.Z - L.Z);
			if (D1 < D0) { W1 += Weights[i]; } else { W0 += Weights[i]; }
		}
		const bool bCluster1 = (W1 > W0);

		// Collect the winning cluster's members and let the weighted-mode + suppression logic
		// pick a real palette color from them.
		SubLabs.Reset();
		SubWeights.Reset();
		for (int32 i = 0; i < Num; ++i)
		{
			const FVector3f& L = Labs[i];
			const float D0 = (C0.X - L.X) * (C0.X - L.X) + (C0.Y - L.Y) * (C0.Y - L.Y) + (C0.Z - L.Z) * (C0.Z - L.Z);
			const float D1 = (C1.X - L.X) * (C1.X - L.X) + (C1.Y - L.Y) * (C1.Y - L.Y) + (C1.Z - L.Z) * (C1.Z - L.Z);
			if ((D1 < D0) == bCluster1)
			{
				SubLabs.Add(L);
				SubWeights.Add(Weights[i]);
			}
		}

		return WeightedModePaletteIndex(Palette, PaletteChroma, SubLabs, SubWeights, bSuppressNeutral, ScratchWins);
	}
}

FString FBeanPalette::GetConfigPath()
{
	return FPaths::ProjectConfigDir() / TEXT("BeanPalette.json");
}

TArray<FBeanPaletteColor> FBeanPalette::LoadOrCreateDefault()
{
	const TArray<FBeanPaletteColor> Defaults = BuildDefaults();
	const FString Path = GetConfigPath();

	TArray<FBeanPaletteColor> Result;
	FString Json;
	if (FFileHelper::LoadFileToString(Json, *Path))
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Colors = nullptr;
			if (Root->TryGetArrayField(TEXT("colors"), Colors))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Colors)
				{
					const TSharedPtr<FJsonObject> Obj = Value->AsObject();
					if (!Obj.IsValid()) continue;
					FString Name;
					FString Hex;
					if (!Obj->TryGetStringField(TEXT("name"), Name)) continue;
					if (!Obj->TryGetStringField(TEXT("hex"), Hex)) continue;
					FBeanPaletteColor Item;
					Item.Name = Name;
					const FColor Srgb = FColor::FromHex(Hex);
					Item.Color = FLinearColor::FromSRGBColor(Srgb);
					Result.Add(Item);
				}
			}
		}
	}

	if (Result.Num() == NumDefaultColors)
	{
		return Result;
	}

	// Missing or invalid config: write the default palette file.
	TSharedPtr<FJsonObject> Root = MakeShareable(new FJsonObject());
	TArray<TSharedPtr<FJsonValue>> ColorArray;
	for (const FBeanPaletteColor& Item : Defaults)
	{
		TSharedPtr<FJsonObject> Obj = MakeShareable(new FJsonObject());
		Obj->SetStringField(TEXT("name"), Item.Name);
		Obj->SetStringField(TEXT("hex"), ColorToHex(Item.Color));
		ColorArray.Add(MakeShareable(new FJsonValueObject(Obj)));
	}
	Root->SetArrayField(TEXT("colors"), ColorArray);

	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	if (FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		FFileHelper::SaveStringToFile(OutJson, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	return Defaults;
}

int32 FBeanPalette::NearestColorIndex(const TArray<FBeanPaletteColor>& Palette, const FLinearColor& Color)
{
	if (Palette.Num() == 0) return 0;

	TArray<FVector3f> LabPalette;
	BuildLabPalette(Palette, LabPalette);
	return NearestLabPaletteIndex(LabPalette, LabFromLinear(Color));
}

void FBeanPalette::BuildLabPalette(const TArray<FBeanPaletteColor>& Palette, TArray<FVector3f>& OutLab)
{
	OutLab.Reset();
	OutLab.Reserve(Palette.Num());
	for (const FBeanPaletteColor& Item : Palette)
	{
		// The cube material renders Item.Color (linear); ToFColorSRGB() gives back the
		// sRGB 8-bit value it displays, so the Lab anchor matches what the player sees.
		const FColor Srgb = Item.Color.ToFColorSRGB();
		OutLab.Add(LabFromSRGB(Srgb.R / 255.0f, Srgb.G / 255.0f, Srgb.B / 255.0f));
	}
}

int32 FBeanPalette::NearestColorIndexLab(const TArray<FVector3f>& LabPalette, const FColor& SrgbColor)
{
	return NearestLabPaletteIndex(LabPalette, LabFromSRGB(SrgbColor.R / 255.0f, SrgbColor.G / 255.0f, SrgbColor.B / 255.0f));
}

void FBeanPalette::QuantizeImage(const TArray<FVector3f>& LabPalette, const FColor* Pixels,
	int32 Width, int32 Height, int32 Granularity, uint8 EmptyValue, TArray<uint8>& OutCells)
{
	const int32 N = Granularity;
	const int32 Count = N * N;

	if (N <= 0 || !Pixels || Width <= 0 || Height <= 0 || LabPalette.Num() == 0)
	{
		OutCells.Init(EmptyValue, FMath::Max(0, Count));
		return;
	}

	OutCells.SetNum(Count);
	const int32 NumPalette = LabPalette.Num();

	// ---- Per-step knobs. Tune these to A/B which combination fits your art ---------
	// Step 2: K-Means (K=2) largest-cluster instead of a plain vote. Most robust; cheap.
	constexpr bool   bUseKMeans         = true;
	// Anti-alias fix: if a block has strongly saturated content but the vote lands on a grey,
	// the grey is a transition "in-between" band -> prefer the strongest real colour. This is
	// what removes the grey outline at green/brown edges, without harming genuinely grey blocks.
	constexpr bool   bSuppressNeutral   = true;
	// Optional post-pass: replace isolated muddy/grey blocks that sit inside a colourful region.
	constexpr bool   bDespeckle         = true;
	// Step 4 (spatial): weight samples toward the block centre (reduces neighbour bleed).
	constexpr bool   bGaussianWeight    = true;
	// Step 4 (optional): down-weight desaturated (transition) pixels. NOTE: suppression above
	// already handles the grey-edge case; keep this off so neutral greys are not biased against.
	constexpr bool   bSaturationWeight  = false;
	constexpr float  SaturationLowStrength = 0.7f;
	constexpr float  GaussianSigma      = 0.35f;   // [0,1] in block-half-normalised units
	// Step 1: local edge sharpening (unsharp) so anti-aliased edge pixels get pushed to one
	// side instead of staying "in between". 0 disables (avoids overshoot/halo risk).
	constexpr float  SharpeningAmount   = 0.5f;
	constexpr int32  SharpeningRadius   = 2;
	// Step 5 (transparency): for stickers with a soft/anti-aliased alpha edge, the fuzzy
	// semi-transparent rim is what leaves a dirty ring of blocks. Down-weight those pixels so
	// the solid Colour wins, and require every block to actually contain a solid pixel.
	constexpr bool   bOpaqueWeight      = true;
	constexpr uint8  OpaqueSolidAlpha   = 200;   // pixels at/above this alpha count as "solid"
	constexpr float  OpaqueLowWeight    = 0.12f; // mostly transparent AA pixels
	constexpr float  OpaqueMidWeight    = 0.35f; // moderately transparent AA pixels
	constexpr bool   bRequireSolid      = true;  // a block must contain a solid pixel to be placed
	constexpr int32  MinKMeansSamples   = 4;
	constexpr int32  KMeansIters        = 8;
	// Cap the stratified subsampling so huge images stay fast; ~8x8 per block.
	constexpr int64  MaxSamplesPerBlock = 64;

	// Per-palette-chroma lookup (used by the neutral suppression).
	TArray<float> PaletteChroma;
	PaletteChroma.SetNum(NumPalette);
	for (int32 i = 0; i < NumPalette; ++i)
	{
		PaletteChroma[i] = LabChroma(LabPalette[i]);
	}

	// Reusable per-block sample buffers.
	TArray<FVector3f> Labs, SubLabs;
	TArray<float>     Weights, SubWeights, Wins;
	Labs.Reserve(160);
	SubLabs.Reserve(160);
	Weights.Reserve(160);
	SubWeights.Reserve(160);
	Wins.SetNum(NumPalette); // cumulative weight per palette color

	for (int32 By = 0; By < N; ++By)
	{
		// Source rectangle owned by this block row (supports both downscale and upscale,
		// so a source smaller than the grid still yields a color for every block).
		const int64 Ys = FMath::Clamp<int64>((int64)FMath::FloorToDouble((double)By * Height / N), 0, Height);
		const int64 Ye = FMath::Clamp<int64>((int64)FMath::CeilToDouble((double)(By + 1) * Height / N), 0, Height);
		const int64 Ah = Ye - Ys;
		if (Ah <= 0) continue;
		const double CY = (Ys + Ye - 1) * 0.5;
		const double HalfY = FMath::Max(1.0, Ah * 0.5);

		for (int32 Bx = 0; Bx < N; ++Bx)
		{
			const int64 Xs = FMath::Clamp<int64>((int64)FMath::FloorToDouble((double)Bx * Width / N), 0, Width);
			const int64 Xe = FMath::Clamp<int64>((int64)FMath::CeilToDouble((double)(Bx + 1) * Width / N), 0, Width);
			const int64 Aw = Xe - Xs;

			Labs.Reset();
			Weights.Reset();

			if (Aw <= 0)
			{
				OutCells[By * N + Bx] = EmptyValue;
				continue;
			}
			const double CX = (Xs + Xe - 1) * 0.5;
			const double HalfX = FMath::Max(1.0, Aw * 0.5);
			bool bAnySolid = false;

			// Sample one source pixel: sharpen, convert to Lab, compute its weight.
			auto SamplePixel = [&](int64 X, int64 Y)
			{
				const FColor& Px = Pixels[Y * (int64)Width + X];
				if (Px.A < 128) return; // transparent: ignored

				float R = Px.R, G = Px.G, B = Px.B;

				if (SharpeningAmount > 0.0f)
				{
					// Local mean of a small neighbourhood, then unsharp: p' = p + k*(p - mean).
					// Transition pixels land closer to one side, so fewer of them are ambiguous.
					int64 SR = 0, SG = 0, SB = 0;
					int32 Nn = 0;
					for (int32 dy = -SharpeningRadius; dy <= SharpeningRadius; ++dy)
					{
						const int64 yy = FMath::Clamp<int64>(Y + dy, 0, Height - 1);
						const FColor* Row = Pixels + yy * (int64)Width;
						for (int32 dx = -SharpeningRadius; dx <= SharpeningRadius; ++dx)
						{
							const int64 xx = FMath::Clamp<int64>(X + dx, 0, Width - 1);
							const FColor& Np = Row[xx];
							if (Np.A < 128) continue;
							SR += Np.R; SG += Np.G; SB += Np.B; ++Nn;
						}
					}
					if (Nn > 0)
					{
						const float Inv = 1.0f / Nn;
						R = FMath::Clamp(R + SharpeningAmount * (R - SR * Inv), 0.0f, 255.0f);
						G = FMath::Clamp(G + SharpeningAmount * (G - SG * Inv), 0.0f, 255.0f);
						B = FMath::Clamp(B + SharpeningAmount * (B - SB * Inv), 0.0f, 255.0f);
					}
				}

				// Step 3: still match in CIE Lab so colour fidelity stays high.
				const FVector3f Lab = LabFromSRGB(R / 255.0f, G / 255.0f, B / 255.0f);

				float W = 1.0f;
				if (bGaussianWeight)
				{
					const float FX = (float)((X - CX) / HalfX);   // ~[-1,1]
					const float FY = (float)((Y - CY) / HalfY);
					W = (float)FMath::Exp(-(FX * FX + FY * FY) / (2.0f * GaussianSigma * GaussianSigma));
				}
				if (bSaturationWeight)
				{
					const float Chroma = FMath::Sqrt(Lab.Y * Lab.Y + Lab.Z * Lab.Z);
					const float Sat = FMath::Clamp(Chroma / 128.0f, 0.0f, 1.0f);
					W *= (1.0f - SaturationLowStrength * (1.0f - Sat));
				}
				if (bOpaqueWeight)
				{
					if (Px.A >= OpaqueSolidAlpha)
					{
						bAnySolid = true;
					}
					else
					{
						// Fuzzy anti-aliased rim: barely contributes, so the solid colour wins.
						W *= (Px.A >= 160) ? OpaqueMidWeight : OpaqueLowWeight;
					}
				}

				Labs.Add(Lab);
				Weights.Add(W);
			};

			if (Aw * Ah <= MaxSamplesPerBlock)
			{
				// Small block: sample every source pixel inside it.
				for (int64 Y = Ys; Y < Ye; ++Y)
					for (int64 X = Xs; X < Xe; ++X)
						SamplePixel(X, Y);
			}
			else
			{
				// Large block: stratified subsample preserving the block's aspect ratio.
				const int32 Cols = FMath::Clamp<int32>(
					(int32)FMath::CeilToDouble(FMath::Sqrt(MaxSamplesPerBlock * (double)Aw / (double)Ah)), 1, (int32)Aw);
				const int32 Rows = FMath::Clamp<int32>(
					(int32)FMath::CeilToDouble(FMath::Sqrt(MaxSamplesPerBlock * (double)Ah / (double)Aw)), 1, (int32)Ah);
				for (int32 R = 0; R < Rows; ++R)
				{
					const int64 Y = Ys + (int64)(((double)R + 0.5) * Ah / Rows);
					for (int32 C = 0; C < Cols; ++C)
					{
						const int64 X = Xs + (int64)(((double)C + 0.5) * Aw / Cols);
						SamplePixel(X, Y);
					}
				}
			}

			const int32 SampleCount = Labs.Num();
			if (SampleCount == 0)
			{
				OutCells[By * N + Bx] = EmptyValue;
				continue;
			}
			if (bRequireSolid && !bAnySolid)
			{
				// Only fuzzy anti-aliased rim here (no solid pixel): drop it so a dirty ring
				// of blocks does not appear around the sticker's transparent silhouette.
				OutCells[By * N + Bx] = EmptyValue;
				continue;
			}

			if (bUseKMeans && SampleCount >= MinKMeansSamples)
			{
				// Step 2: largest K=2 cluster, then its weighted real-color mode (not a centroid).
				OutCells[By * N + Bx] = static_cast<uint8>(KMeansLargestPaletteIndex(
					LabPalette, PaletteChroma, Labs, Weights, KMeansIters,
					bSuppressNeutral, SubLabs, SubWeights, Wins));
			}
			else
			{
				// Fallback / base: weighted mode of each sample's nearest palette colour.
				OutCells[By * N + Bx] = static_cast<uint8>(WeightedModePaletteIndex(
					LabPalette, PaletteChroma, Labs, Weights, bSuppressNeutral, Wins));
			}
		}
	}

	// Optional despeckle: an isolated neutral block entirely surrounded by colourful neighbours
	// is usually a leftover grey transition band -- replace it with the dominant neighbour colour.
	if (bDespeckle && N > 2)
	{
		TArray<uint8> Original = OutCells;
		for (int32 Y = 0; Y < N; ++Y)
		{
			for (int32 X = 0; X < N; ++X)
			{
				const int32 Idx = Y * N + X;
				const uint8 Cell = Original[Idx];
				if (Cell == EmptyValue || PaletteChroma[Cell] >= NeutralChroma) continue; // not neutral

				int32 ColourCounts[256] = {}; // palette index (<= 48) -> neighbour count
				int32 ColourfulNeighbours = 0;
				for (int32 dy = -1; dy <= 1; ++dy)
				{
					const int32 Ny = Y + dy;
					if (Ny < 0 || Ny >= N) continue;
					for (int32 dx = -1; dx <= 1; ++dx)
					{
						if (dx == 0 && dy == 0) continue;
						const int32 Nx = X + dx;
						if (Nx < 0 || Nx >= N) continue;
						const uint8 Nc = Original[Ny * N + Nx];
						if (Nc == EmptyValue) continue;
						++ColourCounts[Nc];
						if (PaletteChroma[Nc] >= NeutralChroma) ++ColourfulNeighbours;
					}
				}
				if (ColourfulNeighbours >= 5) // surrounded by colour -> likely a transition halo
				{
					int32 Best = -1, Worst = 0;
					for (int32 c = 0; c < NumPalette; ++c)
					{
						if (ColourCounts[c] > 0 && PaletteChroma[c] >= NeutralChroma && ColourCounts[c] > Worst)
						{
							Worst = ColourCounts[c];
							Best = c;
						}
					}
					if (Best >= 0)
					{
						OutCells[Idx] = static_cast<uint8>(Best);
					}
				}
			}
		}
	}
}

void FBeanPalette::DownsampleImageColors(const TArray<FVector3f>& LabPalette, const FColor* Pixels,
	int32 Width, int32 Height, int32 Granularity, const TArray<uint8>& Cells, uint8 EmptyValue,
	TArray<FLinearColor>& OutColors, TArray<bool>& OutHasColor)
{
	const int32 N = Granularity;
	const int32 Count = N * N;
	OutColors.SetNum(Count);
	OutHasColor.SetNum(Count);

	for (int32 i = 0; i < Count; ++i)
	{
		OutColors[i] = FLinearColor::Black;
		OutHasColor[i] = false;
	}

	const int32 NumPalette = LabPalette.Num();
	if (N <= 0 || !Pixels || Width <= 0 || Height <= 0 || NumPalette == 0 || Cells.Num() != Count)
	{
		return;
	}

	// Cap the subsampling so a huge source image stays cheap.
	constexpr int64 MaxSamplesPerBlock = 4096;
	TArray<FColor> Cols;
	Cols.Reserve(4096);

	for (int32 By = 0; By < N; ++By)
	{
		const int64 Ys = FMath::Clamp<int64>((int64)FMath::FloorToDouble((double)By * Height / N), 0, Height);
		const int64 Ye = FMath::Clamp<int64>((int64)FMath::CeilToDouble((double)(By + 1) * Height / N), 0, Height);
		const int64 Ah = Ye - Ys;
		if (Ah <= 0) continue;

		for (int32 Bx = 0; Bx < N; ++Bx)
		{
			const int64 Xs = FMath::Clamp<int64>((int64)FMath::FloorToDouble((double)Bx * Width / N), 0, Width);
			const int64 Xe = FMath::Clamp<int64>((int64)FMath::CeilToDouble((double)(Bx + 1) * Width / N), 0, Width);
			const int64 Aw = Xe - Xs;
			if (Aw <= 0) continue;

			const int32 Idx = By * N + Bx;
			const uint8 CellPalette = Cells[Idx];
			if (CellPalette == EmptyValue || CellPalette >= NumPalette)
			{
				continue; // transparent / will not be placed: leave the hint cell empty
			}

			Cols.Reset();
			if (Aw * Ah <= MaxSamplesPerBlock)
			{
				for (int64 Y = Ys; Y < Ye; ++Y)
				{
					const FColor* Row = Pixels + Y * (int64)Width;
					for (int64 X = Xs; X < Xe; ++X)
					{
						const FColor& Px = Row[X];
						if (Px.A >= 128) Cols.Add(Px);
					}
				}
			}
			else
			{
				const int32 BlockCols = FMath::Clamp<int32>(
					(int32)FMath::CeilToDouble(FMath::Sqrt(MaxSamplesPerBlock * (double)Aw / (double)Ah)), 1, (int32)Aw);
				const int32 BlockRows = FMath::Clamp<int32>(
					(int32)FMath::CeilToDouble(FMath::Sqrt(MaxSamplesPerBlock * (double)Ah / (double)Aw)), 1, (int32)Ah);
				for (int32 R = 0; R < BlockRows; ++R)
				{
					const int64 Y = Ys + (int64)(((double)R + 0.5) * Ah / BlockRows);
					const FColor* Row = Pixels + Y * (int64)Width;
					for (int32 C = 0; C < BlockCols; ++C)
					{
						const int64 X = Xs + (int64)(((double)C + 0.5) * Aw / BlockCols);
						const FColor& Px = Row[X];
						if (Px.A >= 128) Cols.Add(Px);
					}
				}
			}
			if (Cols.Num() == 0)
			{
				continue;
			}

			// Average only the source pixels that map to this block's chosen palette color:
			// a block straddling two colour regions keeps the real colour of the winning side
			// (crisp edge) instead of a muddy blend of both sides.
			int64 Sr = 0, Sg = 0, Sb = 0;
			int64 Sn = 0;
			for (const FColor& C : Cols)
			{
				if (NearestColorIndexLab(LabPalette, C) == CellPalette)
				{
					Sr += C.R;
					Sg += C.G;
					Sb += C.B;
					++Sn;
				}
			}
			if (Sn == 0)
			{
				// No sampled pixel classified to the chosen colour (rare after neutral
				// suppression): fall back to averaging every opaque pixel of the block.
				for (const FColor& C : Cols)
				{
					Sr += C.R;
					Sg += C.G;
					Sb += C.B;
				}
				Sn = Cols.Num();
			}

			const float Inv = 1.0f / (float)Sn;
			OutColors[Idx] = FLinearColor::FromSRGBColor(FColor(
				static_cast<uint8>(FMath::RoundToInt(Sr * Inv)),
				static_cast<uint8>(FMath::RoundToInt(Sg * Inv)),
				static_cast<uint8>(FMath::RoundToInt(Sb * Inv))));
			OutHasColor[Idx] = true;
		}
	}
}
