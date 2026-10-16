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

	/** Color in linear space. Defaulted so a default-constructed entry is never uninitialised:
	 *  the engine reports "StructProperty FBeanPaletteColor::Color is not initialized properly"
	 *  otherwise, and an uninitialised entry would make palette matching non-deterministic. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor Color = FLinearColor::White;
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

	// ---------------------------------------------------------------------------------------
	// Non-square ("feature") variant of QuantizeImage, used by the image converter.
	//
	// QuantizeImage works on a square Granularity x Granularity block grid, which is all the
	// 256x256 canvas ever needed. The canvas can now be landscape or portrait (e.g. 256x192), so
	// the block grid has to be OutWidth x OutHeight blocks instead. This entry point runs exactly
	// the same sampling + K-Means-largest-cluster + neutral-suppression + despeckle pipeline as
	// QuantizeImage (both call the same internal helper), so the on-screen result is identical
	// for a square canvas.
	//
	// LabPalette may be a *subset* of the full palette as long as Cells inside this function map
	// back consistently -- callers that restrict the colours must therefore pass a re-indexed
	// subset palette and translate the returned indices afterwards (see FBeanImageConverter).
	//
	// bPostProcessOnly skips the block pass and only runs the despeckle polish over Cells, which
	// is what the error-diffusion path needs (dithering must not be averaged away again).
	// ---------------------------------------------------------------------------------------
	static void QuantizeFeature(const TArray<FVector3f>& LabPalette, const FColor* Pixels,
		int32 Width, int32 Height, int32 OutWidth, int32 OutHeight, uint8 EmptyValue, TArray<uint8>& Cells,
		bool bPostProcessOnly = false);

	// ---------------------------------------------------------------------------------------
	// Palette restriction ("use at most N colours of the 48").
	//
	// A subset palette cannot be handed to the quantizer as-is, because the quantizer returns
	// indices measured against the array it was given. The subset is therefore reported as a
	// RE-INDEXED palette (SubLab / original-index table) plus Remap, which translates a quantizer
	// result back to the caller's own palette indexing:
	//
	//     const uint8 GameIndex = (Cell == EmptyValue) ? EmptyValue : Remap[Cell];
	//
	// Selection is greedy and content driven: the most frequent nearest-palette colour is taken
	// first (so the biggest flat regions always survive), then repeatedly the colour that removes
	// the most remaining CIE L*a*b* error. Pixels with A < 128 are ignored. When MaxColors <= 0 or
	// >= the palette size, the identity subset is returned so callers need no special case.
	// ---------------------------------------------------------------------------------------
	struct FBeanPaletteSubset
	{
		/** Lab coordinates of the selected colors, re-indexed 0..Num()-1 for the quantizer. */
		TArray<FVector3f> SubLab;
		/** Remap[subsetIndex] = index into the caller's full palette. */
		TArray<uint8> Remap;
		/** Number of selected colors. */
		int32 Num() const { return SubLab.Num(); }
	};

	static void SelectPaletteSubset(const TArray<FVector3f>& LabPalette, const FColor* Pixels,
		int32 Width, int32 Height, int32 MaxColors, FBeanPaletteSubset& OutSubset);

	/**
	 * Bounding box of the meaningful (non-transparent) content of a BGRA8 sRGB image, used by the
	 * import dialog to suggest a crop that keeps the subject. Alpha 8-bit images use their alpha
	 * channel; fully opaque images fall back to "pixels that differ noticeably from the border
	 * colour", so a subject on a flat white/black backdrop is still found instead of the whole
	 * frame. Returns false (and a full-frame rect) when no subject can be told apart, so the UI
	 * can fall back to manual cropping rather than crop at random.
	 */
	static bool AutoCropRect(const FColor* Pixels, int32 Width, int32 Height, int32& OutMinX,
		int32& OutMinY, int32& OutMaxX, int32& OutMaxY);

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
