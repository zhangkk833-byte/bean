#include "BeanPalette.h"

#include "Async/ParallelFor.h"
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
	 * larger-weight cluster, then pick its (weighted) mode 鈥?the palette color the members are
	 * actually closest to 鈥?rather than snapping a synthetic centroid. This avoids inventing a
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

// ===========================================================================================
// Block quantizer core, shared by QuantizeImage (square canvas) and QuantizeFeature (any
// canvas shape). Keeping ONE implementation means the improved image conversion and the legacy
// path can never drift apart.
// ===========================================================================================
namespace
{
	/** Per-step knobs. Tune these to A/B which combination fits your art. */
	struct FBeanBlockQuantizeTuning
	{
		bool  bUseKMeans = true;          // K-Means (K=2) largest cluster instead of a plain vote.
		bool  bSuppressNeutral = true;    // kill the grey "in-between" band at coloured edges.
		bool  bDespeckle = true;          // replace isolated muddy blocks inside coloured regions.
		bool  bGaussianWeight = true;     // weight samples toward the block centre.
		bool  bSaturationWeight = false;  // down-weight desaturated transition pixels (off).
		float SaturationLowStrength = 0.7f;
		float GaussianSigma = 0.35f;
		float SharpeningAmount = 0.5f;    // local unsharp so AA edge pixels pick a side.
		int32 SharpeningRadius = 2;
		bool  bOpaqueWeight = true;
		uint8 OpaqueSolidAlpha = 200;
		float OpaqueLowWeight = 0.12f;
		float OpaqueMidWeight = 0.35f;
		bool  bRequireSolid = true;       // a block must contain a solid pixel to be placed.
		/**
		 * Fraction of a block's sampled pixels that may be transparent before the block is
		 * dropped as a hole.
		 *
		 * bRequireSolid alone only rejects blocks with NO solid pixel, so a thin transparent rim
		 * (a logo's soft border, a sticker outline) still produced a full ring of beads: half of
		 * each rim block was empty and the other half was solid, and the solid side won the colour
		 * vote. Treating a mostly-empty block as a hole keeps the silhouette faithful instead of
		 * growing a bead-wide outline around it. Kept at 0.5 so a block that is at least half
		 * solid still gets its bead (detail is not eroded away).
		 */
		float TransparentFractionLimit = 0.5f;
		int32 MinKMeansSamples = 4;
		int32 KMeansIters = 8;
		int64 MaxSamplesPerBlock = 64;    // ~8x8 samples per block keeps huge images fast.
	};

