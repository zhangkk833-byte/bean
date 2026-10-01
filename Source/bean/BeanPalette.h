#pragma once

#include "CoreMinimal.h"
#include "Math/Color.h"
#include "Math/Vector.h"
#include "BeanPalette.generated.h"

/** One entry of the 48-color palette. */
USTRUCT(BlueprintType)
struct FBeanPaletteColor
{
	GENERATED_BODY()

	/** Display name (Chinese, shown in the UI palette bar). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Name;

	/** Color in linear space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor Color;
};

/**
 * Palette helpers: load the 48-color palette from Config/BeanPalette.json,
 * or generate and persist the default palette when the file is missing.
 */
class BEAN_API FBeanPalette
{
public:
	static TArray<FBeanPaletteColor> LoadOrCreateDefault();

	static int32 GetPaletteSize() { return 48; }

	/** Perceptually nearest palette index for a linear-space color (CIE L*a*b* distance). */
	static int32 NearestColorIndex(const TArray<FBeanPaletteColor>& Palette, const FLinearColor& Color);

	/**
	 * Precompute CIE L*a*b* (D65) coordinates for every palette color, using the sRGB
	 * 8-bit value each cube actually displays. sRGB is not perceptually uniform, so
	 * matching in Lab fixes colors that look wrong under plain RGB distance (Q1).
	 */
	static void BuildLabPalette(const TArray<FBeanPaletteColor>& Palette, TArray<FVector3f>& OutLab);

	/** Perceptually nearest palette index for an sRGB-encoded 8-bit color (Lab distance). */
	static int32 NearestColorIndexLab(const TArray<FVector3f>& LabPalette, const FColor& SrgbColor);

	/**
	 * Block quantizer for imported images.
	 *
	 * Maps a Width x Height BGRA8 sRGB image onto a Granularity x Granularity block grid.
	 * Each block's pixels are matched in CIE Lab, split into K=2 clusters (K-Means), and the
	 * larger-weight cluster's weighted real-color mode is snapped to the palette — not a synthetic
	 * centroid — so a block straddling green/brown never invents a blended grey. A chroma-aware
	 * suppression pass also replaces a grey winner on a genuinely colourful block, removing the
	 * grey anti-aliasing halo at colour edges. Optional local edge sharpening / spatial weighting /
	 * despeckle are tunable (see the knobs in the .cpp).
	 *
	 * Pixels are sampled uniformly inside each block's source rectangle: the whole rectangle
	 * when it is small, otherwise a stratified grid capped at ~64 samples so very large images
	 * stay cheap while even a 1-pixel-wide block still gets samples. Transparent pixels
	 * (A < 128) are skipped; a block without opaque pixels becomes EmptyValue.
	 * OutCells is resized to Granularity * Granularity palette indexes.
	 */
	static void QuantizeImage(const TArray<FVector3f>& LabPalette, const FColor* Pixels,
		int32 Width, int32 Height, int32 Granularity, uint8 EmptyValue, TArray<uint8>& OutCells);

	/**
	 * Per-block downsampled "reference" colors of the imported image (NOT palette
	 * quantized). For each block it averages only the source pixels that map to that
	 * block's chosen palette color (Cells[block]), so a block straddling two colour
	 * regions keeps the real colour of the winning side instead of a muddy 50/50 blend
	 * — this keeps the hint layer's edges crisp. Blocks without opaque pixels (or whose
	 * Cells entry is EmptyValue) are marked false in OutHasColor.
	 */
	static void DownsampleImageColors(const TArray<FVector3f>& LabPalette, const FColor* Pixels,
		int32 Width, int32 Height, int32 Granularity, const TArray<uint8>& Cells, uint8 EmptyValue,
		TArray<FLinearColor>& OutColors, TArray<bool>& OutHasColor);

	/** Full path of the palette config file. */
	static FString GetConfigPath();
};