	/** Quantize one source rectangle [Xs,Xe) x [Ys,Ye) into a single palette index.
	 *  Mirrors the original per-block code exactly: sharpen -> Lab -> weight -> K-Means/weighted
	 *  mode -> neutral suppression. Returns OutEmptyValue when the block holds no usable pixel. */
	uint8 QuantizeCellRegion(const TArray<FVector3f>& LabPalette, const TArray<float>& PaletteChroma,
		const FColor* Pixels, int32 Width, int32 Height, int64 Xs, int64 Ys, int64 Xe, int64 Ye,
		uint8 EmptyValue, const FBeanBlockQuantizeTuning& Knobs,
		TArray<FVector3f>& ScratchLabs, TArray<float>& ScratchWeights,
		TArray<FVector3f>& ScratchSubLabs, TArray<float>& ScratchSubWeights, TArray<float>& ScratchWins)
	{
		const int64 Aw = Xe - Xs;
		const int64 Ah = Ye - Ys;
		if (Aw <= 0 || Ah <= 0) return EmptyValue;

		const double CX = (Xs + Xe - 1) * 0.5;
		const double CY = (Ys + Ye - 1) * 0.5;
		const double HalfX = FMath::Max(1.0, Aw * 0.5);
		const double HalfY = FMath::Max(1.0, Ah * 0.5);

		ScratchLabs.Reset();
		ScratchWeights.Reset();
		bool bAnySolid = false;
		int32 TransparentSamples = 0;
		int32 ExaminedSamples = 0;

		// Sample one source pixel: sharpen, convert to Lab, compute its weight.
		auto SamplePixel = [&](int64 X, int64 Y)
		{
			const FColor& Px = Pixels[Y * (int64)Width + X];
			++ExaminedSamples;
			if (Px.A < 128)
			{
				++TransparentSamples;
				return; // transparent: ignored
			}

			float R = Px.R, G = Px.G, B = Px.B;

			if (Knobs.SharpeningAmount > 0.0f)
			{
				// Local mean of a small neighbourhood, then unsharp: p' = p + k*(p - mean).
				// Transition pixels land closer to one side, so fewer of them are ambiguous.
				int64 SR = 0, SG = 0, SB = 0;
				int32 Nn = 0;
				for (int32 dy = -Knobs.SharpeningRadius; dy <= Knobs.SharpeningRadius; ++dy)
				{
					const int64 yy = FMath::Clamp<int64>(Y + dy, 0, Height - 1);
					const FColor* Row = Pixels + yy * (int64)Width;
					for (int32 dx = -Knobs.SharpeningRadius; dx <= Knobs.SharpeningRadius; ++dx)
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
					R = FMath::Clamp(R + Knobs.SharpeningAmount * (R - SR * Inv), 0.0f, 255.0f);
					G = FMath::Clamp(G + Knobs.SharpeningAmount * (G - SG * Inv), 0.0f, 255.0f);
					B = FMath::Clamp(B + Knobs.SharpeningAmount * (B - SB * Inv), 0.0f, 255.0f);
				}
			}

			// Step 3: still match in CIE Lab so colour fidelity stays high.
			const FVector3f Lab = LabFromSRGB(R / 255.0f, G / 255.0f, B / 255.0f);

			float W = 1.0f;
			if (Knobs.bGaussianWeight)
			{
				const float FX = (float)((X - CX) / HalfX);   // ~[-1,1]
				const float FY = (float)((Y - CY) / HalfY);
				W = (float)FMath::Exp(-(FX * FX + FY * FY) / (2.0f * Knobs.GaussianSigma * Knobs.GaussianSigma));
			}
			if (Knobs.bSaturationWeight)
			{
				const float Chroma = FMath::Sqrt(Lab.Y * Lab.Y + Lab.Z * Lab.Z);
				const float Sat = FMath::Clamp(Chroma / 128.0f, 0.0f, 1.0f);
				W *= (1.0f - Knobs.SaturationLowStrength * (1.0f - Sat));
			}
			if (Knobs.bOpaqueWeight)
			{
				if (Px.A >= Knobs.OpaqueSolidAlpha)
				{
					bAnySolid = true;
				}
				else
				{
					// Fuzzy anti-aliased rim: barely contributes, so the solid colour wins.
					W *= (Px.A >= 160) ? Knobs.OpaqueMidWeight : Knobs.OpaqueLowWeight;
				}
			}

			ScratchLabs.Add(Lab);
			ScratchWeights.Add(W);
		};

		if (Aw * Ah <= Knobs.MaxSamplesPerBlock)
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
				(int32)FMath::CeilToDouble(FMath::Sqrt(Knobs.MaxSamplesPerBlock * (double)Aw / (double)Ah)), 1, (int32)Aw);
			const int32 Rows = FMath::Clamp<int32>(
				(int32)FMath::CeilToDouble(FMath::Sqrt(Knobs.MaxSamplesPerBlock * (double)Ah / (double)Aw)), 1, (int32)Ah);
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

		const int32 SampleCount = ScratchLabs.Num();
		if (SampleCount == 0) return EmptyValue;

		// Primarily-transparent block: hole, so a thin transparent rim does not turn into a ring
		// of beads around the subject. Checked against every examined pixel (not just the solid
		// ones) so it is a true coverage fraction.
		if (ExaminedSamples > 0
			&& (float)TransparentSamples > Knobs.TransparentFractionLimit * (float)ExaminedSamples)
		{
			return EmptyValue;
		}

		if (Knobs.bRequireSolid && !bAnySolid)
		{
			// Only fuzzy anti-aliased rim here (no solid pixel): drop it so a dirty ring
			// of blocks does not appear around the sticker's transparent silhouette.
			return EmptyValue;
		}

		if (Knobs.bUseKMeans && SampleCount >= Knobs.MinKMeansSamples)
		{
			// Step 2: largest K=2 cluster, then its weighted real-color mode (not a centroid).
			return static_cast<uint8>(KMeansLargestPaletteIndex(
				LabPalette, PaletteChroma, ScratchLabs, ScratchWeights, Knobs.KMeansIters,
				Knobs.bSuppressNeutral, ScratchSubLabs, ScratchSubWeights, ScratchWins));
		}

		// Fallback / base: weighted mode of each sample's nearest palette colour.
		return static_cast<uint8>(WeightedModePaletteIndex(
			LabPalette, PaletteChroma, ScratchLabs, ScratchWeights, Knobs.bSuppressNeutral, ScratchWins));
	}

	/** Despeckle pass over a W x H cell buffer: an isolated neutral cell entirely surrounded by
	 *  colourful neighbours is usually a leftover grey transition band -> take the dominant
	 *  neighbour colour. Identical to the original square-grid pass, generalised to W != H. */
	void DespeckleCells(const TArray<float>& PaletteChroma, uint8 EmptyValue, int32 W, int32 H, TArray<uint8>& Cells)
	{
		if (W <= 2 || H <= 2 || Cells.Num() < W * H) return;
		const int32 NumPalette = PaletteChroma.Num();
		const TArray<uint8> Original = Cells;

		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const int32 Idx = Y * W + X;
				const uint8 Cell = Original[Idx];
				if (Cell == EmptyValue || Cell >= NumPalette) continue;
				if (PaletteChroma[Cell] >= NeutralChroma) continue; // not neutral

				int32 ColourCounts[256] = {}; // palette index (<= 48) -> neighbour count
				int32 ColourfulNeighbours = 0;
				for (int32 dy = -1; dy <= 1; ++dy)
				{
					const int32 Ny = Y + dy;
					if (Ny < 0 || Ny >= H) continue;
					for (int32 dx = -1; dx <= 1; ++dx)
					{
						if (dx == 0 && dy == 0) continue;
						const int32 Nx = X + dx;
						if (Nx < 0 || Nx >= W) continue;
						const uint8 Nc = Original[Ny * W + Nx];
						if (Nc == EmptyValue || Nc >= NumPalette) continue;
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
						Cells[Idx] = static_cast<uint8>(Best);
					}
				}
			}
		}
	}
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
	const FBeanBlockQuantizeTuning Knobs;

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

		for (int32 Bx = 0; Bx < N; ++Bx)
		{
			const int64 Xs = FMath::Clamp<int64>((int64)FMath::FloorToDouble((double)Bx * Width / N), 0, Width);
			const int64 Xe = FMath::Clamp<int64>((int64)FMath::CeilToDouble((double)(Bx + 1) * Width / N), 0, Width);

			OutCells[By * N + Bx] = QuantizeCellRegion(LabPalette, PaletteChroma, Pixels, Width, Height,
				Xs, Ys, Xe, Ye, EmptyValue, Knobs, Labs, Weights, SubLabs, SubWeights, Wins);
		}
	}
	// (QuantizeImage keeps its own serial loop: it is only used by the square-canvas tests.)

	// Optional despeckle: an isolated neutral block entirely surrounded by colourful neighbours
	// is usually a leftover grey transition band -- replace it with the dominant neighbour colour.
	if (Knobs.bDespeckle)
	{
		DespeckleCells(PaletteChroma, EmptyValue, N, N, OutCells);
	}
}

void FBeanPalette::QuantizeFeature(const TArray<FVector3f>& LabPalette, const FColor* Pixels,
	int32 Width, int32 Height, int32 OutWidth, int32 OutHeight, uint8 EmptyValue, TArray<uint8>& Cells,
	bool bPostProcessOnly)
{
	// Any canvas shape, same pipeline as QuantizeImage. Used by FBeanImageConverter so that
	// landscape / portrait canvases quantize exactly like the square one always did.
	const int32 W = OutWidth;
	const int32 H = OutHeight;
	const int32 Count = W * H;

	if (W <= 0 || H <= 0 || !Pixels || Width <= 0 || Height <= 0 || LabPalette.Num() == 0)
	{
		Cells.Init(EmptyValue, FMath::Max(0, Count));
		return;
	}

	Cells.SetNum(Count);
	const int32 NumPalette = LabPalette.Num();
	const FBeanBlockQuantizeTuning Knobs;

	TArray<float> PaletteChroma;
	PaletteChroma.SetNum(NumPalette);
	for (int32 i = 0; i < NumPalette; ++i)
	{
		PaletteChroma[i] = LabChroma(LabPalette[i]);
	}

	if (!bPostProcessOnly)
	{
		// Cells are independent, so the block loop is split by output row across worker threads;
		// each row keeps its own scratch buffers (they cannot be shared). A 128x128 canvas over a
		// 6000x6000 photo spent ~240 ms here, nearly all of the import.
		ParallelFor(H, [&](int32 By)
		{
			TArray<FVector3f> Labs, SubLabs;
			TArray<float>     Weights, SubWeights, Wins;
			Labs.Reserve(160);
			SubLabs.Reserve(160);
			Weights.Reserve(160);
			SubWeights.Reserve(160);
			Wins.SetNum(NumPalette);

			const int64 Ys = FMath::Clamp<int64>((int64)FMath::FloorToDouble((double)By * Height / H), 0, Height);
			const int64 Ye = FMath::Clamp<int64>((int64)FMath::CeilToDouble((double)(By + 1) * Height / H), 0, Height);

			for (int32 Bx = 0; Bx < W; ++Bx)
			{
				const int64 Xs = FMath::Clamp<int64>((int64)FMath::FloorToDouble((double)Bx * Width / W), 0, Width);
				const int64 Xe = FMath::Clamp<int64>((int64)FMath::CeilToDouble((double)(Bx + 1) * Width / W), 0, Width);

				Cells[By * W + Bx] = QuantizeCellRegion(LabPalette, PaletteChroma, Pixels, Width, Height,
					Xs, Ys, Xe, Ye, EmptyValue, Knobs, Labs, Weights, SubLabs, SubWeights, Wins);
			}
		});
	}

	if (Knobs.bDespeckle)
	{
		DespeckleCells(PaletteChroma, EmptyValue, W, H, Cells);
	}
}


void FBeanPalette::SelectPaletteSubset(const TArray<FVector3f>& LabPalette, const FColor* Pixels,
	int32 Width, int32 Height, int32 MaxColors, FBeanPaletteSubset& OutSubset)
{
	OutSubset.SubLab.Reset();
	OutSubset.Remap.Reset();

	const int32 NumPalette = LabPalette.Num();
	if (NumPalette == 0)
	{
		return;
	}

	// Whole palette requested (or the array already is the whole palette): identity subset, so
	// callers never need a special case and quantizer indices stay valid as-is.
	const bool bIdentity = MaxColors <= 0 || MaxColors >= NumPalette;
	if (bIdentity || !Pixels || Width <= 0 || Height <= 0)
	{
		OutSubset.SubLab = LabPalette;
		OutSubset.Remap.SetNum(NumPalette);
		for (int32 i = 0; i < NumPalette; ++i)
		{
			OutSubset.Remap[i] = static_cast<uint8>(i);
		}
		return;
	}

	// Sample budget. A 1M-pixel sample is plenty to pick colours, and keeps the cost independent
	// of how large the imported image is (an 8192x8192 photo must not stall the import).
	constexpr int64 MaxSamples = 1 << 20;
	const int64 Total = (int64)Width * Height;
	const int32 Stride = (int32)FMath::Max<int64>(1, Total / MaxSamples);

	struct FSample
	{
		FVector3f Lab;
		int32 Nearest = -1;
	};
	TArray<FSample> Samples;
	Samples.Reserve((int32)FMath::Min<int64>(Total / Stride + 1, MaxSamples + 1));

	TArray<int32> Frequency;
	Frequency.Init(0, NumPalette);

	for (int64 i = 0; i < Total; i += Stride)
	{
		const FColor& Px = Pixels[i];
		if (Px.A < 128) continue;
		const FVector3f Lab = LabFromSRGB(Px.R / 255.0f, Px.G / 255.0f, Px.B / 255.0f);
		FSample S;
		S.Lab = Lab;
		S.Nearest = NearestLabPaletteIndex(LabPalette, Lab);
		++Frequency[S.Nearest];
		Samples.Add(S);
	}

	if (Samples.Num() == 0)
	{
		// Nothing usable in the image: keep the full palette so the quantizer still works.
		OutSubset.SubLab = LabPalette;
		OutSubset.Remap.SetNum(NumPalette);
		for (int32 i = 0; i < NumPalette; ++i)
		{
			OutSubset.Remap[i] = static_cast<uint8>(i);
		}
		return;
	}

	// Total Lab-space error left if the picture were drawn with only the already-chosen colours.
	// -1 marks a sample that is NOT yet represented; it only becomes represented on the pass where
	// its own nearest colour is taken, so it is counted exactly once.
	TArray<double> Residual;
	Residual.Init(-1.0, Samples.Num());

	const int32 Want = FMath::Clamp(MaxColors, 1, NumPalette);
	OutSubset.SubLab.Reserve(Want);
	OutSubset.Remap.Reserve(Want);

	for (int32 Step = 0; Step < Want; ++Step)
	{
		int32 Best = -1;
		double BestGain = -1.0;

		for (int32 c = 0; c < NumPalette; ++c)
		{
			// The first colour is the most frequent one: the large flat regions of a picture must
			// always survive, whatever the error metric thinks of them.
			if (Step == 0 && Frequency[c] <= 0) continue;

			double Gain = 0.0;
			for (int32 i = 0; i < Samples.Num(); ++i)
			{
				if (Residual[i] < 0.0 || Samples[i].Nearest != c) continue;
				const FVector3f& L0 = Samples[i].Lab;
				const float d0 = L0.X - LabPalette[c].X;
				const float d1 = L0.Y - LabPalette[c].Y;
				const float d2 = L0.Z - LabPalette[c].Z;
				Gain += d0 * d0 + d1 * d1 + d2 * d2;
			}
			if (Gain > BestGain)
			{
				BestGain = Gain;
				Best = c;
			}
		}

		if (Best < 0)
		{
			break; // no pixel left that a new colour could help
		}

		OutSubset.SubLab.Add(LabPalette[Best]);
		OutSubset.Remap.Add(static_cast<uint8>(Best));

		// Settle the newly represented pixels: their remaining error is held against the palette.
		for (int32 i = 0; i < Samples.Num(); ++i)
		{
			if (Residual[i] < 0.0 || Samples[i].Nearest != Best) continue;
			const FVector3f& L0 = Samples[i].Lab;
			const float d0 = L0.X - LabPalette[Best].X;
			const float d1 = L0.Y - LabPalette[Best].Y;
			const float d2 = L0.Z - LabPalette[Best].Z;
			Residual[i] = d0 * d0 + d1 * d1 + d2 * d2;
		}

		if (BestGain <= 0.0)
		{
			break; // exact matches only; more colours would add nothing
		}
	}
}

bool FBeanPalette::AutoCropRect(const FColor* Pixels, int32 Width, int32 Height, int32& OutMinX,
	int32& OutMinY, int32& OutMaxX, int32& OutMaxY)
{
	OutMinX = 0;
	OutMinY = 0;
	OutMaxX = FMath::Max(0, Width - 1);
	OutMaxY = FMath::Max(0, Height - 1);

	if (!Pixels || Width <= 0 || Height <= 0)
	{
		return false;
	}

	const int64 Total = (int64)Width * Height;
	int64 OpaqueCount = 0;
	int32 MinX = Width, MinY = Height, MaxX = -1, MaxY = -1;
	for (int64 i = 0; i < Total; ++i)
	{
		if (Pixels[i].A < 128) continue;
		++OpaqueCount;
		const int32 X = (int32)(i % Width);
		const int32 Y = (int32)(i / Width);
		MinX = FMath::Min(MinX, X);
		MaxX = FMath::Max(MaxX, X);
		MinY = FMath::Min(MinY, Y);
		MaxY = FMath::Max(MaxY, Y);
	}

	// Real transparency (a PNG sticker / cut-out): the alpha channel alone already tells us where
	// the subject is, which is the most reliable signal available.
	if (OpaqueCount < Total)
	{
		if (OpaqueCount == 0)
		{
			return false; // nothing visible at all
		}
		OutMinX = MinX;
		OutMinY = MinY;
		OutMaxX = MaxX;
		OutMaxY = MaxY;
		return (MaxX - MinX + 1) < Width || (MaxY - MinY + 1) < Height;
	}

	// Fully opaque image (photo / JPEG / BMP / opaque screenshot): a subject framed by a flat
	// backdrop. Threshold in CIE L*a*b* so a JPEG's block noise around a sharp edge, and a faint
	// shadow, do not count as subject — a plain RGB threshold would crop far too generously.
	const FColor Corners[4] = {
		Pixels[0],
		Pixels[(int64)(Width - 1)],
		Pixels[(int64)(Height - 1) * Width],
		Pixels[(int64)(Height - 1) * Width + (Width - 1)],
	};

	bool bCornersAgree = true;
	for (int32 c = 1; c < 4 && bCornersAgree; ++c)
	{
		if (Corners[0].R != Corners[c].R || Corners[0].G != Corners[c].G || Corners[0].B != Corners[c].B)
		{
			bCornersAgree = false;
		}
	}

	// No usable backdrop (the corners disagree, i.e. a photo with content right into the corners):
	// deliberately report "cannot tell" so the caller falls back to manual cropping instead of
	// cutting the subject off at random.
	if (!bCornersAgree)
	{
		return false;
	}

	const FVector3f BgLab = LabFromSRGB(Corners[0].R / 255.0f, Corners[0].G / 255.0f, Corners[0].B / 255.0f);
	constexpr float MinLabDeltaSq = 30.0f; // 8-bit RGB delta ~ 14, i.e. clearly "another colour"

	int64 DiffCount = 0;
	MinX = Width; MinY = Height; MaxX = -1; MaxY = -1;
	for (int64 i = 0; i < Total; ++i)
	{
		const FColor& Px = Pixels[i];
		if (Px.A < 128) continue;
		const FVector3f Lab = LabFromSRGB(Px.R / 255.0f, Px.G / 255.0f, Px.B / 255.0f);
		const float d0 = Lab.X - BgLab.X;
		const float d1 = Lab.Y - BgLab.Y;
		const float d2 = Lab.Z - BgLab.Z;
		if ((d0 * d0 + d1 * d1 + d2 * d2) <= MinLabDeltaSq) continue;

		++DiffCount;
		const int32 X = (int32)(i % Width);
		const int32 Y = (int32)(i / Width);
		MinX = FMath::Min(MinX, X);
		MaxX = FMath::Max(MaxX, X);
		MinY = FMath::Min(MinY, Y);
		MaxY = FMath::Max(MaxY, Y);
	}

	// Nothing distinguishable from the backdrop, or the "subject" covers the entire frame: let the
	// dialog start from the whole picture and let the player crop by hand.
	if (DiffCount == 0 || MaxX < 0)
	{
		return false;
	}
	if ((MaxX - MinX + 1) >= Width && (MaxY - MinY + 1) >= Height)
	{
		OutMinX = 0;
		OutMinY = 0;
		OutMaxX = Width - 1;
		OutMaxY = Height - 1;
		return false;
	}

	OutMinX = MinX;
	OutMinY = MinY;
	OutMaxX = MaxX;
	OutMaxY = MaxY;
	return true;
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
